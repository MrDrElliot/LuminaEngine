#include "RuntimePCH.h"
#include "WorldSaveState.h"

#include "SaveGameArchive.h"
#include "SaveGameComponent.h"
#include "Core/Object/Class.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "World/ECS/CommandBus.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina::SaveGame
{
    namespace
    {
        constexpr uint32 WorldStateMagic   = 0x5357534C;
        constexpr uint32 WorldStateVersion = 1;

        // Version plus one per entity slot, so a zero is a slot the level never filled.
        struct FLevelEntities
        {
            TVector<uint32> VersionPlusOne;
        };

        enum class EMatch : uint8
        {
            LevelId,
            SaveKey,
            NamePath,
            Respawn,
        };

        struct FRecord
        {
            ECS::FEntity Saved;
            EMatch       Match = EMatch::LevelId;
            FString      Key;
            ECS::FEntity SavedParent;
        };

        struct FComponentEntry
        {
            ECS::FSparseSet* Set    = nullptr;
            CStruct*         Struct = nullptr;
        };

        template<typename TBody>
        void WriteSized(FArchive& Ar, TBody&& Body)
        {
            const int64 SizePosition = Ar.Tell();
            int64 Size = 0;
            Ar << Size;
            const int64 Start = Ar.Tell();
            Body();
            const int64 End = Ar.Tell();
            Size = End - Start;
            Ar.Seek(SizePosition);
            Ar << Size;
            Ar.Seek(End);
        }

        template<typename TBody>
        void WriteCounted(FArchive& Ar, TBody&& Body)
        {
            const int64 CountPosition = Ar.Tell();
            uint32 Count = 0;
            Ar << Count;
            Count = Body();
            const int64 End = Ar.Tell();
            Ar.Seek(CountPosition);
            Ar << Count;
            Ar.Seek(End);
        }

        bool ReadCount(FArchive& Ar, uint32& Count, const char* What)
        {
            Ar << Count;
            if (Ar.HasError() || !Ar.CanHoldCount(Count))
            {
                LOG_ERROR("Save game: {} count {} is more than the file holds, so it is damaged.", What, Count);
                Ar.SetHasError(true);
                return false;
            }
            return true;
        }

        void SerializeTransform(FArchive& Ar, FTransform& Transform)
        {
            FVector3 Location = Transform.GetLocation();
            FQuat    Rotation = Transform.GetRotation();
            FVector3 Scale    = Transform.GetScale();
            Ar << Location << Rotation << Scale;
            if (Ar.IsReading())
            {
                Transform.SetLocation(Location);
                Transform.SetRotation(Rotation);
                Transform.SetScale(Scale);
            }
        }

        bool IsScriptStruct(const CStruct* Struct)
        {
            return Struct == SEntityScriptComponent::StaticStruct();
        }

        // A script whose class has a SaveGame field, counted by class so a load can tell two of the same apart.
        template<typename TFunc>
        void ForEachSavedScript(ECS::FRegistry& Registry, ECS::FEntity Entity, TFunc&& Func)
        {
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return;
            }
            THashMap<const CClass*, uint32> Ordinals;
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script == nullptr || !HasSaveGameProperties(Script->GetClass()))
                {
                    continue;
                }
                Func(Script, Ordinals[Script->GetClass()]++);
            }
        }

        CEntityScript* FindScript(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* Class, uint32 Ordinal)
        {
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return nullptr;
            }
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() == Class && Ordinal-- == 0)
                {
                    return Script;
                }
            }
            return nullptr;
        }

        TVector<FComponentEntry> GatherComponents(ECS::FRegistry& Registry, bool bSaveGameOnly)
        {
            TVector<FComponentEntry> Entries;
            for (ECS::FSparseSet* Set : Registry.GetActiveStorages())
            {
                if (Set->IsEmpty())
                {
                    continue;
                }
                CStruct* Struct = FindComponentStructByTypeId(Set->GetTypeInfo().TypeID);
                if (Struct == nullptr || Struct->GetComponentOps() == nullptr)
                {
                    continue;
                }
                if (bSaveGameOnly && (IsScriptStruct(Struct) || !HasSaveGameProperties(Struct)))
                {
                    continue;
                }
                Entries.push_back({ Set, Struct });
            }
            return Entries;
        }

        bool IsRespawnable(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const SSaveGameComponent* Marker = Registry.TryGet<SSaveGameComponent>(Entity);
            return Marker != nullptr && Marker->bRespawn && Marker->SaveKey.IsNone() && !IsLevelEntity(Registry, Entity);
        }

        // Roots only, since a respawned entity brings its whole subtree.
        TVector<ECS::FEntity> GatherRespawnRoots(ECS::FRegistry& Registry)
        {
            TVector<ECS::FEntity> Roots;
            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            for (ECS::FEntity Entity : Registry.View<SSaveGameComponent>())
            {
                if (!IsRespawnable(Registry, Entity))
                {
                    continue;
                }
                bool bUnderAnother = false;
                for (ECS::FEntity Up = Hierarchy.GetParent(Entity); !Up.IsNull(); Up = Hierarchy.GetParent(Up))
                {
                    if (IsRespawnable(Registry, Up))
                    {
                        bUnderAnother = true;
                        break;
                    }
                }
                if (!bUnderAnother)
                {
                    Roots.push_back(Entity);
                }
            }
            return Roots;
        }

        FName EntityName(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const SNameComponent* Name = Registry.TryGet<SNameComponent>(Entity);
            return Name != nullptr ? Name->Name : FName();
        }

        // Earlier same-named siblings, or for a root the same-named roots with a lower slot, which is creation order.
        uint32 NameOrdinal(ECS::FRegistry& Registry, ECS::FEntity Entity, const FName& Name)
        {
            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            const ECS::FEntity Parent = Hierarchy.GetParent(Entity);
            uint32 Ordinal = 0;
            if (!Parent.IsNull())
            {
                for (ECS::FEntity Sibling = Hierarchy.GetFirstChild(Parent); !Sibling.IsNull() && Sibling != Entity; Sibling = Hierarchy.GetNextSibling(Sibling))
                {
                    Ordinal += EntityName(Registry, Sibling) == Name ? 1u : 0u;
                }
                return Ordinal;
            }
            for (ECS::FEntity Other : Registry.View<SNameComponent>())
            {
                if (Other.GetIndex() < Entity.GetIndex() && Hierarchy.GetParent(Other).IsNull() && EntityName(Registry, Other) == Name)
                {
                    ++Ordinal;
                }
            }
            return Ordinal;
        }

        class FEntityMatcher
        {
        public:

            explicit FEntityMatcher(ECS::FRegistry& InRegistry)
                : Registry(InRegistry)
            {}

            ECS::FEntity Find(const FRecord& Record)
            {
                switch (Record.Match)
                {
                case EMatch::LevelId:
                    return Registry.IsValid(Record.Saved) && IsLevelEntity(Registry, Record.Saved) ? Record.Saved : ECS::NullEntity;
                case EMatch::SaveKey:
                    {
                        BuildKeys();
                        const auto Found = ByKey.find(Record.Key);
                        return Found != ByKey.end() ? Found->second : ECS::NullEntity;
                    }
                case EMatch::NamePath:
                    {
                        BuildPaths();
                        const auto Found = ByPath.find(Record.Key);
                        return Found != ByPath.end() ? Found->second : ECS::NullEntity;
                    }
                default:
                    return ECS::NullEntity;
                }
            }

        private:

            void BuildKeys()
            {
                if (bKeysBuilt)
                {
                    return;
                }
                bKeysBuilt = true;
                for (ECS::FEntity Entity : Registry.View<SSaveGameComponent>())
                {
                    const FName& Key = Registry.Get<SSaveGameComponent>(Entity).SaveKey;
                    if (!Key.IsNone())
                    {
                        ByKey.emplace(FString(Key.c_str()), Entity);
                    }
                }
            }

            void BuildPaths()
            {
                if (bPathsBuilt)
                {
                    return;
                }
                bPathsBuilt = true;
                for (ECS::FEntity Entity : Registry.View<SNameComponent>())
                {
                    ByPath.emplace(GetEntityNamePath(Registry, Entity), Entity);
                }
            }

            ECS::FRegistry&              Registry;
            THashMap<FString, ECS::FEntity> ByKey;
            THashMap<FString, ECS::FEntity> ByPath;
            bool                         bKeysBuilt  = false;
            bool                         bPathsBuilt = false;
        };

        void WriteSaveGameBlocks(FArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity,
            const TVector<FComponentEntry>& SaveGameComponents, ESaveGameProperties Which)
        {
            WriteCounted(Ar, [&]
            {
                uint32 Count = 0;
                for (const FComponentEntry& Entry : SaveGameComponents)
                {
                    if (!Entry.Set->Contains(Entity))
                    {
                        continue;
                    }
                    FName Name = Entry.Struct->GetName();
                    Ar << Name;
                    WriteSized(Ar, [&] { SerializeProperties(Ar, Entry.Struct, Entry.Set->GetRaw(Entity), Which); });
                    ++Count;
                }
                return Count;
            });

            WriteCounted(Ar, [&]
            {
                uint32 Count = 0;
                ForEachSavedScript(Registry, Entity, [&](CEntityScript* Script, uint32 Ordinal)
                {
                    FName ClassName = Script->GetClass()->GetName();
                    Ar << ClassName << Ordinal;
                    WriteSized(Ar, [&] { SerializeProperties(Ar, Script->GetClass(), Script, Which); });
                    ++Count;
                });
                return Count;
            });
        }

        void ReadSaveGameBlocks(FArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity, bool bAttachMissingScripts,
            TVector<TStrongObjectPtr<CEntityScript>>& OutRestoredScripts)
        {
            uint32 ComponentCount = 0;
            if (!ReadCount(Ar, ComponentCount, "component"))
            {
                return;
            }
            for (uint32 Index = 0; Index < ComponentCount && !Ar.HasError(); ++Index)
            {
                FName StructName;
                int64 Size = 0;
                Ar << StructName << Size;
                const int64 Start = Ar.Tell();

                CStruct* Struct = FindComponentStruct(StructName.c_str());
                const FComponentOps* Ops = Struct != nullptr ? Struct->GetComponentOps() : nullptr;
                void* Data = Ops != nullptr ? Ops->Emplace(Registry, Entity) : nullptr;
                if (Data != nullptr)
                {
                    SerializeProperties(Ar, Struct, Data, ESaveGameProperties::SaveGame);
                    Ops->Patch(Registry, Entity);
                }
                else
                {
                    LOG_WARN("Save game: component '{}' is gone, so its saved fields are skipped.", StructName.c_str());
                }
                Ar.Seek(Start + Size);
            }

            uint32 ScriptCount = 0;
            if (!ReadCount(Ar, ScriptCount, "script"))
            {
                return;
            }
            for (uint32 Index = 0; Index < ScriptCount && !Ar.HasError(); ++Index)
            {
                FName ClassName;
                uint32 Ordinal = 0;
                int64 Size = 0;
                Ar << ClassName << Ordinal << Size;
                const int64 Start = Ar.Tell();

                CClass* Class = FScriptableRegistry::ResolveClass(ClassName);
                CEntityScript* Script = Class != nullptr ? FindScript(Registry, Entity, Class, Ordinal) : nullptr;
                if (Script == nullptr && Class != nullptr && bAttachMissingScripts)
                {
                    Script = EntityScripts::Attach(Registry, Entity, Class);
                }
                if (Script != nullptr)
                {
                    SerializeProperties(Ar, Class, Script, ESaveGameProperties::SaveGame);
                    OutRestoredScripts.push_back(Script);
                }
                else
                {
                    LOG_WARN("Save game: script '{}' could not be found or attached, so its saved fields are skipped.", ClassName.c_str());
                }
                Ar.Seek(Start + Size);
            }
        }

        void NotifySaving(ECS::FRegistry& Registry)
        {
            TVector<TStrongObjectPtr<CEntityScript>> Saving;
            for (ECS::FEntity Entity : Registry.View<SEntityScriptComponent>())
            {
                ForEachSavedScript(Registry, Entity, [&](CEntityScript* Script, uint32) { Saving.push_back(Script); });
            }
            for (const TStrongObjectPtr<CEntityScript>& Script : Saving)
            {
                Script->OnSaving();
            }
        }

        bool WriteRegistry(FSaveGameArchive& Ar, ECS::FRegistry& Registry)
        {
            NotifySaving(Registry);

            const TVector<FComponentEntry> AllComponents      = GatherComponents(Registry, false);
            const TVector<FComponentEntry> SaveGameComponents = GatherComponents(Registry, true);
            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();

            TVector<FRecord> Records;
            THashSet<ECS::FEntity> Covered;

            for (ECS::FEntity Root : GatherRespawnRoots(Registry))
            {
                auto AddRespawn = [&](ECS::FEntity Entity)
                {
                    Records.push_back({ Entity, EMatch::Respawn, FString(), Hierarchy.GetParent(Entity) });
                    Covered.insert(Entity);
                };
                AddRespawn(Root);
                Hierarchy.ForEachDescendant(Root, AddRespawn);
            }

            uint32 Unmatchable = 0;
            Registry.ForEachEntity([&](ECS::FEntity Entity)
            {
                if (Covered.contains(Entity))
                {
                    return;
                }
                const SSaveGameComponent* Marker = Registry.TryGet<SSaveGameComponent>(Entity);
                bool bHasData = Marker != nullptr;
                for (size_t Index = 0; !bHasData && Index < SaveGameComponents.size(); ++Index)
                {
                    bHasData = SaveGameComponents[Index].Set->Contains(Entity);
                }
                if (!bHasData)
                {
                    ForEachSavedScript(Registry, Entity, [&](CEntityScript*, uint32) { bHasData = true; });
                }
                if (!bHasData)
                {
                    return;
                }

                FRecord Record;
                Record.Saved = Entity;
                if (Marker != nullptr && !Marker->SaveKey.IsNone())
                {
                    Record.Match = EMatch::SaveKey;
                    Record.Key   = Marker->SaveKey.c_str();
                }
                else if (IsLevelEntity(Registry, Entity))
                {
                    Record.Match = EMatch::LevelId;
                }
                else if (!EntityName(Registry, Entity).IsNone())
                {
                    Record.Match = EMatch::NamePath;
                    Record.Key   = GetEntityNamePath(Registry, Entity);
                }
                else
                {
                    ++Unmatchable;
                    return;
                }
                Records.push_back(Move(Record));
            });

            if (Unmatchable > 0)
            {
                LOG_WARN("Save game: {} runtime entities with SaveGame data have no name, no SaveKey and no SSaveGameComponent, so a load could not find them and they were left out.", Unmatchable);
            }

            uint32 Magic = WorldStateMagic;
            uint32 Version = WorldStateVersion;
            Ar << Magic << Version;

            uint32 RecordCount = (uint32)Records.size();
            Ar << RecordCount;
            for (FRecord& Record : Records)
            {
                uint8 Match = (uint8)Record.Match;
                Ar << Record.Saved.Value << Match;
                if (Record.Match == EMatch::SaveKey || Record.Match == EMatch::NamePath)
                {
                    Ar << Record.Key;
                }
                if (Record.Match == EMatch::Respawn)
                {
                    Ar << Record.SavedParent.Value;
                }
            }

            TVector<ECS::FEntity> Destroyed;
            if (const FLevelEntities* Level = Registry.Ctx().Find<FLevelEntities>())
            {
                for (uint32 Index = 0; Index < (uint32)Level->VersionPlusOne.size(); ++Index)
                {
                    const uint32 Stored = Level->VersionPlusOne[Index];
                    const ECS::FEntity Entity(Index, Stored - 1u);
                    if (Stored != 0 && !Registry.IsValid(Entity))
                    {
                        Destroyed.push_back(Entity);
                    }
                }
            }
            uint32 DestroyedCount = (uint32)Destroyed.size();
            Ar << DestroyedCount;
            for (ECS::FEntity& Entity : Destroyed)
            {
                Ar << Entity.Value;
            }

            for (const FRecord& Record : Records)
            {
                const ECS::FEntity Entity = Record.Saved;
                WriteSized(Ar, [&]
                {
                    if (Record.Match == EMatch::Respawn)
                    {
                        WriteCounted(Ar, [&]
                        {
                            uint32 Count = 0;
                            THashSet<CStruct*> Written;
                            for (const FComponentEntry& Entry : AllComponents)
                            {
                                if (!Entry.Set->Contains(Entity) || !Written.insert(Entry.Struct).second)
                                {
                                    continue;
                                }
                                FName Name = Entry.Struct->GetName();
                                Ar << Name;
                                WriteSized(Ar, [&] { Entry.Struct->SerializeTaggedProperties(Ar, Entry.Set->GetRaw(Entity)); });
                                ++Count;
                            }
                            return Count;
                        });
                        WriteSaveGameBlocks(Ar, Registry, Entity, SaveGameComponents, ESaveGameProperties::SaveGameOnly);
                        return;
                    }

                    const SSaveGameComponent* Marker = Registry.TryGet<SSaveGameComponent>(Entity);
                    STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
                    bool bTransform = Marker != nullptr && Marker->bSaveTransform && Transform != nullptr;
                    Ar << bTransform;
                    if (bTransform)
                    {
                        SerializeTransform(Ar, Transform->LocalTransform);
                    }
                    WriteSaveGameBlocks(Ar, Registry, Entity, SaveGameComponents, ESaveGameProperties::SaveGame);
                });
            }
            return !Ar.HasError();
        }

        bool ReadRegistry(FSaveGameArchive& Ar, ECS::FRegistry& Registry)
        {
            if (ECS::FCommandBus::ShouldDefer())
            {
                LOG_ERROR("Save game: a world can only be restored from the game thread outside a parallel pass.");
                return false;
            }

            uint32 Magic = 0;
            uint32 Version = 0;
            Ar << Magic << Version;
            if (Magic != WorldStateMagic || Version == 0 || Version > WorldStateVersion)
            {
                LOG_ERROR("Save game: the world state is not one this engine can read (magic {:x}, version {}).", Magic, Version);
                Ar.SetHasError(true);
                return false;
            }

            uint32 RecordCount = 0;
            if (!ReadCount(Ar, RecordCount, "record"))
            {
                return false;
            }
            TVector<FRecord> Records(RecordCount);
            for (FRecord& Record : Records)
            {
                uint8 Match = 0;
                Ar << Record.Saved.Value << Match;
                Record.Match = (EMatch)Match;
                if (Record.Match == EMatch::SaveKey || Record.Match == EMatch::NamePath)
                {
                    Ar << Record.Key;
                }
                if (Record.Match == EMatch::Respawn)
                {
                    Ar << Record.SavedParent.Value;
                }
            }

            uint32 DestroyedCount = 0;
            if (!ReadCount(Ar, DestroyedCount, "destroyed entity"))
            {
                return false;
            }
            TVector<ECS::FEntity> Destroyed(DestroyedCount);
            for (ECS::FEntity& Entity : Destroyed)
            {
                Ar << Entity.Value;
            }
            if (Ar.HasError())
            {
                return false;
            }

            for (ECS::FEntity Entity : Destroyed)
            {
                if (Registry.IsValid(Entity) && IsLevelEntity(Registry, Entity))
                {
                    Registry.Destroy(Entity);
                }
            }

            // What the save respawns replaces what this session spawned, so nothing is doubled.
            for (ECS::FEntity Root : GatherRespawnRoots(Registry))
            {
                if (Registry.IsValid(Root))
                {
                    ECS::Utils::DestroyEntityHierarchy(Registry, Root);
                }
            }

            THashMap<ECS::FEntity, ECS::FEntity> Map;
            TVector<ECS::FEntity> Targets(RecordCount, ECS::NullEntity);
            FEntityMatcher Matcher(Registry);
            uint32 Unmatched = 0;
            for (uint32 Index = 0; Index < RecordCount; ++Index)
            {
                const FRecord& Record = Records[Index];
                const ECS::FEntity Target = Record.Match == EMatch::Respawn ? Registry.Create() : Matcher.Find(Record);
                Targets[Index] = Target;
                if (!Target.IsNull())
                {
                    Map[Record.Saved] = Target;
                }
                else
                {
                    ++Unmatched;
                }
            }
            if (Unmatched > 0)
            {
                LOG_WARN("Save game: {} saved entities have nothing to restore into in this world, so their data is skipped.", Unmatched);
            }

            Ar.TranslateEntity = [&](ECS::FEntity Saved)
            {
                if (const auto Found = Map.find(Saved); Found != Map.end())
                {
                    return Found->second;
                }
                return Registry.IsValid(Saved) && IsLevelEntity(Registry, Saved) ? Saved : ECS::NullEntity;
            };

            TVector<TStrongObjectPtr<CEntityScript>> RestoredScripts;
            bool bRespawnedAny = false;
            for (uint32 Index = 0; Index < RecordCount && !Ar.HasError(); ++Index)
            {
                const FRecord& Record = Records[Index];
                const ECS::FEntity Target = Targets[Index];
                int64 Size = 0;
                Ar << Size;
                const int64 Start = Ar.Tell();
                if (Target.IsNull())
                {
                    Ar.Seek(Start + Size);
                    continue;
                }

                if (Record.Match == EMatch::Respawn)
                {
                    bRespawnedAny = true;
                    uint32 ComponentCount = 0;
                    if (!ReadCount(Ar, ComponentCount, "component"))
                    {
                        break;
                    }
                    for (uint32 Component = 0; Component < ComponentCount && !Ar.HasError(); ++Component)
                    {
                        FName StructName;
                        int64 ComponentSize = 0;
                        Ar << StructName << ComponentSize;
                        const int64 ComponentStart = Ar.Tell();
                        CStruct* Struct = FindComponentStruct(StructName.c_str());
                        if (const FComponentOps* Ops = Struct != nullptr ? Struct->GetComponentOps() : nullptr)
                        {
                            Ops->EmplaceSerialized(Registry, Target, Ar);
                        }
                        Ar.Seek(ComponentStart + ComponentSize);
                    }

                    // Respawned scripts start with their saved values and run OnAttach later, so they get no OnRestored.
                    TVector<TStrongObjectPtr<CEntityScript>> NotRestored;
                    ReadSaveGameBlocks(Ar, Registry, Target, false, NotRestored);
                }
                else
                {
                    bool bTransform = false;
                    Ar << bTransform;
                    if (bTransform)
                    {
                        FTransform Local;
                        SerializeTransform(Ar, Local);
                        if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(Target))
                        {
                            Transform->SetLocalTransform(Local);
                        }
                    }
                    ReadSaveGameBlocks(Ar, Registry, Target, true, RestoredScripts);
                }
                Ar.Seek(Start + Size);
            }

            for (uint32 Index = 0; Index < RecordCount; ++Index)
            {
                const FRecord& Record = Records[Index];
                if (Record.Match != EMatch::Respawn || Targets[Index].IsNull() || Record.SavedParent.IsNull())
                {
                    continue;
                }
                const ECS::FEntity Parent = Ar.TranslateEntity(Record.SavedParent);
                if (Registry.IsValid(Parent))
                {
                    Registry.AttachChild(Targets[Index], Parent);
                }
            }
            Ar.TranslateEntity = {};

            if (bRespawnedAny)
            {
                ECS::Utils::RebuildTagStorages(Registry);
            }

            for (const TStrongObjectPtr<CEntityScript>& Script : RestoredScripts)
            {
                if (Script.Get() != nullptr)
                {
                    Script->OnRestored();
                }
            }
            return !Ar.HasError();
        }
    }

    void RecordLevelEntities(ECS::FRegistry& Registry)
    {
        FLevelEntities& Level = Registry.Ctx().Emplace<FLevelEntities>();
        Registry.ForEachEntity([&](ECS::FEntity Entity)
        {
            const uint32 Index = Entity.GetIndex();
            if (Index >= Level.VersionPlusOne.size())
            {
                Level.VersionPlusOne.resize(Index + 1, 0u);
            }
            Level.VersionPlusOne[Index] = Entity.GetVersion() + 1u;
        });
    }

    bool IsLevelEntity(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const FLevelEntities* Level = Registry.Ctx().Find<FLevelEntities>();
        const uint32 Index = Entity.GetIndex();
        return Level != nullptr && !Entity.IsNull() && Index < Level->VersionPlusOne.size()
            && Level->VersionPlusOne[Index] == Entity.GetVersion() + 1u;
    }

    FString GetEntityNamePath(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        TVector<FString> Segments;
        for (ECS::FEntity Current = Entity; !Current.IsNull(); Current = Registry.GetHierarchy().GetParent(Current))
        {
            const FName Name = EntityName(Registry, Current);
            FString Segment = Name.IsNone() ? FString("?") : FString(Name.c_str());
            if (const uint32 Ordinal = NameOrdinal(Registry, Current, Name); Ordinal > 0)
            {
                Segment += Format("#{}", Ordinal);
            }
            Segments.push_back(Move(Segment));
        }

        FString Path;
        for (size_t Index = Segments.size(); Index-- > 0;)
        {
            Path += Segments[Index];
            if (Index > 0)
            {
                Path += "/";
            }
        }
        return Path;
    }

    bool SerializeRegistry(FArchive& Ar, ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();
        FSaveGameArchive SaveAr(Ar);
        return Ar.IsWriting() ? WriteRegistry(SaveAr, Registry) : ReadRegistry(SaveAr, Registry);
    }
}
