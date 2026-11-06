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
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ManagedInstance.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptTypeRegistry.h"
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

            // A reload drains the table, so a missing twin is the rebind signal. Only the script's own instance short-circuits here.
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

        // Parents mint first, so a class without its own UpdatePhase finds its parent's already set.
        EScriptUpdatePhase UpdatePhaseInChain(const CClass* Class)
        {
            static const FName UpdatePhaseName("UpdatePhase");
            for (const CClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
            {
                if (Current->HasMeta(UpdatePhaseName))
                {
                    return Current->GetMeta(UpdatePhaseName) == "PostPhysics" ? EScriptUpdatePhase::PostPhysics : EScriptUpdatePhase::PrePhysics;
                }
            }
            return EScriptUpdatePhase::PrePhysics;
        }

        // Ancestors first, the order the generator numbers a shim's slots in, since a shim forwards its Scriptable ancestors' events too.
        void AppendEventThunks(const CClass* NativeClass, TVector<FScriptableEventThunk>& Out)
        {
            if (NativeClass == nullptr)
            {
                return;
            }
            AppendEventThunks(NativeClass->GetSuperClass(), Out);
            if (const auto It = GNativeInfos().Find(FString(*NativeClass->GetName())); It != nullptr)
            {
                Out.Append(It->EventThunks);
            }
        }

        // Names rather than pointers, so a redirect can be registered before its target is minted.
        THashMap<FName, FName>& GClassRedirects()
        {
            static THashMap<FName, FName> Map;
            return Map;
        }

        // Still waiting for registration, so it has no layout and no instances that a rebuild would have to move.
        bool IsMintedThisReload(const CClass* Class)
        {
            return Class->GetClass() == nullptr;
        }

        // False while instances remain, or a subclass still links to it, since destroying the class would dangle their pointers.
        bool TryRetireMintedClass(const FName& NameId, CScriptClass* Class, const char* Reason, bool bWarnIfKept = true)
        {
            int32 LiveInstances = 0;
            int32 LiveSubclasses = 0;
            ForEachObject([&](CObjectBase* Base, int32)
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
                        *NameId, LiveInstances, LiveSubclasses);
                }
                return false;
            }

            // Through the same sources the reinstancer uses, since the reflected graph alone misses ECS storage, config and editor tools.
            FObjectReferenceReplacer Replacer(Class, nullptr);
            Replacer.ApplyToAllObjects();

            // The root set holds the only strong reference, so un-rooting reaches zero and frees inside it.
            if (CObject* DefaultObject = Class->GetDefaultObjectIfCreated())
            {
                DefaultObject->RemoveFromRoot();
            }
            // Before un-rooting, since the layout lives on the class that un-rooting frees.
            Scripting::ForgetScriptClassLayout(Class);

            Class->RemoveFromRoot();

            LOG_DISPLAY("Scriptable: retired minted class '{}' ({}).", *NameId, Reason);
            return true;
        }
    }

    void FScriptableRegistry::RegisterNative(const char* NativeClassName, const FScriptableNativeInfo& Info)
    {
        // A static-init context, so key by string and never by FName.
        GNativeInfos()[FString(NativeClassName)] = Info;
    }

    // Turns the events a C# subclass overrides into functions borrowing the native declaration, rebuilt from scratch each reload.
    void FScriptableRegistry::ApplyScriptOverrides(CScriptClass* Minted, TSpan<const FString> OverriddenEvents)
    {
        if (Minted == nullptr)
        {
            return;
        }

        Minted->ScriptOverrideFunctions.Reset();
        Minted->ScriptOverrideSlots.Reset();

        // Only a native class declares an event, so a class under a C# parent looks past it, registered yet or not, to the native ancestor.
        const CClass* NativeBase = Minted->GetSuperClass();
        while (NativeBase != nullptr && (ToScriptClass(NativeBase) != nullptr || IsMintedThisReload(NativeBase)))
        {
            NativeBase = NativeBase->GetSuperClass();
        }
        if (NativeBase == nullptr)
        {
            return;
        }
        TVector<FScriptableEventThunk> Thunks;
        AppendEventThunks(NativeBase, Thunks);
        Minted->ScriptOverrideSlots.Init(0ull, (Thunks.Num() + 63) / 64);

        for (const FString& Event : OverriddenEvents)
        {
            const FName EventName(*Event);
            const FFunction* Declared = NativeBase->FindFunction(EventName);
            if (Declared == nullptr)
            {
                LOG_WARN("Scriptable '{}' overrides '{}', which its native base does not declare as a reflected "
                         "function; the native body stays in place.", Minted->GetName(), EventName);
                continue;
            }

            // Thunks register in the order the generator numbered the shim's events, so the index is the slot.
            FFunction::FNativeFuncPtr Thunk = nullptr;
            int32 Slot = 0;
            for (int32 i = 0; i < Thunks.Num(); ++i)
            {
                if (Thunks[i].Name != nullptr && EventName == FName(Thunks[i].Name))
                {
                    Thunk = Thunks[i].Thunk;
                    Slot  = i;
                    break;
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
        const FString Name(TypeName.GetData(), TypeName.Len());
        const FName NameId(*Name);

        if (auto Cached = GMintedClasses().Find(NameId); Cached != nullptr)
        {
            return (*Cached); // C# type names are stable across reloads -> reuse
        }
        if (FindObject<CClass>(NameId) != nullptr)
        {
            return nullptr; // a class with this name already exists (native, or a collision) - never shadow it
        }

        const FString BaseName(NativeBaseName.GetData(), NativeBaseName.Len());
        CScriptClass* Minted = nullptr;
        if (auto Parent = GMintedClasses().Find(FName(*BaseName)); Parent != nullptr)
        {
            // The parent's block is already appended, so this class's own block starts after it.
            CScriptClass* ParentClass = (*Parent);
            AllocateStaticScriptClass(TEXT("/Script"), UTF8_TO_TCHAR(*Name), &Minted,
                ParentClass->GetSize(), ParentClass->GetAlignment(), ParentClass, ParentClass->FactoryFunction);
        }
        else if (auto It = GNativeInfos().Find(BaseName); It != nullptr)
        {
            const FScriptableNativeInfo& Info = (*It);
            AllocateStaticScriptClass(TEXT("/Script"), UTF8_TO_TCHAR(*Name), &Minted,
                Info.ShimSize, Info.ShimAlign, Info.GetBaseClass, Info.Factory);
        }
        else if (CClass* NativeBase = FindObject<CClass>(FName(*BaseName)); NativeBase != nullptr && ToScriptClass(NativeBase) == nullptr)
        {
            // Any reflected class can be extended, and REFLECT(Scriptable) only adds events a C# override can replace.
            AllocateStaticScriptClass(TEXT("/Script"), UTF8_TO_TCHAR(*Name), &Minted,
                NativeBase->GetSize(), NativeBase->GetAlignment(), NativeBase, NativeBase->FactoryFunction);
        }
        else
        {
            LOG_WARN("Script class '{}' derives from '{}', which is not a reflected class; not minted.", *Name, *BaseName);
            return nullptr;
        }
        if (Minted != nullptr)
        {
            ApplyScriptOverrides(Minted, OverriddenEvents);
            GMintedClasses()[NameId] = Minted;
        }
        return Minted;
    }


    void FScriptableRegistry::MintNewClasses(TSpan<const Scripting::FManagedTypeDefinition> Definitions)
    {
        // Parents arrive first, so a new class finds its C# parent already minted.
        for (const Scripting::FManagedTypeDefinition& Desc : Definitions)
        {
            if (Desc.Kind != Scripting::EManagedTypeKind::ScriptableClass || GMintedClasses().Contains(Desc.TypeName))
            {
                continue;
            }
            // A parent from an earlier reload may be replaced by this one, so its new child waits to be minted under the replacement.
            auto Parent = GMintedClasses().Find(FName(*Desc.NativeBaseName));
            if (Parent != nullptr && !IsMintedThisReload(*Parent))
            {
                continue;
            }
            Mint(*Desc.TypeName, Desc.NativeBaseName, Desc.OverriddenEvents);
        }
    }

    CClass* FScriptableRegistry::FindClass(const FName& Name)
    {
        if (Name.IsNone())
        {
            return nullptr;
        }
        if (auto Minted = GMintedClasses().Find(Name); Minted != nullptr)
        {
            return (*Minted);
        }
        return FindObject<CClass>(Name);
    }

    void FScriptableRegistry::RegisterClassRedirect(const FName& OldName, const FName& NewName)
    {
        if (OldName.IsNone() || NewName.IsNone() || OldName == NewName)
        {
            return;
        }
        // Some other type claiming a live native type's name must not shadow the real one.
        const CStruct* Existing = FindObject<CStruct>(OldName);
        const bool bScriptOwned = GMintedClasses().Contains(OldName) || Cast<CScriptStruct>(Existing) != nullptr;
        if (Existing != nullptr && !bScriptOwned)
        {
            LOG_WARN("Scriptable: '{}' is claimed as a prior name but a native class already owns it; ignored.",
                *OldName);
            return;
        }
        GClassRedirects()[OldName] = NewName;
    }

    void FScriptableRegistry::SetRedirects(TSpan<const Scripting::FManagedTypeAlias> Aliases)
    {
        // Rebuilt rather than added to, so an [Alias] deleted from the C# source stops redirecting.
        GClassRedirects().Reset();
        for (const Scripting::FManagedTypeAlias& Alias : Aliases)
        {
            RegisterClassRedirect(FName(*Alias.OldName), FName(*Alias.NewName));
        }
    }

    namespace
    {
        // Visits each name a chain of redirects passes through after Name, stopping when Visit returns true.
        template<typename TVisit>
        bool WalkRedirects(const FName& Name, TVisit&& Visit)
        {
            // A visited set, since a bad pair of aliases could otherwise cycle forever.
            THashSet<FName> Seen;
            FName Current = Name;
            Seen.Add(Current);
            while (true)
            {
                auto It = GClassRedirects().Find(Current);
                if (It == nullptr)
                {
                    return false;
                }
                Current = (*It);
                if (Seen.Contains(Current))
                {
                    LOG_WARN("Scriptable: type redirects cycle at '{}'; giving up.", *Current);
                    return false;
                }
                Seen.Add(Current);
                if (Visit(Current))
                {
                    return true;
                }
            }
        }

        template<typename T, typename TFind>
        T* ResolveRedirected(const FName& Name, TFind&& Find)
        {
            if (Name.IsNone())
            {
                return nullptr;
            }
            // Preferring the live old type would strand its instances on it during a rename reload.
            if (!GClassRedirects().Contains(Name))
            {
                return Find(Name);
            }
            T* Found = nullptr;
            WalkRedirects(Name, [&](const FName& Current) { Found = Find(Current); return Found != nullptr; });
            return Found;
        }
    }

    CClass* FScriptableRegistry::ResolveClass(const FName& Name)
    {
        return ResolveRedirected<CClass>(Name, [](const FName& Current) { return FindObject<CClass>(Current); });
    }

    CStruct* FScriptableRegistry::ResolveStruct(const FName& Name)
    {
        return ResolveRedirected<CStruct>(Name, [](const FName& Current) -> CStruct*
        {
            if (CScriptStruct* Script = FScriptStructRegistry::Get().Find(Current))
            {
                return Script;
            }
            return FindObject<CStruct>(Current);
        });
    }

    bool FScriptableRegistry::IsRenamedTo(const FName& OldName, const FName& NewName)
    {
        return WalkRedirects(OldName, [&](const FName& Current) { return Current == NewName; });
    }

    void FScriptableRegistry::GatherRenamedClasses(THashSet<CClass*>& Out)
    {
        for (const auto& [OldName, NewName] : GClassRedirects())
        {
            auto Minted = GMintedClasses().Find(OldName);
            // The new class is minted after this runs, so whether it exists is checked when the instances move.
            if (Minted != nullptr)
            {
                Out.Add(*Minted);
            }
        }
    }

    namespace
    {
        // A reshaped class is replaced, since an instance's size is fixed at allocation, and the reinstancer moves its instances.
        struct FPendingReplacement
        {
            FName         Name;
            CScriptClass* Old = nullptr;
        };

        // Every class whose layout or parent changed, and every class below one, since a child is laid out after its parent.
        THashSet<CClass*> CollectClassesToRebuild(const TVector<const Scripting::FManagedTypeDefinition*>& Descs)
        {
            THashSet<CClass*> NeedRebuild;
            for (const Scripting::FManagedTypeDefinition* Desc : Descs)
            {
                auto Existing = GMintedClasses().Find(Desc->TypeName);
                if (Existing == nullptr || IsMintedThisReload(*Existing))
                {
                    continue;   // a first mint appends rather than rebuilds
                }

                // Even with no default object yet, since a subclass may already be laid out after this class's old size.
                const bool bLayoutChanged = Desc->bHasSchema && !Scripting::ScriptClassLayoutMatches((*Existing), Desc->Schema);
                const CClass* Super = (*Existing)->GetSuperClass();
                const bool bParentChanged = Super != nullptr && Super->GetName() != FName(*Desc->NativeBaseName);
                if (bLayoutChanged || bParentChanged)
                {
                    NeedRebuild.Add(*Existing);
                }
            }
            for (bool bGrew = true; bGrew;)
            {
                bGrew = false;
                for (const auto& [Name, Class] : GMintedClasses())
                {
                    if (!IsMintedThisReload(Class) && !NeedRebuild.Contains(Class)
                        && NeedRebuild.Contains(Class->GetSuperClass()))
                    {
                        NeedRebuild.Add(Class);
                        bGrew = true;
                    }
                }
            }
            return NeedRebuild;
        }

        // Moves each class out of its name, so a replacement can be minted under it while its instances still point at the old one.
        TVector<FPendingReplacement> StepAside(const THashSet<CClass*>& NeedRebuild)
        {
            TVector<FPendingReplacement> Replacements;
            for (CClass* Old : NeedRebuild)
            {
                auto* const Minted = Cast<CScriptClass>(Old);
                if (Minted == nullptr)
                {
                    continue;
                }

                // The object stays alive until the reinstance is done, since its instances still point at it.
                const FName Name = Minted->GetName();
                GMintedClasses().Remove(Name);
                Minted->bSuperseded = true;
                Scripting::RenameSupersededType(*Minted);

                Replacements.Emplace(Name, Minted);
            }
            return Replacements;
        }

        // Mints every class this generation declares and lays it out, then runs its C# initializers into its CDO.
        void MintAndLayOut(const TVector<const Scripting::FManagedTypeDefinition*>& Descs)
        {
            bool bMintedAny = false;
            TVector<CScriptClass*> NeedDefaults;
            for (const Scripting::FManagedTypeDefinition* Desc : Descs)
            {
                if (CScriptClass* Minted = FScriptableRegistry::Mint(*Desc->TypeName, Desc->NativeBaseName, Desc->OverriddenEvents))
                {
                    Minted->bSuperseded = false;
                    // A class is reused by name across reloads, so an added or removed override must update the mask.
                    FScriptableRegistry::ApplyScriptOverrides(Minted, Desc->OverriddenEvents);
                    Minted->bAbstract = Desc->bAbstract;
                    for (const FName& Key : Minted->ScriptClassMetaKeys)
                    {
                        Minted->Metadata.RemoveValue(Key);
                    }
                    Minted->ScriptClassMetaKeys.Reset();
                    for (const std::pair<FString, FString>& Pair : Desc->Meta)
                    {
                        const FName Key(*Pair.first);
                        Minted->Metadata.SetValue(Key, Pair.second);
                        Minted->ScriptClassMetaKeys.Add(Key);
                    }
                    Minted->ScriptUpdatePhase = static_cast<uint8>(UpdatePhaseInChain(Minted));

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
                                NeedDefaults.Add(Minted);
                            }
                        }
                    }
                    else if (bHaveSchema)
                    {
                        // A layout change was staged as a replacement, so only metadata and defaults can differ here.
                        const EScriptTypeDirty Dirty = Scripting::DiffScriptClassLayout(Minted, Schema);

                        if (EnumHasAnyFlags(Dirty, EScriptTypeDirty::Metadata))
                        {
                            Scripting::RefreshScriptPropertyMetadata(Minted, Schema);
                        }

                        // An unbound instance reads native-backed fields as zero, so the schema never shows an edited initializer.
                        NeedDefaults.Add(Minted);
                    }

                    bMintedAny = true;
                }
            }

            // Finalize registration + create CDOs so FindObject<CClass>(name) / NewObject(class) work.
            if (bMintedAny)
            {
                RegisterPendingTypes();
            }

            // It runs on the managed side because an initializer is an arbitrary C# expression.
            for (CScriptClass* Minted : NeedDefaults)
            {
                CObject* DefaultObject = Minted->GetDefaultObject();
                DotNet::ApplyScriptableDefaults(Minted->GetName().ToString(), DefaultObject);
            }
        }

        // Moves every live instance onto its class's replacement, whether the class was reshaped or renamed.
        void ReinstanceOntoReplacements(const TVector<FPendingReplacement>& Replacements, const THashSet<CClass*>& Renamed,
            THashMap<CClass*, FName>& RenamedFrom, const THashSet<FName>& LiveNames)
        {
            // After minting, since a replacement needs its default object before an instance can be built on it.
            FObjectReinstancer Reinstancer;

            // The reinstancer carries every value across, so the [SkipHotReload] reset and the unsaved carry-over happen here.
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
                New->OnReinstanced(*Old);
            });

            for (const FPendingReplacement& Pending : Replacements)
            {
                // A class that was renamed as well as reshaped is found through its alias.
                const auto Found = GMintedClasses().Find(Pending.Name);
                CClass* const New = Found != nullptr ? *Found : FScriptableRegistry::ResolveClass(Pending.Name);
                if (New == nullptr)
                {
                    LOG_WARN("Scriptable: '{}' changed shape but was not re-minted; its instances are left on the "
                             "previous class.", *Pending.Name);
                    continue;
                }
                // Nothing can be built on an abstract class, so its live instances stay on the old one until they go.
                if (New->IsAbstract())
                {
                    LOG_WARN("Scriptable: '{}' became abstract, so its live instances stay on the previous class.", *Pending.Name);
                    continue;
                }
                LOG_TRACE("Scriptable: reshaped '{}' under '{}'.", *Pending.Name,
                    New->GetSuperClass() != nullptr ? *New->GetSuperClass()->GetName() : "nothing");
                Reinstancer.MapClass(Pending.Old, New);
            }

            // A rename has no replacement of its own, so the instances move onto whatever the alias resolves to.
            for (CClass* Old : Renamed)
            {
                // An alias naming a type that still exists is a stale attribute, not a rename.
                if (LiveNames.Contains(RenamedFrom[Old]))
                {
                    continue;
                }
                if (CClass* Target = FScriptableRegistry::ResolveClass(RenamedFrom[Old]); Target != nullptr && Target != Old && !Target->IsAbstract())
                {
                    LOG_TRACE("Scriptable: moving instances of renamed '{}' onto '{}'.", *Old->GetName(), *Target->GetName());
                    Reinstancer.MapClass(Old, Target);
                }
            }

            // Instances stranded on an older class move as soon as its name resolves again, re-added or through an alias.
            for (const TPair<FName, CScriptClass*>& Waiting : GAwaitingRetirement())
            {
                if (CClass* Target = FScriptableRegistry::ResolveClass(Waiting.Key); Target != nullptr && Target != Waiting.Value && !Target->IsAbstract())
                {
                    Reinstancer.MapClass(Waiting.Value, Target);
                }
            }

            if (!Reinstancer.IsEmpty())
            {
                const FReinstanceResult Result = Reinstancer.Commit();
                Scripting::FManagedTypeRegistry::Get().NoteReplaced(Result.Replaced);
                LOG_DISPLAY("Scriptable: reinstanced {} object(s), repointed {} reference(s) across {} object(s) "
                            "and {} provider(s).", Result.InstancesReplaced, Result.ReferencesPatched,
                            Result.ObjectsScanned, Result.ProvidersVisited);
            }
        }

        // Frees every superseded or removed class nothing is still on, keeping the rest for a later reload.
        void RetireUnusedClasses(const TVector<FPendingReplacement>& Replacements, const TVector<FName>& StaleNames)
        {
            // Repeated, because a parent can only go once the subclasses on it have gone.
            TVector<FPendingReplacement> Unretired = Replacements;
            for (const TPair<FName, CScriptClass*>& Waiting : GAwaitingRetirement())
            {
                Unretired.Emplace(Waiting.Key, Waiting.Value);
            }
            GAwaitingRetirement().Reset();
            for (bool bProgress = true; bProgress;)
            {
                bProgress = false;
                for (int32 Index = Unretired.Num(); Index-- > 0;)
                {
                    if (TryRetireMintedClass(Unretired[Index].Old->GetName(), Unretired[Index].Old, "superseded by a reshaped replacement", false))
                    {
                        Unretired.RemoveAt(Index);
                        bProgress = true;
                    }
                }
                for (const FName& Name : StaleNames)
                {
                    auto Stale = GMintedClasses().Find(Name);
                    if (Stale != nullptr && TryRetireMintedClass(Name, (*Stale), "its C# type no longer exists", false))
                    {
                        GMintedClasses().Remove(Name);
                        bProgress = true;
                    }
                }
            }
            for (const FPendingReplacement& Pending : Unretired)
            {
                if (!TryRetireMintedClass(Pending.Old->GetName(), Pending.Old, "superseded by a reshaped replacement"))
                {
                    GAwaitingRetirement().Add({ Pending.Name, Pending.Old });
                }
            }
            for (const FName& Name : StaleNames)
            {
                if (auto Stale = GMintedClasses().Find(Name); Stale != nullptr)
                {
                    TryRetireMintedClass(Name, (*Stale), "its C# type no longer exists");
                }
            }
        }
    }

    void FScriptableRegistry::RefreshMintedClasses(TSpan<const Scripting::FManagedTypeDefinition> Definitions, TSpan<const Scripting::FManagedTypeAlias> Aliases)
    {
        TVector<const Scripting::FManagedTypeDefinition*> Descs;
        for (const Scripting::FManagedTypeDefinition& Definition : Definitions)
        {
            if (Definition.Kind == Scripting::EManagedTypeKind::ScriptableClass)
            {
                Descs.Add(&Definition);
            }
        }

        SetRedirects(Aliases);

        const THashSet<CClass*> NeedRebuild = CollectClassesToRebuild(Descs);

        // Its instances are the wrong class rather than the wrong size, and the redirect moves them across.
        THashSet<CClass*> Renamed;
        GatherRenamedClasses(Renamed);

        // Named now, since a renamed class that is also reshaped gets renamed aside before its alias is followed.
        THashMap<CClass*, FName> RenamedFrom;
        for (CClass* Old : Renamed)
        {
            RenamedFrom[Old] = Old->GetName();
        }

        const TVector<FPendingReplacement> Replacements = StepAside(NeedRebuild);

        // After the replacements step aside, so a renamed class retires now instead of lingering in editor pickers.
        THashSet<FName> LiveNames;
        for (const Scripting::FManagedTypeDefinition* Desc : Descs)
        {
            LiveNames.Add(Desc->TypeName);
        }
        TVector<FName> StaleNames;
        for (const auto& [Name, Class] : GMintedClasses())
        {
            if (!LiveNames.Contains(Name))
            {
                StaleNames.Add(Name);
            }
        }
        for (const FName& Name : StaleNames)
        {
            GMintedClasses()[Name]->bSuperseded = true;
            if (TryRetireMintedClass(Name, GMintedClasses()[Name], "its C# type no longer exists", /*bWarnIfKept*/ false))
            {
                GMintedClasses().Remove(Name);
            }
        }

        MintAndLayOut(Descs);
        ReinstanceOntoReplacements(Replacements, Renamed, RenamedFrom, LiveNames);
        RetireUnusedClasses(Replacements, StaleNames);
    }
}
