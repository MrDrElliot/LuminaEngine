#include "RuntimePCH.h"
#include "ScriptableObject.h"

#include "EntityScript.h"

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
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

            // Already built (this generation): the slot is the single source of truth. A hot reload drains the
            // table, so an empty slot IS the rebind signal -- no generation stamp to carry per instance.
            if (void* Existing = ManagedInstances::Find(Object))
            {
                return Existing;
            }

            // The class default object never gets a managed counterpart. It is also never dispatched to, so
            // this is belt-and-braces rather than a hot path.
            if (Object->HasAnyFlag(OF_DefaultObject) || Object->GetClass() == nullptr)
            {
                return nullptr;
            }

            void* Handle = DotNet::CreateScriptable(Object->GetClass()->GetName().ToString(), (uint64)(uintptr_t)Object);
            if (Handle != nullptr)
            {
                // The slot owns it from here: ~CObjectBase frees it, and the teardown contract drains the
                // whole table before the collectible ALC unloads.
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
        THashMap<FName, CClass*>& GMintedClasses()
        {
            static THashMap<FName, CClass*> Map;
            return Map;
        }

        // Prior script class name -> its current name, from the `[Alias]` attributes on C# script classes.
        // Names rather than CClass*, so a redirect can be registered before its target has been minted.
        THashMap<FName, FName>& GClassRedirects()
        {
            static THashMap<FName, FName> Map;
            return Map;
        }

        void ClearDeadClassRefsInStruct(CStruct* Layout, void* Data, CClass* Dead, const CObject* Owner, int32& OutCleared);

        // Nulls every reflected value inside Value that still points at Dead: FClassProperty (TSubclassOf)
        // directly, recursing through struct values, array elements, and instanced-struct payloads.
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

        // Tears down a minted class whose C# type no longer exists in the current generation, so
        // FindObject / pickers stop seeing it and the name frees up for a future re-mint. Returns false
        // (class kept) while live instances remain; destroying the class under them would dangle their
        // class pointer, so removal is retried on the next reload once they are gone.
        bool TryRetireMintedClass(const FName& NameId, CClass* Class)
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

            // FClassProperty holds a raw CClass* (TSubclassOf), so any reflected value still naming this
            // class (a settings CDO, for example) must be nulled before the class dies. It serializes by
            // name, so a re-added type re-resolves from config/saves untouched.
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

            if (CObject* DefaultObject = Class->GetDefaultObjectIfCreated())
            {
                DefaultObject->RemoveFromRoot();
                DefaultObject->ForceDestroyNow();
            }
            Class->RemoveFromRoot();
            Class->ForceDestroyNow();

            // Safe only here, past the live-instance check above: the layout record owns everything the
            // class's appended properties point at.
            Scripting::ForgetScriptClassLayout(Class);

            LOG_DISPLAY("Scriptable: retired minted class '{}' (its C# type no longer exists).", NameId.c_str());
            return true;
        }
    }

    void FScriptableRegistry::RegisterNative(const char* NativeClassName, const FScriptableNativeInfo& Info)
    {
        // Static-init context (see GNativeInfos): key by string, never FName.
        GNativeInfos()[FString(NativeClassName)] = Info;
    }

    CClass* FScriptableRegistry::Mint(FStringView TypeName, FStringView NativeBaseName, uint64 OverrideFlags)
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
        CClass* Minted = nullptr;
        AllocateStaticClass(TEXT("/Script"), UTF8_TO_TCHAR(Name.c_str()), &Minted,
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
        // A name that is currently a live class is not a redirect source: some other type claiming it as a
        // prior name must not shadow the real one.
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
        // Redirects are consulted BEFORE the name itself. During the reload that renames a type both
        // classes exist for a moment, and the whole point of the redirect is to move instances OFF the old
        // one -- preferring the live old class would silently strand them on it. RegisterClassRedirect
        // already refuses to shadow a name a non-minted class owns, so this cannot hijack a native class.
        if (GClassRedirects().find(Name) == GClassRedirects().end())
        {
            if (CClass* Direct = FindObject<CClass>(Name))
            {
                return Direct;
            }
            return nullptr;
        }

        // Follow the chain, so two renames in a row still resolve. Bounded by the redirect count, with a
        // visited set because a bad pair of aliases could otherwise cycle forever.
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
            // Only if the rename target actually exists; otherwise the old class is all there is and moving
            // its instances would destroy them.
            if (FindObject<CClass>(NewName) != nullptr)
            {
                Out.insert(Minted->second);
            }
        }
    }

    void FScriptableRegistry::RefreshMintedClasses()
    {
        TVector<DotNet::FScriptableTypeDesc> Descs;
        DotNet::GatherScriptableTypes(Descs);

        // Where any renamed class went, FIRST: GatherRenamedClasses below reads these to decide what to
        // evacuate, and the component load path reads them to resolve a saved reference to the old name.
        TVector<DotNet::FScriptableAlias> Aliases;
        DotNet::GatherScriptableAliases(Aliases);
        for (const DotNet::FScriptableAlias& Alias : Aliases)
        {
            RegisterClassRedirect(FName(Alias.OldName.c_str()), FName(Alias.NewName.c_str()));
        }

        // Evacuate before minting anything, because a class whose property set changed cannot be rebuilt
        // with instances of it alive: an object's size is baked in at allocation. Gathering the schemas twice
        // (once here, once in the mint loop) is the price of knowing which classes are affected BEFORE the
        // first one is touched, and a reload that changes no layout evacuates nothing at all.
        THashSet<CClass*> NeedRebuild;
        for (const DotNet::FScriptableTypeDesc& Desc : Descs)
        {
            const FName NameId(Desc.TypeName.c_str());
            auto Existing = GMintedClasses().find(NameId);
            if (Existing == GMintedClasses().end() || Existing->second->GetDefaultObjectIfCreated() == nullptr)
            {
                continue;   // a first mint appends rather than rebuilds
            }

            Scripting::FScriptExportSchema Schema;
            TVector<Scripting::FScriptPropertyEntry> Defaults;
            if (DotNet::GatherScriptSchema(Desc.TypeName, Schema, Defaults)
                && !Scripting::ScriptClassLayoutMatches(Existing->second, Schema))
            {
                NeedRebuild.insert(Existing->second);
            }
        }

        // A RENAMED class needs the same round trip for a different reason: its instances are not the wrong
        // size, they are the wrong class. Evacuating them writes the old name, and Restore resolves that
        // through the redirect onto the new class, which is what actually moves them across. Doing it here
        // also empties the old class, so the retire pass below stops deferring on it.
        GatherRenamedClasses(NeedRebuild);

        TVector<EntityScripts::FEvacuatedScripts> Evacuated;
        if (!NeedRebuild.empty())
        {
            const int32 Count = EntityScripts::Evacuate(NeedRebuild, Evacuated);
            if (Count > 0)
            {
                LOG_DISPLAY("Scriptable: {} script class(es) changed shape or name; evacuated {} entit{}.",
                    NeedRebuild.size(), Count, Count == 1 ? "y" : "ies");
            }
        }

        // Retire minted classes whose C# type vanished this generation, so a deleted script's class doesn't
        // linger in FindObject / editor pickers forever.
        //
        // AFTER the evacuation, not before. A renamed class is "vanished" by name, and its instances were
        // just evacuated, so it retires on this reload instead of deferring to the next one and leaving the
        // editor showing a class nothing can be attached to.
        THashSet<FName> LiveNames;
        for (const DotNet::FScriptableTypeDesc& Desc : Descs)
        {
            LiveNames.insert(FName(Desc.TypeName.c_str()));
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
        TVector<CClass*> NeedDefaults;
        for (const DotNet::FScriptableTypeDesc& Desc : Descs)
        {
            if (CClass* Minted = Mint(Desc.TypeName, Desc.NativeBaseName, Desc.OverrideFlags))
            {
                // Re-stamp on every refresh, not just the first mint: minted classes are REUSED by name
                // across reloads, so a generation that adds or removes an override on an existing type must
                // update the mask the shims read.
                Minted->ScriptOverrides = Desc.OverrideFlags;

                // Append this type's [Property] members as real FPropertys in the trailing block.
                //
                // Two paths, because a minted class is reused by name across reloads and keeps its identity.
                // On a FIRST mint the block is simply appended. On a LATER reload the block already exists,
                // so re-appending would duplicate the properties and move every existing object's fields;
                // the block has to be torn down and rebuilt instead, and only when the schema actually
                // changed (the common reload edits a method body and leaves the layout alone).
                Scripting::FScriptExportSchema Schema;
                TVector<Scripting::FScriptPropertyEntry> Defaults;
                const bool bHaveSchema = DotNet::GatherScriptSchema(Desc.TypeName, Schema, Defaults);

                if (Minted->GetDefaultObjectIfCreated() == nullptr)
                {
                    if (bHaveSchema && Schema.IsValid())
                    {
                        const uint32 Count = Scripting::AppendScriptPropertiesToClass(Minted, Schema);
                        if (Count > 0)
                        {
                            LOG_DISPLAY("Scriptable '{}': appended {} script propert{} to the minted class.",
                                Desc.TypeName.c_str(), Count, Count == 1 ? "y" : "ies");
                            NeedDefaults.push_back(Minted);
                        }
                    }
                }
                else if (bHaveSchema && !Scripting::ScriptClassLayoutMatches(Minted, Schema))
                {
                    // Refuses (and warns) while live instances remain, since they are laid out at the old
                    // size. Evacuating them is the caller's job and is not wired up yet, so today this
                    // rebuilds on reload only when nothing is attached.
                    if (Scripting::MigrateMintedClassLayout(Minted, Schema))
                    {
                        NeedDefaults.push_back(Minted);
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

        // The C# type's declared initializers land on the CDO, which only exists after the pass above; every
        // instance is then copied from it. This is the script equivalent of a C++ constructor seeding its CDO,
        // and it runs on the managed side because the initializer is an arbitrary C# expression -- it is
        // replayed through the real property accessors rather than decoded from a value blob.
        for (CClass* Minted : NeedDefaults)
        {
            CObject* DefaultObject = Minted->GetDefaultObject();
            DotNet::ApplyScriptableDefaults(Minted->GetName().ToString(), DefaultObject);
        }

        // Last, so every rebuilt class has its new layout AND its new defaults: a restored script is
        // constructed from the CDO and then has its saved values replayed over it, so a property added this
        // reload has to already carry the author's initializer or it would come back zeroed.
        if (!Evacuated.empty())
        {
            const int32 Count = EntityScripts::Restore(Evacuated);
            LOG_DISPLAY("Scriptable: restored {} entit{} after the rebuild.", Count, Count == 1 ? "y" : "ies");
        }
    }
}
