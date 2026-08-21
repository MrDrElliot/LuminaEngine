#include "RuntimePCH.h"
#include "ObjectBase.h"
#include "Class.h"
#include "DeferredRegistry.h"
#include "Lumina.h"
#include "ManagedInstance.h"
#include "ObjectAllocator.h"
#include "ObjectArray.h"
#include "ObjectHash.h"
#include "Core/Console/ConsoleVariable.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Package/Package.h"

namespace Lumina
{

    static TConsoleVar MaxCObjectCount("Core.CObject.MaxCount", 100'000, "Maximum number of allowed CObjects");
    
    RUNTIME_API FCObjectArray GObjectArray;


    /** Rooted objects: never auto-destroyed. */
    static THashSet<TObjectPtr<CObjectBase>> GRootedObjects;
    static FMutex RootMutex;

    struct FPendingRegistrantInfo
    {
        static TVector<CObjectBase*>& Get()
        {
            static TVector<CObjectBase*> PendingRegistrantInfo;
            return PendingRegistrantInfo;
        }
    };
    
    struct FPendingRegistrant
    {
        CObjectBase*        Object;
        FPendingRegistrant* Next;
    };

    static FPendingRegistrant* GFirstPendingRegistrant = nullptr;
    static FPendingRegistrant* GLastPendingRegistrant = nullptr;


    CObjectBase::CObjectBase()
        : ObjectFlags()
        , InternalIndex(INDEX_NONE)
    {
    }

    CObjectBase::~CObjectBase()
    {
        // Mirror of the construct in StaticAllocateObject; must stay in lockstep with it or script strings
        // leak / double-free. Gated on the flag rather than on a null class so that reaching through
        // ClassPrivate here is confined to the objects that genuinely have trailing script storage: every
        // native object's destructor stays independent of whether its class is still alive, and the ones
        // that do depend on it are covered by the types-last shutdown order in FCObjectArray::Shutdown.
        if (HasAnyFlag(OF_ScriptProperties) && ClassPrivate != nullptr)
        {
            ClassPrivate->DestructScriptProperties(this);
        }

        // Every destruction path lands here, so this is the one place the cached C# wrapper has to be let go.
        // Guarded on the slot so an object that was never wrapped (the overwhelming majority) pays a compare.
        if (ManagedInstanceSlot != INDEX_NONE)
        {
            ManagedInstances::Release(this);
        }

        FObjectHashTables::Get().RemoveObject(this);
        if (InternalIndex != INDEX_NONE)
        {
            GObjectArray.DeallocateObject(InternalIndex);
            InternalIndex = INDEX_NONE;
        }
    }

    void CObjectBase::ConstructInternal(const FObjectInitializer& OI)
    {
        NamePrivate = OI.Params.Name;
        GUIDPrivate = OI.Params.Guid;
        ClassPrivate = const_cast<CClass*>(OI.Params.Class);
        PackagePrivate = OI.Package;

        // The flags the construction asked for. Additive, so they join whatever the constructor set, and
        // applied before AddObject/PostInitProperties so the object is registered and initialized as the
        // caller declared it. Without this every flag passed to NewObject was silently dropped, and the
        // only one that stuck anywhere was OF_DefaultObject, re-stamped by hand after the fact.
        EnumAddFlags(ObjectFlags, OI.Params.Flags);

        AddObject();
    }

    CObjectBase::CObjectBase(EObjectFlags InFlags)
        : ObjectFlags(InFlags)
        , InternalIndex(INDEX_NONE)
    {
    }

    CObjectBase::CObjectBase(CClass* InClass, EObjectFlags InFlags, CPackage* Package, FName InName, const FGuid& GUID)
        : ObjectFlags(InFlags)
        , ClassPrivate(InClass)
        , PackagePrivate(Package)
        , NamePrivate(Move(InName))
        , GUIDPrivate(GUID)
        , InternalIndex(INDEX_NONE)
    {
    }

    void CObjectBase::BeginRegister()
    {
        FPendingRegistrant* PendingRegistrant = new FPendingRegistrant{this, nullptr };
        FPendingRegistrantInfo::Get().push_back(this);

        if (GLastPendingRegistrant)
        {
            GLastPendingRegistrant->Next = PendingRegistrant;
        }
        else
        {
            ASSERT(!GFirstPendingRegistrant);
            GFirstPendingRegistrant = PendingRegistrant;
        }

        GLastPendingRegistrant = PendingRegistrant;
    }

    void CObjectBase::FinishRegister(CClass* InClass, const TCHAR* InName)
    {
        ASSERT(ClassPrivate == nullptr);
        ClassPrivate = InClass;

        AddObject();
        AddToRoot();
    }
    
    void CObjectBase::DestroyInternal()
    {
        SetFlag(OF_MarkedDestroy);

        OnDestroy();

        GCObjectAllocator.FreeCObject(this);
    }

    void CObjectBase::BeginDestroyForShutdown()
    {
        if (HasAnyFlag(OF_MarkedDestroy))
        {
            return;
        }

        SetFlag(OF_MarkedDestroy);

        OnDestroy();
    }

    void CObjectBase::FinishDestroyForShutdown()
    {
        // An object created during the OnDestroy pass was never visited by it. Give it the same
        // teardown rather than freeing it half-destroyed.
        if (!HasAnyFlag(OF_MarkedDestroy))
        {
            SetFlag(OF_MarkedDestroy);
            OnDestroy();
        }

        GCObjectAllocator.FreeCObject(this);
    }

    void CObjectBase::ForceDestroyNow()
    {
        if (HasAnyFlag(OF_MarkedDestroy))
        {
            return;
        }

        // Unconditional teardown: any TObjectPtr still holding this object will be left dangling. That's
        // valid at shutdown (everything goes) but a bug at runtime, long-lived non-owning references
        // must be TWeakObjectPtr. Catch the misuse in debug builds; release still tears down.
        DEBUG_ASSERT(GObjectArray.IsShuttingDown() || GObjectArray.GetStrongRefCountByIndex(InternalIndex) == 0,
            "ForceDestroyNow on an object with live strong references; holders will dangle. Use TWeakObjectPtr for non-owning references.");

        DestroyInternal();
    }

    void CObjectBase::ConditionalBeginDestroy()
    {
        // The reference check + mark + free are serialized against weak->strong upgrades inside the
        // object array, so this can't race a resurrection into a use-after-free.
        GObjectArray.ConditionalDestroy(this);
    }

    int32 CObjectBase::GetStrongRefCount() const
    {
        return GObjectArray.GetStrongRefCountByIndex(InternalIndex);
    }

    int32 CObjectBase::GetWeakRefCount() const
    {
        return GObjectArray.GetWeakRefCountByIndex(InternalIndex);
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

    void CObjectBase::AddToRoot()
    {
        FScopeLock Lock(RootMutex);
        GRootedObjects.emplace(this);
        SetFlag(OF_Rooted);
    }

    void CObjectBase::RemoveFromRoot()
    {
        // OF_Rooted tracks membership exactly, so an unrooted object still costs nothing and, importantly,
        // is never pinned by the line below (which on an unreferenced object would destroy it on release).
        if (!HasAnyFlag(OF_Rooted))
        {
            return;
        }

        // Pinned across the erase: the root set often holds the ONLY strong reference, so dropping it inside
        // erase() destroys and frees this object, and ClearFlags below then writes to freed memory. The pin
        // releases at the end of scope, where reaching zero is a clean destruction with nothing left to touch.
        TObjectPtr<CObjectBase> Pinned(this);
        {
            FScopeLock Lock(RootMutex);
            GRootedObjects.erase(this);
            ClearFlags(OF_Rooted);
        }
    }

    FFixedString CObjectBase::MakeDisplayName() const
    {
        return NamePrivate.c_str();
    }

    void CObjectBase::AddObject()
    {
        if (InternalIndex != INDEX_NONE)
        {
            return;
        }
        InternalIndex = GObjectArray.AllocateObject(this).Index;
        FObjectHashTables::Get().AddObject(this);
    }

    static void DequeuePendingAutoRegistrations(TVector<FPendingRegistrant>& OutPending)
    {
        FPendingRegistrant* NextPendingRegistrant = GFirstPendingRegistrant;
        GFirstPendingRegistrant = nullptr;
        GLastPendingRegistrant = nullptr;
        while(NextPendingRegistrant)
        {
            FPendingRegistrant* PendingRegistrant = NextPendingRegistrant;
            OutPending.push_back(*PendingRegistrant);
            NextPendingRegistrant = PendingRegistrant->Next;
            Memory::Delete(PendingRegistrant);
        }
    }

    static void ProcessRegistrants()
    {
        TVector<FPendingRegistrant> PendingRegistrants;
        DequeuePendingAutoRegistrations(PendingRegistrants);

        for (size_t Index = 0; Index < PendingRegistrants.size(); ++Index)
        {
            const FPendingRegistrant& PendingRegistrant = PendingRegistrants[Index];

            CObjectForceRegistration(PendingRegistrant.Object);

            DequeuePendingAutoRegistrations(PendingRegistrants);
        }
    }
    
    void CObjectForceRegistration(CObjectBase* Object)
    {
        TVector<CObjectBase*>& Pending = FPendingRegistrantInfo::Get();
        int32 Index = VectorFindIndex(Pending, Object);
        
        if (Index != INDEX_NONE)
        {
            Pending.erase(Pending.begin() + Index);
            Object->FinishRegister(CClass::StaticClass(), TEXT(""));
        }
    }
    
    FDeferredRegistrationSnapshot SnapshotDeferredRegistrations()
    {
        FDeferredRegistrationSnapshot Snapshot;
        Snapshot.NumClasses = FClassDeferredRegistry::Get().NumRegistrations();
        Snapshot.NumEnums   = FEnumDeferredRegistry::Get().NumRegistrations();
        Snapshot.NumStructs = FStructDeferredRegistry::Get().NumRegistrations();

        return Snapshot;
    }

    void RollbackDeferredRegistrations(const FDeferredRegistrationSnapshot& Snapshot)
    {
        FClassDeferredRegistry::Get().TruncateRegistrations(Snapshot.NumClasses);
        FEnumDeferredRegistry::Get().TruncateRegistrations(Snapshot.NumEnums);
        FStructDeferredRegistry::Get().TruncateRegistrations(Snapshot.NumStructs);
    }

    static void LoadAllCompiledInEnumsAndStructs()
    {
        FEnumDeferredRegistry& EnumRegistry = FEnumDeferredRegistry::Get();
        FStructDeferredRegistry& StructRegistry = FStructDeferredRegistry::Get();

        EnumRegistry.ProcessRegistrations();
        StructRegistry.ProcessRegistrations();
    }

    void ProcessNewlyLoadedCObjects()
    {
        FClassDeferredRegistry& ClassRegistry = FClassDeferredRegistry::Get();
        FEnumDeferredRegistry& EnumRegistry = FEnumDeferredRegistry::Get();
        FStructDeferredRegistry& StructRegistry = FStructDeferredRegistry::Get();

        while (GFirstPendingRegistrant
            || ClassRegistry.HasPendingRegistrations()
            || EnumRegistry.HasPendingRegistrations()
            || StructRegistry.HasPendingRegistrations())
        {
            ProcessRegistrants();
            LoadAllCompiledInEnumsAndStructs();

            if (ClassRegistry.HasPendingRegistrations())
            {
                TVector<CClass*> NewClasses;
                ClassRegistry.ProcessRegistrations([&NewClasses](CClass& Class)
                {
                    NewClasses.push_back(&Class);
                });

                THashMap<const CClass*, int32> DepthMemo;

                TFunction<int32(const CClass*)> GetClassDepth;
                GetClassDepth = [&](const CClass* Cls) -> int32
                {
                    if (!Cls)
                    {
                        return 0;
                    }

                    int32& Memo = DepthMemo[Cls];
                    if (Memo != 0)
                    {
                        return Memo;
                    }

                    Memo = 1 + GetClassDepth(Cls->GetSuperClass());
                    return Memo;
                };
                
                // Base classes before derived.
                Algo::Sort(NewClasses.begin(), NewClasses.end(), [&](const CClass* A, const CClass* B)
                {
                    return GetClassDepth(A) < GetClassDepth(B);
                });

                for (CClass* NewClass : NewClasses)
                {
                    NewClass->GetDefaultObject();
                }
            }
        }
        
    }

    void InitializeCObjectSystem()
    {
        GObjectArray.AllocateObjectPool(MaxCObjectCount.GetValue());
    }

    void ShutdownCObjectSystem()
    {
        // Must precede the root clear. Otherwise dropping the last ref on each rooted object destroys it
        // immediately.
        GObjectArray.BeginShutdown();

        GRootedObjects.clear();

        GObjectArray.Shutdown();

        FObjectHashTables::Get().Clear();
    }


    void RegisterCompiledInInfo(CClass*(*RegisterFn)(), const TCHAR* Package, const TCHAR* Name)
    {
        FClassDeferredRegistry::Get().AddRegistration(RegisterFn);
    }

    void RegisterCompiledInInfo(CEnum*(*RegisterFn)(), const FEnumRegisterCompiledInInfo& Info)
    {
        FEnumDeferredRegistry::Get().AddRegistration(RegisterFn);
    }

    void RegisterCompiledInInfo(CStruct*(*RegisterFn)(), const FStructRegisterCompiledInInfo& Info)
    {
        FStructDeferredRegistry::Get().AddRegistration(RegisterFn);
    }

    CEnum* GetStaticEnum(CEnum*(* RegisterFn)(), const TCHAR* Name)
    {
        return RegisterFn();
    }

    void RegisterCompiledInInfo(const FClassRegisterCompiledInInfo* Info, size_t NumClassInfo)
    {
        for (const FClassRegisterCompiledInInfo* It = Info; It != Info + NumClassInfo; ++It)
        {
            RegisterCompiledInInfo(It->RegisterFn, Info->Package, Info->Name);
        }
    }

    void RegisterCompiledInInfo(const FEnumRegisterCompiledInInfo* EnumInfo, size_t NumEnumInfo, const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo)
    {
        for (const FClassRegisterCompiledInInfo* It = ClassInfo; It != ClassInfo + NumClassInfo; ++It)
        {
            RegisterCompiledInInfo(It->RegisterFn, ClassInfo->Package, ClassInfo->Name);
        }

        for (const FEnumRegisterCompiledInInfo* It = EnumInfo; It != EnumInfo + NumEnumInfo; ++It)
        {
            RegisterCompiledInInfo(It->RegisterFn, *It);
        }
    }

    void RegisterCompiledInInfo(const FEnumRegisterCompiledInInfo* EnumInfo, size_t NumEnumInfo, const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo, const FStructRegisterCompiledInInfo* StructInfo, size_t NumStructInfo)
    {
        for (const FClassRegisterCompiledInInfo* It = ClassInfo; It != ClassInfo + NumClassInfo; ++It)
        {
            RegisterCompiledInInfo(It->RegisterFn, ClassInfo->Package, ClassInfo->Name);
        }

        for (const FEnumRegisterCompiledInInfo* It = EnumInfo; It != EnumInfo + NumEnumInfo; ++It)
        {
            RegisterCompiledInInfo(It->RegisterFn, *It);
        }

        for (const FStructRegisterCompiledInInfo* It = StructInfo; It != StructInfo + NumStructInfo; ++It)
        {
            RegisterCompiledInInfo(It->RegisterFn, *It);
        }
    }
}
