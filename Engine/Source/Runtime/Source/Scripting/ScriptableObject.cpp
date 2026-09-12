#include "RuntimePCH.h"
#include "ScriptableObject.h"
#include "ManagedTypeRegistry.h"

#include "EntityScript.h"

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Engine/Engine.h"
#include "DotNet/DotNetHost.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ManagedInstance.h"
#include "Scripting/ScriptStruct.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Log/Log.h"

namespace Lumina
{
    
    namespace Scriptable
    {
        void* GetOrCreateInstance(CObject* Object)
        {
            if (Object == nullptr)
            {
                return nullptr;
            }

            // A hot reload drains the table, so an empty slot IS the rebind signal with no per-instance stamp.
            if (void* Existing = ManagedInstances::Find(Object))
            {
                return Existing;
            }

            // The class default object is never dispatched to, so this is belt and braces rather than hot.
            if (Object->HasAnyFlag(OF_DefaultObject) || Object->GetClass() == nullptr)
            {
                return nullptr;
            }

            void* Handle = DotNet::CreateScriptable(Object->GetClass()->GetName().ToString(), (uint64)(uintptr_t)Object);
            if (Handle != nullptr)
            {
                // The teardown contract drains the whole table before the collectible load context unloads.
                ManagedInstances::Set(Object, Handle);
            }
            return Handle;
        }
    }

    namespace
    {
        THashMap<FString, FScriptableNativeInfo>& GNativeInfos()
        {
            static THashMap<FString, FScriptableNativeInfo> Map;
            return Map;
        }
        THashMap<FName, CScriptClass*>& GMintedClasses()
        {
            static THashMap<FName, CScriptClass*> Map;
            return Map;
        }

        // Names rather than pointers, so a redirect can be registered before its target is minted.
        THashMap<FName, FName>& GClassRedirects()
        {
            static THashMap<FName, FName> Map;
            return Map;
        }

        void ClearDeadClassRefsInStruct(CStruct* Layout, void* Data, CClass* Dead, const CObject* Owner, int32& OutCleared);

        // Recurses through struct values, array elements and instanced-struct payloads.
        void ClearDeadClassRefsInValue(FProperty* Property, void* Value, CClass* Dead, const CObject* Owner, int32& OutCleared)
        {
            switch (Property->TypeFlags)
            {
            case EPropertyTypeFlags::Class:
            {
                CClass** Ptr = static_cast<CClass**>(Value);
                if (*Ptr == Dead)
                {
                    *Ptr = nullptr;
                    ++OutCleared;
                    LOG_DISPLAY("Scriptable: cleared '{}.{}', it referenced the retired class '{}'.",
                        Owner->GetName().c_str(), Property->Name.c_str(), Dead->GetName().c_str());
                }
                break;
            }
            case EPropertyTypeFlags::Struct:
            {
                FStructProperty* StructProperty = static_cast<FStructProperty*>(Property);
                if (StructProperty->GetStruct() != nullptr)
                {
                    ClearDeadClassRefsInStruct(StructProperty->GetStruct(), Value, Dead, Owner, OutCleared);
                }
                break;
            }
            case EPropertyTypeFlags::Vector:
            {
                FArrayProperty* ArrayProperty = static_cast<FArrayProperty*>(Property);
                if (FProperty* Inner = ArrayProperty->GetInternalProperty())
                {
                    ArrayProperty->ForEach(Value, [&](void* Element, SIZE_T)
                    {
                        ClearDeadClassRefsInValue(Inner, Element, Dead, Owner, OutCleared);
                    });
                }
                break;
            }
            case EPropertyTypeFlags::InstancedStruct:
            {
                FInstancedStruct* Instanced = static_cast<FInstancedStruct*>(Value);
                if (Instanced->GetScriptStruct() != nullptr && Instanced->GetMutableMemory() != nullptr)
                {
                    ClearDeadClassRefsInStruct(Instanced->GetScriptStruct(), Instanced->GetMutableMemory(), Dead, Owner, OutCleared);
                }
                break;
            }
            default:
                break;
            }
        }

        void ClearDeadClassRefsInStruct(CStruct* Layout, void* Data, CClass* Dead, const CObject* Owner, int32& OutCleared)
        {
            for (CStruct* Current = Layout; Current != nullptr; Current = Current->GetSuperStruct())
            {
                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    ClearDeadClassRefsInValue(Property, static_cast<uint8*>(Data) + Property->Offset, Dead, Owner, OutCleared);
                });
            }
        }

        // Returns false while live instances remain, since destroying the class would dangle their pointers.
        bool TryRetireMintedClass(const FName& NameId, CScriptClass* Class)
        {
            int32 LiveInstances = 0;
            GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
            {
                if (Base != nullptr && Base->GetClass() == Class
                    && !Base->HasAnyFlag(OF_MarkedDestroy) && !Base->HasAnyFlag(OF_DefaultObject))
                {
                    ++LiveInstances;
                }
            });
            if (LiveInstances > 0)
            {
                LOG_WARN("Scriptable: C# class '{}' was removed but {} live instance(s) remain; the class is kept until they are gone.",
                    NameId.c_str(), LiveInstances);
                return false;
            }

            // It serializes by name, so a re-added type re-resolves from config and saves untouched.
            int32 Cleared = 0;
            GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
            {
                if (Base == nullptr || Base == Class || Base->HasAnyFlag(OF_MarkedDestroy))
                {
                    return;
                }
                CObject* Object = static_cast<CObject*>(Base);
                if (Object->GetClass() != nullptr)
                {
                    ClearDeadClassRefsInStruct(Object->GetClass(), Object, Class, Object, Cleared);
                }
            });

            // The root set holds the only strong reference, so un-rooting reaches zero and frees inside it.
            if (CObject* DefaultObject = Class->GetDefaultObjectIfCreated())
            {
                DefaultObject->RemoveFromRoot();
            }
            // Before un-rooting, which frees the class: the layout lives ON it, so the other order is a
            // use-after-free rather than the tidy-up it reads as.
            Scripting::ForgetScriptClassLayout(Class);

            Class->RemoveFromRoot();

            LOG_DISPLAY("Scriptable: retired minted class '{}' (its C# type no longer exists).", NameId.c_str());
            return true;
        }
    }

    void FScriptableRegistry::RegisterNative(const char* NativeClassName, const FScriptableNativeInfo& Info)
    {
        // A static-init context, so key by string and never by FName.
        GNativeInfos()[FString(NativeClassName)] = Info;
    }

    CScriptClass* FScriptableRegistry::Mint(FStringView TypeName, FStringView NativeBaseName, uint64 OverrideFlags)
    {
        const FString Name(TypeName.data(), TypeName.size());
        const FName NameId(Name.c_str());

        if (auto Cached = GMintedClasses().find(NameId); Cached != GMintedClasses().end())
        {
            return Cached->second; // C# type names are stable across reloads -> reuse
        }
        if (FindObject<CClass>(NameId) != nullptr)
        {
            return nullptr; // a class with this name already exists (native, or a collision) - never shadow it
        }

        const FString BaseName(NativeBaseName.data(), NativeBaseName.size());
        auto It = GNativeInfos().find(BaseName);
        if (It == GNativeInfos().end())
        {
            LOG_WARN("Scriptable '{}': native base '{}' is not a REFLECT(Scriptable) class; not minted.",
                Name.c_str(), BaseName.c_str());
            return nullptr;
        }

        const FScriptableNativeInfo& Info = It->second;
        CScriptClass* Minted = nullptr;
        AllocateStaticScriptClass(TEXT("/Script"), UTF8_TO_TCHAR(Name.c_str()), &Minted,
            Info.ShimSize, Info.ShimAlign, Info.GetBaseClass, Info.Factory);
        if (Minted != nullptr)
        {
            Minted->ScriptOverrides = OverrideFlags;
            GMintedClasses()[NameId] = Minted;
        }
        return Minted;
    }


    void FScriptableRegistry::RegisterClassRedirect(const FName& OldName, const FName& NewName)
    {
        if (OldName.IsNone() || NewName.IsNone() || OldName == NewName)
        {
            return;
        }
        // Some other type claiming a live class's name must not shadow the real one.
        if (FindObject<CClass>(OldName) != nullptr && GMintedClasses().find(OldName) == GMintedClasses().end())
        {
            LOG_WARN("Scriptable: '{}' is claimed as a prior name but a native class already owns it; ignored.",
                OldName.c_str());
            return;
        }
        GClassRedirects()[OldName] = NewName;
    }

    CClass* FScriptableRegistry::ResolveClass(const FName& Name)
    {
        if (Name.IsNone())
        {
            return nullptr;
        }
        // Preferring the live old class would strand instances on it during a rename reload.
        if (GClassRedirects().find(Name) == GClassRedirects().end())
        {
            if (CClass* Direct = FindObject<CClass>(Name))
            {
                return Direct;
            }
            return nullptr;
        }

        // A visited set, since a bad pair of aliases could otherwise cycle forever.
        THashSet<FName> Seen;
        FName Current = Name;
        Seen.insert(Current);
        while (true)
        {
            auto It = GClassRedirects().find(Current);
            if (It == GClassRedirects().end())
            {
                return nullptr;
            }
            Current = It->second;
            if (Seen.find(Current) != Seen.end())
            {
                LOG_WARN("Scriptable: class redirects cycle at '{}'; giving up.", Current.c_str());
                return nullptr;
            }
            Seen.insert(Current);
            if (CClass* Renamed = FindObject<CClass>(Current))
            {
                return Renamed;
            }
        }
    }

    void FScriptableRegistry::GatherRenamedClasses(THashSet<CClass*>& Out)
    {
        for (const auto& [OldName, NewName] : GClassRedirects())
        {
            auto Minted = GMintedClasses().find(OldName);
            if (Minted == GMintedClasses().end())
            {
                continue;   // nothing minted under the old name, so nothing to move
            }
            // Otherwise the old class is all there is, and moving its instances would destroy them.
            if (FindObject<CClass>(NewName) != nullptr)
            {
                Out.insert(Minted->second);
            }
        }
    }

    void FScriptableRegistry::RefreshMintedClasses(TSpan<const Scripting::FManagedTypeDefinition> Definitions)
    {
        // Before any rebuild, so this reload's retirements are stamped with the generation it opens.
        Scripting::AdvanceScriptTypeGeneration();

        TVector<const Scripting::FManagedTypeDefinition*> Descs;
        for (const Scripting::FManagedTypeDefinition& Definition : Definitions)
        {
            if (Definition.Kind == Scripting::EManagedTypeKind::ScriptableClass)
            {
                Descs.push_back(&Definition);
            }
        }

        // Read both to decide what to evacuate and to resolve a saved reference to the old name.
        TVector<DotNet::FScriptableAlias> Aliases;
        DotNet::GatherScriptableAliases(Aliases);
        for (const DotNet::FScriptableAlias& Alias : Aliases)
        {
            RegisterClassRedirect(FName(Alias.OldName.c_str()), FName(Alias.NewName.c_str()));
        }

        // An object's size is baked in at allocation, so a changed property set needs an empty class.
        THashSet<CClass*> NeedRebuild;
        for (const Scripting::FManagedTypeDefinition* Desc : Descs)
        {
            auto Existing = GMintedClasses().find(Desc->TypeName);
            if (Existing == GMintedClasses().end() || Existing->second->GetDefaultObjectIfCreated() == nullptr)
            {
                continue;   // a first mint appends rather than rebuilds
            }

            if (Desc->bHasSchema && !Scripting::ScriptClassLayoutMatches(Existing->second, Desc->Schema))
            {
                NeedRebuild.insert(Existing->second);
            }
        }

        // Its instances are the wrong class rather than the wrong size, and the redirect moves them across.
        GatherRenamedClasses(NeedRebuild);

        TVector<EntityScripts::FEvacuatedScripts> Evacuated;

        // The one live instance no registry owns, so nothing else would take it out of the way of a rebuild.
        FName          EvacuatedGameInstanceClass;
        TVector<uint8> EvacuatedGameInstanceBytes;
        bool           bEvacuatedGameInstance = false;

        if (!NeedRebuild.empty())
        {
            const int32 Count = EntityScripts::Evacuate(NeedRebuild, Evacuated);
            if (Count > 0)
            {
                LOG_DISPLAY("Scriptable: {} script class(es) changed shape or name; evacuated {} entit{}.",
                    NeedRebuild.size(), Count, Count == 1 ? "y" : "ies");
            }

            if (GEngine != nullptr)
            {
                bEvacuatedGameInstance = GEngine->EvacuateGameInstance(
                    NeedRebuild, EvacuatedGameInstanceClass, EvacuatedGameInstanceBytes);
                if (bEvacuatedGameInstance)
                {
                    LOG_DISPLAY("Scriptable: evacuated the game instance ('{}') for the rebuild.",
                        EvacuatedGameInstanceClass.c_str());
                }
            }
        }

        // AFTER the evacuation, so a renamed class retires now instead of lingering in editor pickers.
        THashSet<FName> LiveNames;
        for (const Scripting::FManagedTypeDefinition* Desc : Descs)
        {
            LiveNames.insert(Desc->TypeName);
        }
        TVector<FName> StaleNames;
        for (const auto& [Name, Class] : GMintedClasses())
        {
            if (LiveNames.find(Name) == LiveNames.end())
            {
                StaleNames.push_back(Name);
            }
        }
        for (const FName& Name : StaleNames)
        {
            if (TryRetireMintedClass(Name, GMintedClasses()[Name]))
            {
                GMintedClasses().erase(Name);
            }
        }

        bool bMintedAny = false;
        TVector<CScriptClass*> NeedDefaults;
        for (const Scripting::FManagedTypeDefinition* Desc : Descs)
        {
            if (CScriptClass* Minted = Mint(Desc->TypeName.c_str(), Desc->NativeBaseName, Desc->OverrideFlags))
            {
                // Minted classes are REUSED by name, so an added or removed override must update the mask.
                Minted->ScriptOverrides = Desc->OverrideFlags;
                Minted->ScriptUpdatePhase = Desc->UpdatePhase;

                // Re-appending would duplicate properties, so a changed schema tears the block down and rebuilds.
                const Scripting::FScriptExportSchema& Schema = Desc->Schema;
                const bool bHaveSchema = Desc->bHasSchema;

                if (Minted->GetDefaultObjectIfCreated() == nullptr)
                {
                    if (bHaveSchema && Schema.IsValid())
                    {
                        const uint32 Count = Scripting::AppendScriptPropertiesToClass(Minted, Schema);
                        if (Count > 0)
                        {
                            LOG_DISPLAY("Scriptable '{}': appended {} script propert{} to the minted class.",
                                Desc->TypeName.c_str(), Count, Count == 1 ? "y" : "ies");
                            NeedDefaults.push_back(Minted);
                        }
                    }
                }
                else if (bHaveSchema)
                {
                    const EScriptTypeDirty Dirty = Scripting::DiffScriptClassLayout(Minted, Schema);

                    if (EnumHasAnyFlags(Dirty, EScriptTypeDirty::Layout))
                    {
                        // Refuses while live instances remain, since they are laid out at the old size.
                        if (Scripting::MigrateMintedClassLayout(Minted, Schema))
                        {
                            NeedDefaults.push_back(Minted);
                        }
                    }
                    else
                    {
                        // No reshape, so these land without touching instances that are already live.
                        if (EnumHasAnyFlags(Dirty, EScriptTypeDirty::Metadata))
                        {
                            Scripting::RefreshScriptPropertyMetadata(Minted, Schema);
                        }
                        if (EnumHasAnyFlags(Dirty, EScriptTypeDirty::Defaults))
                        {
                            NeedDefaults.push_back(Minted);
                        }
                    }
                }

                bMintedAny = true;
            }
        }

        // Finalize registration + create CDOs so FindObject<CClass>(name) / NewObject(class) work.
        if (bMintedAny)
        {
            ProcessNewlyLoadedCObjects();
        }

        // It runs on the managed side because an initializer is an arbitrary C# expression.
        for (CScriptClass* Minted : NeedDefaults)
        {
            CObject* DefaultObject = Minted->GetDefaultObject();
            DotNet::ApplyScriptableDefaults(Minted->GetName().ToString(), DefaultObject);
        }

        // A property added this reload has to carry its initializer already or it would come back zeroed.
        if (!Evacuated.empty())
        {
            const int32 Count = EntityScripts::Restore(Evacuated);
            LOG_DISPLAY("Scriptable: restored {} entit{} after the rebuild.", Count, Count == 1 ? "y" : "ies");
        }

        if (bEvacuatedGameInstance && GEngine != nullptr)
        {
            GEngine->RestoreGameInstance(EvacuatedGameInstanceClass, EvacuatedGameInstanceBytes);
            LOG_DISPLAY("Scriptable: restored the game instance after the rebuild.");
        }
    }
}
