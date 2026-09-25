#include "RuntimePCH.h"
#include "ObjectBase.h"
#include "Class.h"
#include "Cast.h"
#include "ScriptClass.h"
#include "DeferredRegistry.h"
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

    static TConsoleVar MaxCObjectCount("Core.CObject.MaxCount", 100'000, "Maximum number of allowed CObjects");
    
    RUNTIME_API FCObjectArray GObjectArray;


    // Rooted objects are never auto-destroyed.
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


    // Thread local, so two threads constructing at once never read each other's object.
    static thread_local const FConstructCObjectParams* GConstructing = nullptr;

    FScopedObjectConstruction::FScopedObjectConstruction(const FConstructCObjectParams& Params)
        : Previous(GConstructing)
    {
        GConstructing = &Params;
    }

    FScopedObjectConstruction::~FScopedObjectConstruction()
    {
        GConstructing = Previous;
    }

    const FConstructCObjectParams* FScopedObjectConstruction::Current()
    {
        return GConstructing;
    }

    // Identity lands here rather than after the constructor, so a constructor body can read GetClass and GetName.
    CObjectBase::CObjectBase()
        : ObjectFlags()
        , InternalIndex(INDEX_NONE)
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
        if (!GObjectArray.TryClaimDestroyForShutdown(this))
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
        DEBUG_ASSERT(GObjectArray.IsShuttingDown() || GObjectArray.GetStrongRefCountByIndex(InternalIndex) == 0,
            "ForceDestroyNow on an object with live strong references; holders will dangle. Use TWeakObjectPtr for non-owning references.");

        GObjectArray.ForceDestroy(this);
    }

    void CObjectBase::ConditionalBeginDestroy()
    {
        // Serialized against weak-to-strong upgrades, so this cannot race a resurrection into a free.
        GObjectArray.ConditionalDestroy(this);
    }

    int32 CObjectBase::GetStrongRefCount() const
    {
        return GObjectArray.GetStrongRefCountByIndex(InternalIndex);
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
        FScopeLock Lock(RootMutex);
        GRootedObjects.emplace(this);
        SetFlag(OF_Rooted);
    }

    void CObjectBase::RemoveFromRoot()
    {
        // OF_Rooted tracks membership exactly, so an unrooted object is never pinned by the line below.
        if (!HasAnyFlag(OF_Rooted))
        {
            return;
        }

        // The root set often holds the ONLY strong ref, so erase would free this before the flags clear.
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
            Object->FinishRegister(static_cast<CClass*>(Object)->GetMetaClass(), TEXT(""));
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

    // Re-entrancy guard for SettleDeferredRegistrations. The pass creates default objects itself, and a CDO
    // must not restart the pass that is building it.
    static bool GProcessingNewlyLoadedCObjects = false;

    void SettleDeferredRegistrations()
    {
        if (!GProcessingNewlyLoadedCObjects)
        {
            ProcessNewlyLoadedCObjects();
        }
    }

    void ProcessNewlyLoadedCObjects()
    {
        const TGuardValue<bool> Guard(GProcessingNewlyLoadedCObjects, true);

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
                Algo::Sort(NewClasses, [&](const CClass* A, const CClass* B)
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
        // Otherwise dropping the last ref on each rooted object destroys it immediately.
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
