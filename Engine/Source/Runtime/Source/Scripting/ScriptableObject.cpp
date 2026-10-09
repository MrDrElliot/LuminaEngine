#include "RuntimePCH.h"
#include "ScriptFunctionMint.h"
#include "ScriptableObject.h"

#include "Core/Object/ObjectReferenceReplacer.h"
#include "Core/Object/ObjectReinstancer.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
#include "ManagedTypeRegistry.h"

#include "EntityScript.h"
#include "UI/UIScript.h"

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
            if (void* Existing = ManagedInstances::FindScriptTwin(Object))
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

        // Replaced classes an instance or subclass still held, under the name they had, retried every refresh.
        TVector<TPair<FName, CScriptClass*>>& GAwaitingRetirement()
        {
            static TVector<TPair<FName, CScriptClass*>> Classes;
            return Classes;
        }

        // Names rather than pointers, so a redirect can be registered before its target is minted.
        THashMap<FName, FName>& GClassRedirects()
        {
            static THashMap<FName, FName> Map;
            return Map;
        }

        // Returns false while live instances remain, since destroying the class would dangle their pointers.
        // A subclass still links to its parent, so a parent with a live subclass is kept even with no instance of its own.
        bool TryRetireMintedClass(const FName& NameId, CScriptClass* Class, const char* Reason, bool bWarnIfKept = true)
        {
            int32 LiveInstances = 0;
            int32 LiveSubclasses = 0;
            GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
            {
                if (Base == nullptr || Base->HasAnyFlag(OF_MarkedDestroy))
                {
                    return;
                }
                if (Base->GetClass() == Class && !Base->HasAnyFlag(OF_DefaultObject))
                {
                    ++LiveInstances;
                }
                else if (const CClass* Other = Cast<CClass>(Base); Other != nullptr && Other != Class && Other->GetSuperClass() == Class)
                {
                    ++LiveSubclasses;
                }
            });
            if (LiveInstances > 0 || LiveSubclasses > 0)
            {
                if (bWarnIfKept)
                {
                    LOG_WARN("Scriptable: C# class '{}' was removed but {} live instance(s) and {} subclass(es) remain; the class is kept until they are gone.",
                        NameId.c_str(), LiveInstances, LiveSubclasses);
                }
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
        CScriptClass* Minted = nullptr;
        if (auto Parent = GMintedClasses().find(FName(BaseName.c_str())); Parent != GMintedClasses().end())
        {
            // The parent's block is already appended, so this class's own block starts after it.
            CScriptClass* ParentClass = Parent->second;
            AllocateStaticScriptClass(TEXT("/Script"), UTF8_TO_TCHAR(Name.c_str()), &Minted,
                ParentClass->GetSize(), ParentClass->GetAlignment(), ParentClass, ParentClass->FactoryFunction);
        }
        else if (auto It = GNativeInfos().find(BaseName); It != GNativeInfos().end())
        {
            const FScriptableNativeInfo& Info = It->second;
            AllocateStaticScriptClass(TEXT("/Script"), UTF8_TO_TCHAR(Name.c_str()), &Minted,
                Info.ShimSize, Info.ShimAlign, Info.GetBaseClass, Info.Factory);
        }
        else
        {
            LOG_WARN("Scriptable '{}' derives from '{}', which is neither a REFLECT(Scriptable) class nor a minted one; not minted.",
                Name.c_str(), BaseName.c_str());
            return nullptr;
        }
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
            // The new class is minted after this runs, so whether it exists is checked when the instances move.
            if (Minted != GMintedClasses().end())
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
            if (Existing == GMintedClasses().end())
            {
                continue;   // a first mint appends rather than rebuilds
            }

            // Even with no default object yet, since a subclass may already be laid out after this class's old size.
            if (Desc->bHasSchema && !Scripting::ScriptClassLayoutMatches(Existing->second, Desc->Schema))
            {
                NeedRebuild.insert(Existing->second);
            }
        }

        for (const Scripting::FManagedTypeDefinition* Desc : Descs)
        {
            auto Existing = GMintedClasses().find(Desc->TypeName);
            const CClass* Super = Existing != GMintedClasses().end() ? Existing->second->GetSuperClass() : nullptr;
            if (Super != nullptr && Super->GetName() != FName(Desc->NativeBaseName.c_str()))
            {
                NeedRebuild.insert(Existing->second);
            }
        }
        for (bool bGrew = true; bGrew;)
        {
            bGrew = false;
            for (const auto& [Name, Class] : GMintedClasses())
            {
                if (NeedRebuild.find(Class) == NeedRebuild.end() && NeedRebuild.find(Class->GetSuperClass()) != NeedRebuild.end())
                {
                    NeedRebuild.insert(Class);
                    bGrew = true;
                }
            }
        }

        // Its instances are the wrong class rather than the wrong size, and the redirect moves them across.
        THashSet<CClass*> Renamed;
        GatherRenamedClasses(Renamed);

        // Named now, since a renamed class that is also reshaped gets renamed aside before its alias is followed.
        THashMap<CClass*, FName> RenamedFrom;
        for (CClass* Old : Renamed)
        {
            RenamedFrom[Old] = Old->GetName();
        }

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
            Minted->bSuperseded = true;

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
            GMintedClasses()[Name]->bSuperseded = true;
            if (TryRetireMintedClass(Name, GMintedClasses()[Name], "its C# type no longer exists", /*bWarnIfKept*/ false))
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
                Minted->bSuperseded = false;
                // Minted classes are REUSED by name, so an added or removed override must update the mask.
                ApplyScriptOverrides(Minted, Desc->OverriddenEvents);
                Minted->bAbstract = Desc->bAbstract;
                Minted->ScriptUpdatePhase = Desc->UpdatePhase;
                Minted->bScriptParallelUpdate = Desc->bParallelUpdate;
                Minted->ScriptNetRealm = Desc->NetRealm;
                for (const FName& Key : Minted->ScriptClassMetaKeys)
                {
                    Minted->Metadata.RemoveValue(Key);
                }
                Minted->ScriptClassMetaKeys.clear();
                for (const std::pair<FString, FString>& Pair : Desc->ClassMeta)
                {
                    const FName Key(Pair.first.c_str());
                    Minted->Metadata.SetValue(Key, Pair.second);
                    Minted->ScriptClassMetaKeys.push_back(Key);
                }

                // Re-appending would duplicate properties, so a changed schema tears the block down and rebuilds.
                const Scripting::FScriptExportSchema& Schema = Desc->Schema;
                const bool bHaveSchema = Desc->bHasSchema;

                // Only into a class with no block yet, since a second append would shadow the first rather than replace it.
                if (Minted->GetDefaultObjectIfCreated() == nullptr && Minted->LayoutRecord.Get() == nullptr)
                {
                    if (bHaveSchema && Schema.IsValid())
                    {
                        const uint32 Count = Scripting::AppendScriptPropertiesToClass(Minted, Schema);
                        if (Count > 0)
                        {
                            NeedDefaults.push_back(Minted);
                        }
                    }
                }
                else if (bHaveSchema)
                {
                    // A layout change never reaches here: it was staged as a replacement above, so this class
                    // is a fresh one and the branch over it appended its block already.
                    const EScriptTypeDirty Dirty = Scripting::DiffScriptClassLayout(Minted, Schema);

                    if (EnumHasAnyFlags(Dirty, EScriptTypeDirty::Metadata | EScriptTypeDirty::Defaults))
                    {
                        Scripting::RefreshScriptPropertyMetadata(Minted, Schema);
                    }

                    // An unbound instance reads native-backed fields as zero, so the schema never shows an edited initializer.
                    NeedDefaults.push_back(Minted);
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

        // last, because a replacement has to be linked and hold a default object before an instance can be
        // built on it. Every holder is reached through the reflected graph and the registered providers, so
        // nothing here knows what was pointing at the old classes.
        FObjectReinstancer Reinstancer;

        // A [SkipHotReload] property is the one thing the value carry-over must not carry, and the
        // reinstancer has no reason to know what that means.
        Reinstancer.SetPostReplaceHook([](CObject* Old, CObject* New)
        {
            // The tagged carry-over skips what is never saved, such as a UI's Bind values, which a plain reload keeps.
            if (CScriptClass* NewClass = Cast<CScriptClass>(New->GetClass()))
            {
                NewClass->ForEachScriptProperty([&](FProperty* Property)
                {
                    const FProperty* Previous = Property->ShouldSerialize() ? nullptr : Old->GetClass()->GetProperty(Property->GetPropertyName());
                    if (Previous != nullptr && Property->HasSameValueType(Previous))
                    {
                        Property->CopyCompleteValue(Property->GetValuePtr<void>(New), Previous->GetValuePtr<void>(Old));
                    }
                });
            }
            Scripting::ResetSkipHotReloadProperties(New);

            // OnReloaded is what a reload delivers, so a replaced script must not look new enough for OnAttach and OnReady again.
            CEntityScript* OldScript = Cast<CEntityScript>(Old);
            CEntityScript* NewScript = Cast<CEntityScript>(New);
            if (OldScript != nullptr && NewScript != nullptr)
            {
                NewScript->ContinueLifecycleOf(*OldScript);
                UIScripts::Replace(OldScript, NewScript);
            }
        });

        for (const FPendingReplacement& Pending : Replacements)
        {
            // A class that was renamed as well as reshaped is found through its alias.
            const auto Found = GMintedClasses().find(Pending.Name);
            CClass* const New = Found != GMintedClasses().end() ? Found->second : ResolveClass(Pending.Name);
            if (New == nullptr)
            {
                LOG_WARN("Scriptable: '{}' changed shape but was not re-minted; its instances are left on the "
                         "previous class.", Pending.Name.c_str());
                continue;
            }
            // Nothing can be built on an abstract class, so its live instances stay on the old one until they go.
            if (New->IsAbstract())
            {
                LOG_WARN("Scriptable: '{}' became abstract, so its live instances stay on the previous class.", Pending.Name.c_str());
                continue;
            }
            LOG_TRACE("Scriptable: reshaped '{}' under '{}'.", Pending.Name.c_str(),
                New->GetSuperClass() != nullptr ? New->GetSuperClass()->GetName().c_str() : "nothing");
            Reinstancer.MapClass(Pending.Old, New);
        }

        // A rename has no replacement of its own, so the instances move onto whatever the alias resolves to.
        for (CClass* Old : Renamed)
        {
            // An alias naming a type that still exists is a stale attribute, not a rename.
            if (LiveNames.find(RenamedFrom[Old]) != LiveNames.end())
            {
                continue;
            }
            if (CClass* Target = ResolveClass(RenamedFrom[Old]); Target != nullptr && Target != Old && !Target->IsAbstract())
            {
                LOG_TRACE("Scriptable: moving instances of renamed '{}' onto '{}'.", Old->GetName().c_str(), Target->GetName().c_str());
                Reinstancer.MapClass(Old, Target);
            }
        }

        // Instances stranded on an older class move as soon as its name resolves again, re-added or through an alias.
        for (const TPair<FName, CScriptClass*>& Waiting : GAwaitingRetirement())
        {
            if (CClass* Target = ResolveClass(Waiting.first); Target != nullptr && Target != Waiting.second && !Target->IsAbstract())
            {
                Reinstancer.MapClass(Waiting.second, Target);
            }
        }

        if (!Reinstancer.IsEmpty())
        {
            const FReinstanceResult Result = Reinstancer.Commit();
            LOG_DISPLAY("Scriptable: reinstanced {} object(s), repointed {} reference(s) across {} object(s) "
                        "and {} provider(s).", Result.InstancesReplaced, Result.ReferencesPatched,
                        Result.ObjectsScanned, Result.ProvidersVisited);
        }

        // Repeated, because a parent can only go once the subclasses on it have gone.
        TVector<FPendingReplacement> Unretired = Replacements;
        for (const TPair<FName, CScriptClass*>& Waiting : GAwaitingRetirement())
        {
            Unretired.push_back(FPendingReplacement{ Waiting.first, Waiting.second });
        }
        GAwaitingRetirement().clear();
        for (bool bProgress = true; bProgress;)
        {
            bProgress = false;
            for (size_t Index = Unretired.size(); Index-- > 0;)
            {
                if (TryRetireMintedClass(Unretired[Index].Old->GetName(), Unretired[Index].Old, "superseded by a reshaped replacement", false))
                {
                    Unretired.erase(Unretired.begin() + Index);
                    bProgress = true;
                }
            }
            for (const FName& Name : StaleNames)
            {
                auto Stale = GMintedClasses().find(Name);
                if (Stale != GMintedClasses().end() && TryRetireMintedClass(Name, Stale->second, "its C# type no longer exists", false))
                {
                    GMintedClasses().erase(Stale);
                    bProgress = true;
                }
            }
        }
        for (const FPendingReplacement& Pending : Unretired)
        {
            if (!TryRetireMintedClass(Pending.Old->GetName(), Pending.Old, "superseded by a reshaped replacement"))
            {
                GAwaitingRetirement().push_back({ Pending.Name, Pending.Old });
            }
        }
        for (const FName& Name : StaleNames)
        {
            if (auto Stale = GMintedClasses().find(Name); Stale != GMintedClasses().end())
            {
                TryRetireMintedClass(Name, Stale->second, "its C# type no longer exists");
            }
        }
    }
}
