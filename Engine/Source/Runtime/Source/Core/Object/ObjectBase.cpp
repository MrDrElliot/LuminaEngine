#include "RuntimePCH.h"
#include "ObjectBase.h"
#include "Class.h"
#include "Cast.h"
#include "ScriptClass.h"
#include "Lumina.h"
#include "ManagedInstance.h"
#include "ObjectAllocator.h"
#include "ObjectArray.h"
#include "ObjectHash.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Package/Package.h"

namespace Lumina
{

    static TConsoleVar MaxCObjectCount("Core.CObject.MaxCount", 4'194'304, "Maximum number of allowed CObjects");

    struct FPendingTypes
    {
        // Classes allocated before the object system could finish them, completed in allocation order.
        TVector<CObjectBase*>            Registrants;

        TVector<const FCompiledInTypes*> Modules;
        int32                            NumRegistered = 0;

        // Copied out of the module, since its table lives in a DLL that can be freed.
        THashMap<FString, uint64>        LayoutHashes;

        // A default object can register types of its own, so the pass must not restart while it builds one.
        bool                             bRegistering = false;
    };

    static FPendingTypes& PendingTypes()
    {
        static FPendingTypes Pending;
        return Pending;
    }

    namespace Object::Internal
    {
        // The currently constructing context of a CObject.
        static thread_local const FConstructCObjectParams*  GConstructing{};
        static THashSet<TStrongObjectPtr<CObjectBase>>      GRootedObjects{};
        static FMutex                                       GRootMutex{};
    }

    FScopedObjectConstruction::FScopedObjectConstruction(const FConstructCObjectParams& Params)
        : Previous(Object::Internal::GConstructing)
    {
        Object::Internal::GConstructing = &Params;
    }

    FScopedObjectConstruction::~FScopedObjectConstruction()
    {
        Object::Internal::GConstructing = Previous;
    }

    const FConstructCObjectParams* FScopedObjectConstruction::Current()
    {
        return Object::Internal::GConstructing;
    }

    CObjectBase::CObjectBase()
        : ObjectFlags()
        , InternalIndex(Constants::kIndexNone)
    {
        if (const FConstructCObjectParams* Params = FScopedObjectConstruction::Current())
        {
            ObjectFlags    = Params->Flags;
            ClassPrivate   = const_cast<CClass*>(Params->Class);
            PackagePrivate = Params->Package;
            NamePrivate    = Params->Name;
            GUIDPrivate    = Params->Guid;
        }
    }

    CObjectBase::~CObjectBase()
    {
        // Gated on the flag so reaching through ClassPrivate is confined to objects with script storage.
        if (HasAnyFlag(OF_ScriptProperties) && ClassPrivate != nullptr)
        {
            if (const CScriptClass* ScriptClass = ToScriptClass(ClassPrivate))
            {
                ScriptClass->DestructScriptProperties(this);
            }
        }

        // Guarded on the slot, so an object that was never wrapped pays only a compare.
        if (ManagedInstanceSlot != Constants::kIndexNone)
        {
            ManagedInstances::Release(this);
        }

        FObjectHashTables::Get().RemoveObject(this);
        if (InternalIndex != Constants::kIndexNone)
        {
            Object::Internal::DeallocateObject(InternalIndex);
            InternalIndex = Constants::kIndexNone;
        }
    }

    CObjectBase::CObjectBase(EObjectFlags InFlags)
        : ObjectFlags(InFlags)
        , InternalIndex(Constants::kIndexNone)
    {
    }

    CObjectBase::CObjectBase(CClass* InClass, EObjectFlags InFlags, CPackage* Package, FName InName, const FGuid& GUID)
        : ObjectFlags(InFlags)
        , ClassPrivate(InClass)
        , PackagePrivate(Package)
        , NamePrivate(Move(InName))
        , GUIDPrivate(GUID)
        , InternalIndex(Constants::kIndexNone)
    {
    }

    void CObjectBase::BeginRegister()
    {
        PendingTypes().Registrants.Add(this);
    }

    void CObjectBase::FinishRegister(CClass* InClass, const TCHAR* InName)
    {
        ASSERT(ClassPrivate == nullptr);
        ClassPrivate = InClass;

        AddObject();
        AddToRoot();
    }
    
    // The shutdown sweep runs OnDestroy a phase before the free, so this, not the claim, keeps it single.
    void CObjectBase::RunOnDestroyOnce()
    {
        if (HasAnyFlag(OF_DestroyStarted))
        {
            return;
        }

        SetFlag(OF_DestroyStarted);
        OnDestroy();
    }

    void CObjectBase::DestroyInternal()
    {
        SetFlag(OF_MarkedDestroy);

        RunOnDestroyOnce();

        GCObjectAllocator.FreeCObject(this);
    }

    void CObjectBase::BeginDestroyForShutdown()
    {
        SetFlag(OF_MarkedDestroy);

        RunOnDestroyOnce();
    }

    void CObjectBase::FinishDestroyForShutdown()
    {
        // The claim is what keeps this exactly once even though the sweep does no other bookkeeping.
        if (!Object::Internal::TryClaimDestroyForShutdown(this))
        {
            return;
        }

        // An object created during the OnDestroy pass was never visited, so give it the same teardown.
        SetFlag(OF_MarkedDestroy);
        RunOnDestroyOnce();

        GCObjectAllocator.FreeCObject(this);
    }

    void CObjectBase::ForceDestroyNow()
    {
        // Valid at shutdown but a bug at runtime, so long-lived non-owning references must be weak.
        DEBUG_ASSERT(Object::Internal::IsShuttingDown() || Object::Internal::GetStrongRefCountByIndex(InternalIndex) == 0,
            "ForceDestroyNow on an object with live strong references; holders will dangle. Use TWeakObjectPtr for non-owning references.");

        Object::Internal::ForceDestroy(this);
    }

    void CObjectBase::ConditionalBeginDestroy()
    {
        // Serialized against weak-to-strong upgrades, so this cannot race a resurrection into a free.
        Object::Internal::ConditionalDestroy(this);
    }

    int32 CObjectBase::GetStrongRefCount() const
    {
        return Object::Internal::GetStrongRefCountByIndex(InternalIndex);
    }

    void CObjectBase::HandleNameChange(const FName& NewName, CPackage* NewPackage) noexcept
    {
        FObjectHashTables::Get().RemoveObject(this);
        
        NamePrivate = NewName;
        
        if (NewPackage != PackagePrivate && NewPackage != nullptr)
        {
            PackagePrivate = NewPackage;
        }

        FObjectHashTables::Get().AddObject(this);
    }

    void CObjectBase::ClearPackage() noexcept
    {
        FObjectHashTables::Get().RemoveObject(this);
        PackagePrivate = nullptr;
        FObjectHashTables::Get().AddObject(this);
    }

    void CObjectBase::HandleGUIDChange(const FGuid& NewGUID) noexcept
    {
        if (NewGUID == GUIDPrivate)
        {
            return;
        }

        // The hash is keyed on the GUID, so it has to come out before the value moves under it.
        FObjectHashTables::Get().RemoveObject(this);
        GUIDPrivate = NewGUID;
        FObjectHashTables::Get().AddObject(this);
    }

    void CObjectBase::AddToRoot()
    {
        FScopeLock Lock(Object::Internal::GRootMutex);
        Object::Internal::GRootedObjects.TryAdd(this);
        SetFlag(OF_Rooted);
    }

    void CObjectBase::RemoveFromRoot()
    {
        // OF_Rooted tracks membership exactly, so an unrooted object is never pinned by the line below.
        if (!HasAnyFlag(OF_Rooted))
        {
            return;
        }

        // The root set often holds the ONLY strong ref, so Remove would free this before the flags clear.
        TStrongObjectPtr<CObjectBase> Pinned(this);
        {
            FScopeLock Lock(Object::Internal::GRootMutex);
            Object::Internal::GRootedObjects.Remove(this);
            ClearFlags(OF_Rooted);
        }
    }

    FFixedString CObjectBase::MakeDisplayName() const
    {
        return *NamePrivate;
    }

    void CObjectBase::AddObject()
    {
        if (InternalIndex != Constants::kIndexNone)
        {
            return;
        }
        InternalIndex = Object::Internal::AllocateObject(this).Index;
        FObjectHashTables::Get().AddObject(this);
    }

    void FinishRegistration(CObjectBase* Object)
    {
        TVector<CObjectBase*>& Pending = PendingTypes().Registrants;
        const int32 Index = Pending.Find(Object);
        if (Index != Constants::kIndexNone)
        {
            Pending.RemoveAt(Index);
            Object->FinishRegister(static_cast<CClass*>(Object)->GetMetaClass(), TEXT(""));
        }
    }

    FTypeRegistrar::FTypeRegistrar(const FCompiledInTypes& Types)
    {
        PendingTypes().Modules.Add(&Types);
        if (Types.Module != nullptr)
        {
            PendingTypes().LayoutHashes[FString(Types.Module)] = Types.LayoutHash;
        }
    }

    uint64 FindModuleLayoutHash(FStringView Module)
    {
        const THashMap<FString, uint64>& Hashes = PendingTypes().LayoutHashes;
        auto It = Hashes.Find(FString(Module.GetData(), Module.Len()));
        return It != nullptr ? (*It) : 0;
    }

    int32 SnapshotPendingTypes()
    {
        return PendingTypes().Modules.Num();
    }

    void RollbackPendingTypes(int32 Snapshot)
    {
        FPendingTypes& Pending = PendingTypes();

        // A registered module's types are live, so only the refused module's tail can be dropped.
        ASSERT(Snapshot >= Pending.NumRegistered);
        if (Snapshot < Pending.Modules.Num())
        {
            Pending.Modules.SetNum(Snapshot);
        }
    }

    void SettlePendingTypes()
    {
        if (!PendingTypes().bRegistering)
        {
            RegisterPendingTypes();
        }
    }

    void RegisterPendingTypes()
    {
        FPendingTypes& Pending = PendingTypes();
        const TGuardValue<bool> Guard(Pending.bRegistering, true);
        while (!Pending.Registrants.IsEmpty() || Pending.NumRegistered < Pending.Modules.Num())
        {
            // Finishing one can allocate another, which queues behind it.
            while (!Pending.Registrants.IsEmpty())
            {
                FinishRegistration(Pending.Registrants[0]);
            }

            const int32 First = Pending.NumRegistered;
            const int32 Last = Pending.Modules.Num();
            Pending.NumRegistered = Last;

            // Enums and structs first, since a class's properties can name either.
            for (int32 Index = First; Index < Last; ++Index)
            {
                const FCompiledInTypes& Types = *Pending.Modules[Index];
                for (uint32 Enum = 0; Enum < Types.NumEnums; ++Enum)
                {
                    Types.Enums[Enum]();
                }
                for (uint32 Struct = 0; Struct < Types.NumStructs; ++Struct)
                {
                    Types.Structs[Struct]();
                }
            }

            TVector<CClass*> NewClasses;
            for (int32 Index = First; Index < Last; ++Index)
            {
                const FCompiledInTypes& Types = *Pending.Modules[Index];
                for (uint32 Class = 0; Class < Types.NumClasses; ++Class)
                {
                    NewClasses.Add(Types.Classes[Class]());
                }
            }

            auto Depth = [](const CClass* Class)
            {
                int32 Result = 0;
                for (; Class != nullptr; Class = Class->GetSuperClass())
                {
                    ++Result;
                }
                return Result;
            };

            // Bases first, so a default object never sees an incomplete parent.
            Algo::Sort(NewClasses, [&Depth](const CClass* A, const CClass* B) { return Depth(A) < Depth(B); });

            for (CClass* NewClass : NewClasses)
            {
                (void)NewClass->GetDefaultObject();
            }
        }
    }

    void InitializeCObjectSystem()
    {
        Object::Internal::AllocateObjectPool(MaxCObjectCount.GetValue());
        Memory::RegisterCountedCategory("CObject", [] { return GCObjectAllocator.GetStats(); });
    }

    void ShutdownCObjectSystem()
    {
        // Otherwise dropping the last ref on each rooted object destroys it immediately.
        Object::Internal::BeginShutdown();

        Object::Internal::GRootedObjects.Reset();

        Object::Internal::Shutdown();

        FObjectHashTables::Get().Clear();
    }
}
