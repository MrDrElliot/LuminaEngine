#include "RuntimePCH.h"
#include "ScriptTypeRegistry.h"
#include "ScriptStruct.h"
#include "ScriptableObject.h"
#include "ManagedTypeRegistry.h"
#include "Core/Object/Class.h"
#include "Core/Object/ScriptEnum.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/ObjectUtils.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/Function.h"
#include "Containers/StringFormat.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Core/Object/InstancedStruct.h"
#include "World/Entity/Components/Component.h"

namespace Lumina
{
    namespace
    {
        template<typename TType>
        TStrongObjectPtr<TType> AllocateTransientType(const FName& Name)
        {
            FConstructCObjectParams Params(TType::StaticClass());
            Params.Name    = Name;
            Params.Flags   = OF_Transient;
            Params.Package = CPackage::GetTransientPackage();
            Params.Guid    = FGuid::New();

            TStrongObjectPtr<TType> Allocated = static_cast<TType*>(StaticAllocateObject(Params));
            if (Allocated.Get() != nullptr)
            {
                FinishRegistration(Allocated.Get());
            }
            return Allocated;
        }

        // Only a type a C# reload produced may be replaced, so a native name is never taken over.
        bool IsClaimedByNativeType(const FName& Name)
        {
            const CField* Existing = FindObject<CField>(Name);
            return Existing != nullptr && Cast<CScriptStruct>(Existing) == nullptr && Cast<CScriptEnum>(Existing) == nullptr;
        }

        NODISCARD bool IsComponent(const Scripting::FManagedTypeDefinition& Desc)
        {
            for (const std::pair<FString, FString>& Pair : Desc.Meta)
            {
                if (Pair.first == "Component")
                {
                    return true;
                }
            }
            return false;
        }

        const CStruct& StructOf(const ECS::FComponentTypeInfo& Info)
        {
            return *Info.GetBoundStruct();
        }

        void ConstructComponent(const ECS::FComponentTypeInfo& Info, void* Dest)
        {
            StructOf(Info).InitializeStruct(Dest);
        }

        void CopyConstructComponent(const ECS::FComponentTypeInfo& Info, void* Dest, const void* Source)
        {
            StructOf(Info).InitializeStruct(Dest);
            StructOf(Info).CopyStruct(Dest, Source);
        }

        void DestructComponent(const ECS::FComponentTypeInfo& Info, void* Dest)
        {
            StructOf(Info).DestroyStruct(Dest);
        }

        void RelocateComponent(const ECS::FComponentTypeInfo& Info, void* Dest, void* Source)
        {
            StructOf(Info).RelocateStruct(Dest, Source);
        }

        void MigrateComponent(const ECS::FComponentTypeInfo& From, const void* FromValue, const ECS::FComponentTypeInfo& To, void* ToValue)
        {
            MigrateStructByFieldName(From.GetBoundStruct(), FromValue, To.GetBoundStruct(), ToValue);
        }

        // Paged like a native component that asks for nothing, so a handle into a pool survives an emplace beside it.
        ECS::FComponentTypeInfo MakeComponentTypeInfo(const FName& Name, CScriptStruct& Struct)
        {
            ECS::FComponentTypeInfo Info;
            Info.Name             = Name;
            Info.CachedStruct     = &Struct;
            Info.Size             = Struct.GetAlignedSize();
            Info.Alignment        = Math::Max<uint32>(Struct.GetAlignment(), 1u);
            Info.bEmpty           = Info.Size == 0;
            Info.bPaged           = !Info.bEmpty;
            Info.bRuntimeLayout   = true;
            Info.DefaultConstruct = &ConstructComponent;
            Info.CopyConstruct    = &CopyConstructComponent;
            Info.Destruct         = &DestructComponent;
            Info.Relocate         = &RelocateComponent;
            return Info;
        }

        // Everything a script struct's metadata holds comes from its C# type, so a specifier removed there is removed here.
        void ApplyScriptMetadata(CScriptStruct& Struct, const Scripting::FManagedTypeDefinition& Desc)
        {
            Struct.Metadata = FMetaDataPair();
            for (const std::pair<FString, FString>& Pair : Desc.Meta)
            {
                Struct.Metadata.SetValue(FName(*Pair.first), Pair.second);
            }
            // An asset stores this rather than the object's name, which a superseded type no longer has.
            Struct.Metadata.SetValue("ScriptTypeName", Desc.TypeName.ToString());
        }

        template<typename TType, typename TRetire>
        void RetireAbsentTypes(THashMap<FName, TStrongObjectPtr<TType>>& Types, const THashSet<FName>& Live, TRetire&& Retire)
        {
            TVector<FName> Removed;
            for (const auto& [Name, Type] : Types)
            {
                if (!Live.Contains(Name))
                {
                    Removed.Add(Name);
                }
            }
            for (const FName& Name : Removed)
            {
                Retire(Move(Types[Name]));
                Types.Remove(Name);
            }
        }

        // One pass over every object, since each live field typed by a superseded type keeps that type for another reload.
        template<typename TType>
        void RetireUnreferencedTypes(TVector<TStrongObjectPtr<TType>>& Superseded)
        {
            THashSet<const CField*> Unreferenced;
            for (const TStrongObjectPtr<TType>& Type : Superseded)
            {
                // The registry's own entry is one strong ref, and an FInstancedStruct pinning the type is a second.
                const CStruct* Struct = Cast<CStruct>(static_cast<CObject*>(Type.Get()));
                if (Type->GetStrongRefCount() == 1 && ECS::FComponentTypeRegistry::Get().FindByStruct(Struct) == ECS::InvalidComponentTypeID)
                {
                    Unreferenced.Add(Type.Get());
                }
            }
            if (Unreferenced.IsEmpty())
            {
                return;
            }

            // Never a match, so the walk reaches a map's value after its key instead of stopping at the first hit.
            auto Unpin = [&](const CField* Field)
            {
                if (Field != nullptr)
                {
                    Unreferenced.Remove(Field);
                }
                return false;
            };
            auto DropReferenced = [&](TSpan<FProperty* const> Properties)
            {
                for (const FProperty* Property : Properties)
                {
                    Scripting::PropertyReferencesType(Property, Unpin);
                }
            };
            ForEachObject([&](CObjectBase* Base, int32)
            {
                CStruct* Holder = Base != nullptr && !Base->HasAnyFlag(OF_MarkedDestroy) ? Cast<CStruct>(Base) : nullptr;
                if (Holder == nullptr)
                {
                    return;
                }
                DropReferenced(Holder->GetOwnProperties());
                for (const FFunction* Function : Holder->GetFunctions())
                {
                    DropReferenced(Function->GetParams());
                }
            });

            Superseded.RemoveAll([&](const TStrongObjectPtr<TType>& Type) { return Unreferenced.Contains(Type.Get()); });
        }
    }

    FScriptStructRegistry& FScriptStructRegistry::Get()
    {
        static FScriptStructRegistry Instance;
        return Instance;
    }

    FScriptStructRegistry::FScriptStructRegistry() = default;
    FScriptStructRegistry::~FScriptStructRegistry() = default;

    void FScriptStructRegistry::RegisterComponent(const FName& Name, CScriptStruct& Struct)
    {
        ECS::FComponentTypeRegistry& ComponentTypes = ECS::FComponentTypeRegistry::Get();
        const ECS::FComponentTypeInfo Info = MakeComponentTypeInfo(Name, Struct);

        // A type renamed with [Alias] takes over its old pool, so live components follow the rename.
        FComponentOps* Ops = nullptr;
        for (const TUniquePtr<FComponentOps>& Existing : ComponentOps)
        {
            const FName& Held = ComponentTypes.GetInfo(Existing->GetTypeID()).Name;
            if (Held == Name)
            {
                Ops = Existing.Get();
                break;
            }
            if (Ops == nullptr && FScriptableRegistry::IsRenamedTo(Held, Name))
            {
                Ops = Existing.Get();
            }
        }

        if (Ops == nullptr)
        {
            Ops = ComponentOps.EmplaceGetRef(MakeUnique<FComponentOps>()).Get();
            Ops->TypeId = ComponentTypes.Acquire(Info);
        }
        else if (ComponentTypes.GetInfo(Ops->GetTypeID()).GetBoundStruct() != &Struct)
        {
            ComponentTypes.Relayout(Ops->GetTypeID(), Info, &MigrateComponent);
        }
        RegisterComponentOps(&Struct, Ops);
    }

    void Scripting::RenameSupersededType(CObject& Type)
    {
        const FString Retired = FString("REPLACED_") + *Type.GetName();
        Type.Rename(FName(*Retired, Type.GetInternalIndex() + 1), Type.GetPackage());
    }

    void FScriptStructRegistry::Supersede(TStrongObjectPtr<CScriptStruct>&& Struct)
    {
        CScriptStruct* Old = Struct.Get();
        if (Old == nullptr)
        {
            return;
        }
        Old->MarkSuperseded();
        Scripting::RenameSupersededType(*Old);
        Superseded.Add(Move(Struct));
        ++Generation;
    }

    void FScriptStructRegistry::Refresh(TSpan<const Scripting::FManagedTypeDefinition> Definitions)
    {
        THashSet<FName> Live;
        uint32 MintedCount = 0;

        auto ApplyDefinition = [this](const FName& Name, CScriptStruct& Struct, const Scripting::FManagedTypeDefinition& Desc)
        {
            const FString Stable = Name.ToString();
            DotNet::ApplyScriptableDefaults(FStringView(*Stable, Stable.Len()), Struct.GetDefaultInstance());
            ApplyScriptMetadata(Struct, Desc);
            if (IsComponent(Desc))
            {
                RegisterComponent(Name, Struct);
            }
        };

        // Definitions arrive with every struct after the structs it holds by value, so each finds its fields' types already current.
        for (const Scripting::FManagedTypeDefinition& Desc : Definitions)
        {
            if (Desc.Kind != Scripting::EManagedTypeKind::ScriptStruct || Desc.TypeName.IsNone())
            {
                continue;
            }

            const FName Name = Desc.TypeName;
            Live.Add(Name);

            CStruct* Base = nullptr;
            if (!Desc.NativeBaseName.IsEmpty())
            {
                Base = FindObject<CStruct>(FName(*Desc.NativeBaseName));
                if (Base == nullptr)
                {
                    LOG_WARN("Script struct '{}' extends '{}', which is not a reflected struct; not minted.", Name, *Desc.NativeBaseName);
                    continue;
                }
            }

            auto Existing = Types.Find(Name);
            if (Existing != nullptr)
            {
                CScriptStruct* Current = Existing->Get();
                const EScriptTypeDirty Dirty = Scripting::DiffScriptLayout(Current->GetAppliedSchema(), Current->GetOwnProperties(), Desc.Schema);
                if (Current->GetNativeBase() == Base && !EnumHasAnyFlags(Dirty, EScriptTypeDirty::Layout))
                {
                    // Same layout, so every holder keeps its pointer and only the defaults and metadata are brought up to date.
                    Current->SetAppliedSchema(Desc.Schema);
                    ApplyDefinition(Name, *Current, Desc);
                    continue;
                }
                Supersede(Move(*Existing));
                Types.Remove(Name);
            }

            if (IsClaimedByNativeType(Name))
            {
                LOG_WARN("Script struct '{}' has the name of a native type; not minted.", Name);
                continue;
            }

            TStrongObjectPtr<CScriptStruct> Minted = AllocateTransientType<CScriptStruct>(Name);
            if (Minted.Get() == nullptr || !Minted->BuildFromSchema(Desc.Schema, Base))
            {
                LOG_WARN("Script struct '{}': layout could not be built; not minted.", Name);
                continue;
            }
            Minted->SetAppliedSchema(Desc.Schema);

            // A C# struct's initializers live in its rewritten accessors, so the schema carried none of them.
            ApplyDefinition(Name, *Minted, Desc);
            Types[Name] = Move(Minted);
            ++MintedCount;
        }

        RetireAbsentTypes(Types, Live, [this](TStrongObjectPtr<CScriptStruct>&& Struct) { Supersede(Move(Struct)); });
        RetireUnreferencedTypes(Superseded);

        if (MintedCount > 0)
        {
            LOG_INFO("Minted {} script struct(s).", MintedCount);
        }
    }

    CScriptStruct* FScriptStructRegistry::Find(const FName& ScriptTypeName) const
    {
        auto It = Types.Find(ScriptTypeName);
        return It != nullptr ? It->Get() : nullptr;
    }

    FScriptEnumRegistry& FScriptEnumRegistry::Get()
    {
        static FScriptEnumRegistry Instance;
        return Instance;
    }

    void FScriptEnumRegistry::Refresh(TSpan<const Scripting::FManagedTypeDefinition> Definitions)
    {
        THashSet<FName> Live;
        for (const Scripting::FManagedTypeDefinition& Desc : Definitions)
        {
            if (Desc.Kind != Scripting::EManagedTypeKind::Enum || Desc.TypeName.IsNone())
            {
                continue;
            }
            Live.Add(Desc.TypeName);

            TStrongObjectPtr<CScriptEnum>& Enum = Enums[Desc.TypeName];
            if (Enum.Get() == nullptr)
            {
                if (IsClaimedByNativeType(Desc.TypeName))
                {
                    LOG_WARN("Script enum '{}' has the name of a native type; not minted.", Desc.TypeName);
                    Enums.Remove(Desc.TypeName);
                    continue;
                }
                Enum = AllocateTransientType<CScriptEnum>(Desc.TypeName);
                if (Enum.Get() == nullptr)
                {
                    Enums.Remove(Desc.TypeName);
                    continue;
                }
            }

            // A property holds the enum itself, so members change in place and every field sees them at once.
            Enum->UnderlyingType = Desc.EnumUnderlying;
            Enum->Names.Reset();
            for (const Scripting::FScriptEnumEntry& Entry : Desc.EnumEntries)
            {
                Enum->AddEnum(Entry.Name, (uint64)Entry.Value);
            }
            Enum->Metadata = FMetaDataPair();
            for (const std::pair<FString, FString>& Pair : Desc.Meta)
            {
                Enum->Metadata.SetValue(FName(*Pair.first), Pair.second);
            }
        }

        // Kept alive, since a field of a class not yet reinstanced may still point at a retired enum.
        RetireAbsentTypes(Enums, Live, [this](TStrongObjectPtr<CScriptEnum>&& Enum)
        {
            Scripting::RenameSupersededType(*Enum);
            Superseded.Add(Move(Enum));
        });
        RetireUnreferencedTypes(Superseded);
    }

    CStruct* ResolveStructByName(const FName& Name)
    {
        return FScriptableRegistry::ResolveStruct(Name);
    }

    FName StructIdentity(const CStruct* Struct)
    {
        if (Struct == nullptr)
        {
            return FName();
        }
        if (const FString* Stable = Struct->Metadata.TryGetMetadata("ScriptTypeName"))
        {
            if (!Stable->IsEmpty())
            {
                return FName(*(*Stable));
            }
        }
        return Struct->GetName();
    }
}
