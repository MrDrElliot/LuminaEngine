#include "MCPPrefabTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentEntityToken.h"
#include "Agent/AgentToolRegistry.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Assets/Factories/Factory.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Session/SessionOps.h"
#include "MCPTextMatch.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"
#include "Scripting/EntityScript.h"

namespace Lumina::MCP
{
    namespace MCPPrefabToolsInternal
    {
        // Class names in the registry are strings, so each distinct one is resolved once per call.
        bool IsPrefabClassName(const FName& ClassName, THashMap<FName, bool>& Cache)
        {
            const auto Found = Cache.Find(ClassName);
            if (Found != nullptr)
            {
                return (*Found);
            }

            CClass* Class = FindObject<CClass>(ClassName);
            const bool bPrefab = Class != nullptr && Class->IsChildOf(CPrefab::StaticClass());
            Cache.TryAdd(ClassName, bPrefab);
            return bPrefab;
        }

        Agent::FToolResult PrefabEditInstance(const SPrefabInstanceParams& In, SPrefabInstanceResult& Out)
        {
            FString Error;
            ECS::FRegistry* Registry = SessionOps::GetSceneRegistry(Error);
            ECS::FEntity Root = ECS::NullEntity;
            if (Registry == nullptr || !Agent::FEntityTokens::Resolve(*Registry, FStringView(In.Entity), Root, Error))
            {
                return Agent::FToolResult::Error(Error);
            }
            Out.bDone = In.Action == EPrefabInstanceAction::Detach
                ? SessionOps::DetachPrefabInstance(Root, Error)
                : SessionOps::ResyncPrefabInstance(Root, Error);
            return Out.bDone ? Agent::FToolResult::Ok("Done.") : Agent::FToolResult::Error(Error);
        }

        void RegisterPrefabInstance(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SPrefabInstanceParams, SPrefabInstanceResult>(
                Owner, "prefab.instance",
                "Resync a placed instance from its prefab, or Detach it from the prefab, as the outliner's menu does.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SPrefabInstanceParams& In, SPrefabInstanceResult& Out) { return PrefabEditInstance(In, Out); });
        }

        Agent::FToolResult PrefabCreateFromEntities(const SCreatePrefabParams& In, SCreatePrefabResult& Out)
        {
            FString Error;
            ECS::FRegistry* Registry = SessionOps::GetSceneRegistry(Error);
            if (Registry == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }
            TVector<ECS::FEntity> Entities;
            for (const FString& Token : In.Entities)
            {
                ECS::FEntity Entity = ECS::NullEntity;
                if (!Agent::FEntityTokens::Resolve(*Registry, FStringView(Token), Entity, Error))
                {
                    return Agent::FToolResult::Error(Error);
                }
                Entities.Add(Entity);
            }
            if (!SessionOps::SelectEntities(Entities, Error))
            {
                return Agent::FToolResult::Error(Error);
            }
            CPrefab* Prefab = SessionOps::CreatePrefabFromSelection(FStringView(In.Folder.IsEmpty() ? FString("/Game/Content") : In.Folder),
                FStringView(In.Name), Error);
            if (Prefab == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }
            Out.Guid = FString(*Prefab->GetGUID().ToString());
            Out.Path = FString(*Prefab->GetPackage()->GetPackagePath());
            return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
        }

        Agent::FToolResult PrefabCreateVariant(const SCreatePrefabParams& In, SCreatePrefabResult& Out)
        {
            CPrefab* Parent = nullptr;
            FString Error;
            if (!Agent::ResolveAsset<CPrefab>(FStringView(In.VariantOf), Parent, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CPackage* ParentPackage = Parent->GetPackage();
            if (ParentPackage == nullptr)
            {
                return Agent::FToolResult::Error("That prefab has no package to sit beside.");
            }

            const FFixedString ParentPath = ParentPackage->GetPackagePath();
            const FString Folder = Paths::Parent(FStringView(*ParentPath, ParentPath.Len()));
            const FString Name = In.Name.IsEmpty() ? FString(*Parent->GetName().ToString()) + "_Variant" : In.Name;

            FFixedString Path = Paths::Combine(FStringView(Folder), FStringView(Name));
            CPackage::AddPackageExt(Path);
            if (VFS::Exists(Path))
            {
                return Agent::FToolResult::Error(Lumina::Format("{} already exists.", Path));
            }

            CPrefab* Variant = CFactory::CreateNewOf<CPrefab>(FStringView(*Path, Path.Len()));
            if (Variant == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("Could not create {}.", Path));
            }

            Variant->ParentPrefab = Parent;
            Variant->ResolveVariant();

            if (!CPackage::SavePackage(Variant->GetPackage(), FStringView(*Path, Path.Len())))
            {
                return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
            }
            FAssetRegistry::Get().AssetCreated(Variant);

            Out.Path = FString(*Path);
            Out.Guid = FString(*Variant->GetGUID().ToString());
            return Agent::FToolResult::Ok(Lumina::Format("Created variant {} of {}.", Out.Path, Parent->GetName().ToString()));
        }

        void RegisterPrefabCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreatePrefabParams, SCreatePrefabResult>(
                Owner, "prefab.create",
                "Save world entities as a new prefab (this selects them). With VariantOf set, instead create a variant saved beside that prefab.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreatePrefabParams& In, SCreatePrefabResult& Out)
                {
                    return In.VariantOf.IsEmpty() ? PrefabCreateFromEntities(In, Out) : PrefabCreateVariant(In, Out);
                });
        }

        void RegisterPrefabList(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListPrefabsParams, SListPrefabsResult>(
                Owner, "prefab.list",
                "List prefab assets, with the GUID prefab.spawn takes.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListPrefabsParams& In, SListPrefabsResult& Out)
                {
                    const int32 Limit = In.Limit > 0 ? In.Limit : 50;
                    const int32 Offset = In.Offset > 0 ? In.Offset : 0;

                    THashMap<FName, bool> ClassCache;

                    FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
                    {
                        if (!IsPrefabClassName(Data.AssetClass, ClassCache))
                        {
                            return false;
                        }

                        const FString Name(*Data.AssetName.ToString());
                        const FString Path(*Data.Path);
                        if (!ContainsTextFold(FStringView(Name), In.Contains) && !ContainsTextFold(FStringView(Path), In.Contains))
                        {
                            return false;
                        }

                        ++Out.Matched;

                        if (Out.Matched > Offset && Out.Results.Num() < Limit)
                        {
                            SAssetInfo Info;
                            Info.Name       = Name;
                            Info.Path       = Path;
                            Info.AssetClass = FString(*Data.AssetClass.ToString());
                            Info.Guid       = FString(*Data.AssetGUID.ToString());
                            Out.Results.Add(Move(Info));
                        }

                        return false;
                    });

                    return Agent::FToolResult::Ok(Lumina::Format("{} of {} matching prefab(s).",
                        Out.Results.Num(), Out.Matched));
                });
        }

        void RegisterPrefabDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribePrefabParams, SDescribePrefabResult>(
                Owner, "prefab.describe",
                "Report what a prefab contains: its root entities, their components, and any parent variant.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribePrefabParams& In, SDescribePrefabResult& Out)
                {
                    CPrefab* Prefab = nullptr;
                    FString Error;
                    if (!Agent::ResolveAsset<CPrefab>(FStringView(In.Asset), Prefab, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name  = FString(*Prefab->GetName().ToString());
                    Out.ClassName = FString(*Prefab->GetClass()->GetName().ToString());

                    if (CPackage* Package = Prefab->GetPackage())
                    {
                        Out.Path = FString(*Package->GetPackagePath());
                    }

                    if (Prefab->ParentPrefab != nullptr)
                    {
                        Out.ParentPrefab = FString(*Prefab->ParentPrefab->GetGUID().ToString());
                    }

                    ECS::FRegistry& Registry = Prefab->Registry;

                    auto StableIDOf = [&Registry](ECS::FEntity Entity)
                    {
                        const SPrefabComponent* Node = Registry.IsValid(Entity) ? Registry.TryGet<SPrefabComponent>(Entity) : nullptr;
                        return Node != nullptr ? FString(*Node->StableID) : FString();
                    };

                    Registry.ForEachEntity([&](ECS::FEntity Entity)
                    {
                        ++Out.EntityCount;

                        SPrefabNodeInfo& Node = Out.Nodes.EmplaceGetRef();
                        Node.StableID = StableIDOf(Entity);
                        Node.ParentStableID = StableIDOf(Registry.GetHierarchy().GetParent(Entity));
                        if (const SNameComponent* Named = Registry.TryGet<SNameComponent>(Entity))
                        {
                            Node.Name = FString(*Named->Name.ToString());
                        }
                        ForEachComponentStruct([&](CStruct* Reflected)
                        {
                            if (ECS::Utils::HasComponent(Registry, Entity, Reflected))
                            {
                                Node.Components.Emplace(*Reflected->GetName().ToString());
                            }
                        });
                        if (const SEntityScriptComponent* Scripts = Registry.TryGet<SEntityScriptComponent>(Entity))
                        {
                            for (const TStrongObjectPtr<CEntityScript>& Script : Scripts->Scripts)
                            {
                                Node.Scripts.Add(Script.Get() != nullptr ? FString(*Script->GetClass()->GetName()) : FString("<null>"));
                            }
                            for (const SHeldValue& Held : Scripts->Pending)
                            {
                                Node.Scripts.Add(FString("held:") + *Held.TypeName);
                            }
                        }

                        if (Registry.GetHierarchy().GetParent(Entity) != ECS::NullEntity)
                        {
                            return;
                        }

                        SPrefabEntityInfo Info;
                        if (const SNameComponent* Named = Registry.TryGet<SNameComponent>(Entity))
                        {
                            Info.Name = FString(*Named->Name.ToString());
                        }

                        ForEachComponentStruct([&](CStruct* Reflected)
                        {
                            if (ECS::Utils::HasComponent(Registry, Entity, Reflected))
                            {
                                Info.Components.Emplace(*Reflected->GetName().ToString());
                            }
                        });

                        Out.RootEntities.Add(Move(Info));
                    });

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' holds {} entit{} with {} root(s).{}",
                        Out.Name, Out.EntityCount, Out.EntityCount == 1 ? "y" : "ies", Out.RootEntities.Num(),
                        Out.ParentPrefab.IsEmpty() ? "" : " It is a variant."));
                });
        }

        void RegisterPrefabSpawn(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSpawnPrefabParams, SSpawnPrefabResult>(
                Owner, "prefab.spawn",
                "Place an instance of a prefab in the open world, as one undo step.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSpawnPrefabParams& In, SSpawnPrefabResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error("No world editor is open, so there is nowhere to spawn.");
                    }

                    if (SessionOps::IsSceneTargetSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    CPrefab* Prefab = nullptr;
                    FString Error;
                    if (!Agent::ResolveAsset<CPrefab>(FStringView(In.Asset), Prefab, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CWorld* World = SessionOps::GetSceneWorld(SceneError);
                    ECS::FRegistry& Registry = *ScenePtr;

                    ECS::FEntity Parent = ECS::NullEntity;
                    if (!In.Parent.IsEmpty() && !Agent::FEntityTokens::Resolve(Registry, FStringView(In.Parent), Parent, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FTransform Transform;
                    Transform.SetLocation(FVector3(In.Position.X, In.Position.Y, In.Position.Z));
                    Transform.SetRotationFromEuler(FVector3(In.Rotation.X, In.Rotation.Y, In.Rotation.Z));
                    Transform.SetScale(FVector3(In.Scale, In.Scale, In.Scale));

                    TSoftObjectPtr<CPrefab> Ref;
                    if (CPackage* Package = Prefab->GetPackage())
                    {
                        Ref.SetPath(*Package->GetPackagePath());
                    }

                    ECS::FEntity Root = ECS::NullEntity;
                    SessionOps::RunCreationTransacted("Spawn Prefab (agent)", [&]()
                    {
                        Root = World->GetSpawner()->SpawnPrefabAt(Ref, Transform, Parent);
                        if (Root != ECS::NullEntity)
                        {
                            SessionOps::AdoptSpawnedSubtree(Root);
                        }

                        if (Root != ECS::NullEntity && !In.Name.IsEmpty())
                        {
                            if (SNameComponent* Named = Registry.TryGet<SNameComponent>(Root))
                            {
                                Named->Name = FName(In.Name);
                            }
                        }
                    }, SceneError);

                    if (Root == ECS::NullEntity)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' could not be instantiated.", Prefab->GetName()));
                    }

                    Out.Entity = Agent::FEntityTokens::Mint(Registry, Root);
                    if (const SNameComponent* Named = Registry.TryGet<SNameComponent>(Root))
                    {
                        Out.Name = FString(*Named->Name.ToString());
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Spawned '{}' as {}.", Out.Name, Out.Entity));
                });
        }

        void RegisterPrefabCapture(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCapturePrefabParams, SCapturePrefabResult>(
                Owner, "prefab.capture",
                "Overwrite a prefab with a placed instance's entities, save it and refresh other instances. This is how to edit a prefab: change an instance, then capture.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCapturePrefabParams& In, SCapturePrefabResult& Out)
                {
                    FString SceneError;
                    ECS::FRegistry* ScenePtr = SessionOps::GetSceneRegistry(SceneError);
                    if (ScenePtr == nullptr)
                    {
                        return Agent::FToolResult::Error("No world editor is open, so there is nothing to capture from.");
                    }

                    if (SessionOps::IsSceneTargetSimulating())
                    {
                        return Agent::FToolResult::Error("Stop play-in-editor first.");
                    }

                    CPrefab* Prefab = nullptr;
                    FString Error;
                    if (!Agent::ResolveAsset<CPrefab>(FStringView(In.Asset), Prefab, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    ECS::FRegistry& Registry = *ScenePtr;
                    ECS::FEntity Root = ECS::NullEntity;
                    if (!Agent::FEntityTokens::Resolve(Registry, FStringView(In.Entity), Root, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CWorld* World = SessionOps::GetSceneWorld(SceneError);

                    // The prefab's own root name, so capturing a placed instance does not rename the asset's root after it.
                    FName PreviousRootName;
                    Prefab->Registry.ForEachEntity([&](ECS::FEntity E)
                    {
                        const SNameComponent* Name = Prefab->Registry.TryGet<SNameComponent>(E);
                        if (Prefab->Registry.GetHierarchy().GetParent(E) == ECS::NullEntity && Name != nullptr)
                        {
                            PreviousRootName = Name->Name;
                        }
                    });

                    const SPrefabInstanceComponent* Instance = Registry.TryGet<SPrefabInstanceComponent>(Root);
                    const bool bPushingInstanceBack = Instance != nullptr && Instance->bIsRoot && Instance->SourcePrefab.Get() == Prefab;

                    // A plain source becomes the prefab's first instance, so a later capture from it still matches the others.
                    bool bPlainSource = Instance == nullptr;
                    Registry.GetHierarchy().ForEachDescendant(Root, [&](ECS::FEntity E)
                    {
                        bPlainSource = bPlainSource && !Registry.HasAll<SPrefabInstanceComponent>(E);
                    });
                    Prefab->CaptureFromWorld(World, Root, bPushingInstanceBack || bPlainSource);

                    if (bPushingInstanceBack && !PreviousRootName.IsNone())
                    {
                        Prefab->Registry.ForEachEntity([&](ECS::FEntity E)
                        {
                            if (Prefab->Registry.GetHierarchy().GetParent(E) == ECS::NullEntity)
                            {
                                if (SNameComponent* Name = Prefab->Registry.TryGet<SNameComponent>(E))
                                {
                                    Name->Name = PreviousRootName;
                                }
                            }
                        });
                    }

                    // Anchor the captured root at origin so the prefab opens centered in its editor.
                    Prefab->Registry.ForEachEntity([&](ECS::FEntity E)
                    {
                        Out.EntityCount++;
                        if (Prefab->Registry.GetHierarchy().GetParent(E) != ECS::NullEntity)
                        {
                            return;
                        }
                        if (STransformComponent* Tx = Prefab->Registry.TryGet<STransformComponent>(E))
                        {
                            Tx->SetLocalTransform(FTransform());
                        }
                    });

                    // A variant saves only its delta, so without this the capture would be thrown away on save.
                    if (Prefab->IsVariant())
                    {
                        Prefab->CaptureVariantDelta();
                    }

                    CPackage* Package = Prefab->GetPackage();
                    if (Package == nullptr)
                    {
                        return Agent::FToolResult::Error("That prefab has no package to save.");
                    }

                    const FFixedString Path = Package->GetPackagePath();
                    if (!CPackage::SavePackage(Package, Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetSaved(Prefab);
                    Prefab->RefreshInstancesInLoadedWorlds();
                    Prefab->PropagateToVariants();

                    Out.Path = FString(*Path);
                    return Agent::FToolResult::Ok(Lumina::Format("Captured {} into {}.", In.Entity, Out.Path));
                });
        }
    }

    using namespace MCPPrefabToolsInternal;

    void RegisterPrefabTools(FStringView Owner)
    {
        RegisterPrefabList(Owner);
        RegisterPrefabDescribe(Owner);
        RegisterPrefabSpawn(Owner);
        RegisterPrefabCapture(Owner);
        RegisterPrefabCreate(Owner);
        RegisterPrefabInstance(Owner);
    }
}
