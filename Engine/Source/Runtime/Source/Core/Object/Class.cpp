#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "Class.h"
#include "ScriptClass.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Metadata/PropertyMetadata.h"
#include "Package/Package.h"

IMPLEMENT_INTRINSIC_CLASS(CClass, CStruct, RUNTIME_API)

namespace Lumina
{
    static CPackage* ResolveClassPackage(const TCHAR* Package)
    {
        if (Package == nullptr || Package[0] == '\0')
        {
            return nullptr;
        }

        CPackage* PackageObject = FindObject<CPackage>(Package);
        if (PackageObject == nullptr)
        {
            PackageObject = NewObject<CPackage>(nullptr, Package);
        }
        return PackageObject;
    }

    static void LinkAndQueueStaticClass(CClass* NewClass, CClass* (*SuperClassFn)())
    {
        CClass* SuperClass = SuperClassFn();
        const bool bValidSuperClass = (SuperClass != NewClass);

        NewClass->SetSuperStruct(bValidSuperClass ? SuperClass : nullptr);

        NewClass->RegisterDependencies();
        NewClass->BeginRegister();
    }

    RUNTIME_API void AllocateStaticClass(const TCHAR* Package, const TCHAR* Name, CClass** OutClass, uint32 Size, uint32 Alignment, CClass* (*SuperClassFn)(), CClass::FactoryFunctionType FactoryFunc)
    {
        DEBUG_ASSERT(*OutClass == nullptr);

        *OutClass = Memory::New<CClass>(ResolveClassPackage(Package), FName(Name), Size, Alignment, OF_None, FactoryFunc);
        LinkAndQueueStaticClass(*OutClass, SuperClassFn);
    }

    RUNTIME_API void AllocateStaticScriptClass(const TCHAR* Package, const TCHAR* Name, CScriptClass** OutClass, uint32 Size, uint32 Alignment, CClass* (*SuperClassFn)(), CClass::FactoryFunctionType FactoryFunc)
    {
        DEBUG_ASSERT(*OutClass == nullptr);

        *OutClass = Memory::New<CScriptClass>(ResolveClassPackage(Package), FName(Name), Size, Alignment, OF_None, FactoryFunc);
        LinkAndQueueStaticClass(*OutClass, SuperClassFn);
    }
    

    bool CField::HasMeta(const FName& Key) const
    {
        return Metadata.HasMetadata(Key);
    }

    const FString& CField::GetMeta(const FName& Key) const
    {
        return Metadata.GetMetadata(Key);
    }

    CClass* CClass::GetMetaClass() const
    {
        return StaticClass();
    }

    CObject* CClass::EmplaceInstance(void* Memory) const
    {
        DEBUG_ASSERT(FactoryFunction);
        return FactoryFunction(Memory);
    }

    CClass* CClass::GetSuperClass() const
    {
        return static_cast<CClass*>(GetSuperStruct());
    }

    // One recursive lock for every class, since building a CDO runs constructors that ask for others.
    static FRecursiveMutex GDefaultObjectMutex;

    CObject* CClass::GetDefaultObject() const
    {
        if (CObject* Existing = ClassDefaultObject.load(std::memory_order_acquire))
        {
            return Existing;
        }

        FRecursiveScopeLock Lock(GDefaultObjectMutex);

        // Inside the lock because the pass is not thread safe either, and it can build this very CDO.
        SettleDeferredRegistrations();

        // Re-read, since settling builds this one on the way past and another thread may have too.
        if (ClassDefaultObject.load(std::memory_order_relaxed) == nullptr)
        {
            const_cast<CClass*>(this)->CreateDefaultObject();
        }

        return ClassDefaultObject.load(std::memory_order_acquire);
    }

    void* CStruct::GetDefaultInstance()
    {
        if (DefaultInstance != nullptr)
        {
            return DefaultInstance;
        }

        FStructOps* Ops = GetStructOps();
        if (Ops == nullptr || !Ops->HasConstruct())
        {
            return nullptr;
        }

        // Process-lifetime allocation, mirroring how class CDOs are rooted and never released.
        const uint32 InstanceSize = GetSize();
        const uint32 InstanceAlign = GetAlignment();
        LUMINA_MEMORY_SCOPE("CObject");
        DefaultInstance = Memory::Malloc(InstanceSize, InstanceAlign);
        Ops->Construct(DefaultInstance);
        return DefaultInstance;
    }

    void* CClass::GetDefaultInstance()
    {
        return GetDefaultObject();
    }

    CObject* CClass::CreateDefaultObject()
    {
        DEBUG_ASSERT(ClassDefaultObject.load(std::memory_order_relaxed) == nullptr);

        // Only a constructor of this very class can land here, and handing it a half-built CDO is worse.
        if (bCreatingDefaultObject)
        {
            LOG_ERROR("'{}' asked for its own default object while that object was still being built.", GetName());
            return nullptr;
        }

        const TGuardValue<bool> Building(bCreatingDefaultObject, true);

        Link();

        FString DefaultObjectName = GetName().c_str();
        DefaultObjectName += "_CDO";

        FConstructCObjectParams Params(this);
        Params.Flags    |= OF_DefaultObject;
        Params.Name     = FName(DefaultObjectName);
        Params.Package  = GetPackage();
        Params.Guid     = FGuid::New();

        CObject* Created = StaticAllocateObject(Params);
        Created->AddToRoot();

        // Published before PostCreateCDO, which registers the CDO in tables that may ask for it back.
        ClassDefaultObject.store(Created, std::memory_order_release);

        Created->PostCreateCDO();

        return Created;
    }

    void CClass::DiscardDefaultObject()
    {
        FRecursiveScopeLock Lock(GDefaultObjectMutex);

        CObject* Discarded = ClassDefaultObject.exchange(nullptr, std::memory_order_acq_rel);
        if (Discarded == nullptr)
        {
            return;
        }

        // That root reference is the only strong one a CDO has, so un-rooting is the destruction.
        Discarded->RemoveFromRoot();
    }

    static CStruct* StaticGetBaseStructureInternal(const FName& Name)
    {
        CStruct* Result = static_cast<CStruct*>(FindObjectImpl(Name, CStruct::StaticClass()));
        return Result;
    }

    CStruct* TBaseStructure<FVector2>::Get()
    {
        static CStruct* Struct = StaticGetBaseStructureInternal("FVector2");
        return Struct;
    }

    CStruct* TBaseStructure<FVector3>::Get()
    {
        static CStruct* Struct = StaticGetBaseStructureInternal("FVector3");
        return Struct;
    }

    CStruct* TBaseStructure<FVector4>::Get()
    {
        static CStruct* Struct = StaticGetBaseStructureInternal("FVector4");
        return Struct;
    }

    CStruct* TBaseStructure<FQuat>::Get()
    {
        static CStruct* Struct = StaticGetBaseStructureInternal("FQuat");
        return Struct;
    }

}
