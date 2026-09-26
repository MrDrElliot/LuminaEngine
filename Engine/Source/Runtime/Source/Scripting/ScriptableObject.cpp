#include "RuntimePCH.h"
#include "ScriptFunctionMint.h"
#include "ScriptableObject.h"

#include "Core/Object/ObjectReferenceReplacer.h"
#include "Core/Object/ObjectReinstancer.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
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

            // A hot reload drains the table, so a missing twin is the rebind signal with no per-instance stamp.
            // Only the script's own instance short-circuits; a weak wrapper here would silence the script.
            if (ManagedInstances::IsScriptTwin(Object))
            {
                if (void* Existing = ManagedInstances::Find(Object))
                {
                    return Existing;
                }
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
                ManagedInstances::Set(Object, Handle, /*bScriptTwin*/ true);
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

        // Returns false while live instances remain, since destroying the class would dangle their pointers.
        bool TryRetireMintedClass(const FName& NameId, CScriptClass* Class, const char* Reason)
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

            // Through the same three sources the reinstancer uses, since a walk of the reflected graph alone
            // cannot see an ECS storage, a config map or an editor tool holding this class. It serializes by
            // name, so a re-added type re-resolves from config and saves untouched.
            FObjectReferenceReplacer Replacer(Class, nullptr);
            Replacer.ApplyToAllObjects();

            // The root set holds the only strong reference, so un-rooting reaches zero and frees inside it.
            if (CObject* DefaultObject = Class->GetDefaultObjectIfCreated())
            {
                DefaultObject->RemoveFromRoot();
            }
            // Before un-rooting, which frees the class: the layout lives ON it, so the other order is a
            // use-after-free rather than the tidy-up it reads as.
            Scripting::ForgetScriptClassLayout(Class);

            Class->RemoveFromRoot();

            LOG_DISPLAY("Scriptable: retired minted class '{}' ({}).", NameId.c_str(), Reason);
            return true;
        }
    }

    void FScriptableRegistry::RegisterNative(const char* NativeClassName, const FScriptableNativeInfo& Info)
    {
        // A static-init context, so key by string and never by FName.
        GNativeInfos()[FString(NativeClassName)] = Info;
    }

    /**
     * Turns the events a C# subclass overrides into real functions on the minted class.
     *
     * Each one borrows the declaration from the native base, so the override describes exactly the frame the
     * generated caller builds; all that differs is whose body runs. Rebuilt from scratch, since a reload can
     * remove an override as easily as add one.
     */
    void FScriptableRegistry::ApplyScriptOverrides(CScriptClass* Minted, TSpan<const FString> OverriddenEvents)
    {
        if (Minted == nullptr)
        {
            return;
        }

        Minted->ScriptOverrideFunctions.clear();
        Minted->ScriptOverrideSlots.clear();

        CStruct* Base = Minted->GetSuperStruct();
        if (Base == nullptr)
        {
            return;
        }

        // The thunks are registered against the native base the shim was generated for, which is where the
        // typed dispatch for each event lives.
        const FScriptableNativeInfo* Info = nullptr;
        if (const auto It = GNativeInfos().find(FString(Base->GetName().c_str())); It != GNativeInfos().end())
        {
            Info = &It->second;
            Minted->ScriptOverrideSlots.assign((Info->EventThunks.size() + 63) / 64, 0ull);
        }

        for (const FString& Event : OverriddenEvents)
        {
            const FName EventName(Event.c_str());
            const FFunction* Declared = Base->FindFunction(EventName);
            if (Declared == nullptr)
            {
                LOG_WARN("Scriptable '{}' overrides '{}', which its base does not declare as a reflected "
                         "function; the native body stays in place.", Minted->GetName(), EventName);
                continue;
            }

            // Thunks register in the order the generator numbered the shim's events, so the index is the slot.
            FFunction::FNativeFuncPtr Thunk = nullptr;
            size_t Slot = 0;
            if (Info != nullptr)
            {
                for (size_t i = 0; i < Info->EventThunks.size(); ++i)
                {
                    const FScriptableEventThunk& Candidate = Info->EventThunks[i];
                    if (Candidate.Name != nullptr && EventName == FName(Candidate.Name))
                    {
                        Thunk = Candidate.Thunk;
                        Slot  = i;
                        break;
                    }
                }
            }

            if (Thunk == nullptr)
            {
                LOG_WARN("Scriptable '{}': '{}' has no generated dispatch, so the native body stays in place.",
                    Minted->GetName(), EventName);
                continue;
            }

            Scripting::MintScriptOverride(*Minted, *Declared, Thunk);
            Minted->ScriptOverrideSlots[Slot / 64] |= (1ull << (Slot & 63));
        }
    }

    CScriptClass* FScriptableRegistry::Mint(FStringView TypeName, FStringView NativeBaseName,
                                           TSpan<const FString> OverriddenEvents)
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
            ApplyScriptOverrides(Minted, OverriddenEvents);
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
        TVector<const Scripting::FManagedTypeDefinition*> Descs;
        for (const Scripting::FManagedTypeDefinition& Definition : Definitions)
        {
            if (Definition.Kind == Scripting::EManagedTypeKind::ScriptableClass)
            {
                Descs.push_back(&Definition);
            }
        }

        // Read both to decide what to evacuate and to resolve a saved reference to the old name. Rebuilt
        // rather than added to, so deleting an [Alias] from the C# source actually stops the redirect
        // instead of leaving this generation honouring a rename the source no longer declares.
        TVector<DotNet::FScriptableAlias> Aliases;
        DotNet::GatherScriptableAliases(Aliases);
        GClassRedirects().clear();
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
        THashSet<CClass*> Renamed;
        GatherRenamedClasses(Renamed);

        // A reshaped class is REPLACED rather than rebuilt: an instance's size is fixed at allocation, so a
        // new layout needs a new class, and the reinstancer is what moves the live instances onto it.
        struct FPendingReplacement
        {
            FName         Name;
            CScriptClass* Old = nullptr;
        };

        TVector<FPendingReplacement> Replacements;
        for (CClass* Old : NeedRebuild)
        {
            auto* const Minted = Cast<CScriptClass>(Old);
            if (Minted == nullptr)
            {
                continue;
            }

            // Out of the registry and out of the way of the name so the replacement can claim it. The class
            // object itself stays alive until the reinstance is done, since its instances still point at it.
            const FName Name = Minted->GetName();
            GMintedClasses().erase(Name);

            static uint32 ReplacementCounter = 0;
            const FString Retired = FString("REPLACED_") + Name.c_str();
            Minted->Rename(FName(Retired.c_str(), ++ReplacementCounter), Minted->GetPackage());

            Replacements.push_back(FPendingReplacement{ Name, Minted });
        }

        // AFTER the replacements, so a renamed class retires now instead of lingering in editor pickers.
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
            if (TryRetireMintedClass(Name, GMintedClasses()[Name], "its C# type no longer exists"))
            {
                GMintedClasses().erase(Name);
            }
        }

        bool bMintedAny = false;
        TVector<CScriptClass*> NeedDefaults;
        for (const Scripting::FManagedTypeDefinition* Desc : Descs)
        {
            if (CScriptClass* Minted = Mint(Desc->TypeName.c_str(), Desc->NativeBaseName, Desc->OverriddenEvents))
            {
                // Minted classes are REUSED by name, so an added or removed override must update the mask.
                ApplyScriptOverrides(Minted, Desc->OverriddenEvents);
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
                    // A layout change never reaches here: it was staged as a replacement above, so this class
                    // is a fresh one and the branch over it appended its block already.
                    const EScriptTypeDirty Dirty = Scripting::DiffScriptClassLayout(Minted, Schema);

                    if (EnumHasAnyFlags(Dirty, EScriptTypeDirty::Metadata))
                    {
                        Scripting::RefreshScriptPropertyMetadata(Minted, Schema);
                    }
                    if (EnumHasAnyFlags(Dirty, EScriptTypeDirty::Defaults))
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

        // It runs on the managed side because an initializer is an arbitrary C# expression.
        for (CScriptClass* Minted : NeedDefaults)
        {
            CObject* DefaultObject = Minted->GetDefaultObject();
            DotNet::ApplyScriptableDefaults(Minted->GetName().ToString(), DefaultObject);
        }

        // LAST, because a replacement has to be linked and hold a default object before an instance can be
        // built on it. Every holder is reached through the reflected graph and the registered providers, so
        // nothing here knows what was pointing at the old classes.
        FObjectReinstancer Reinstancer;

        // A [SkipHotReload] property is the one thing the value carry-over must not carry, and the
        // reinstancer has no reason to know what that means.
        Reinstancer.SetPostReplaceHook([](CObject*, CObject* New) { Scripting::ResetSkipHotReloadProperties(New); });

        for (const FPendingReplacement& Pending : Replacements)
        {
            const auto Found = GMintedClasses().find(Pending.Name);
            CScriptClass* const New = Found != GMintedClasses().end() ? Found->second : nullptr;
            if (New == nullptr)
            {
                LOG_WARN("Scriptable: '{}' changed shape but was not re-minted; its instances are left on the "
                         "previous class.", Pending.Name.c_str());
                continue;
            }
            Reinstancer.MapClass(Pending.Old, New);
        }

        // A rename has no replacement of its own: the instances move onto whatever the alias resolves to.
        for (CClass* Old : Renamed)
        {
            if (CClass* Target = ResolveClass(Old->GetName()))
            {
                Reinstancer.MapClass(Old, Target);
            }
        }

        if (!Reinstancer.IsEmpty())
        {
            const FReinstanceResult Result = Reinstancer.Commit();
            LOG_DISPLAY("Scriptable: reinstanced {} object(s), repointed {} reference(s) across {} object(s) "
                        "and {} provider(s).", Result.InstancesReplaced, Result.ReferencesPatched,
                        Result.ObjectsScanned, Result.ProvidersVisited);
        }

        // Outside the commit: a replacement that failed to re-mint leaves the reinstancer empty, and the
        // class renamed aside above would then stay rooted with its layout arena for the rest of the session.
        for (const FPendingReplacement& Pending : Replacements)
        {
            TryRetireMintedClass(Pending.Old->GetName(), Pending.Old, "superseded by a reshaped replacement");
        }
    }
}
