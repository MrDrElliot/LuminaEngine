#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Metadata/PropertyMetadata.h"
#include "Package/Package.h"

namespace Lumina
{
    RUNTIME_API void AllocateStaticClass(const TCHAR* Package, const TCHAR* Name, CClass** OutClass, uint32 Size, uint32 Alignment, CClass* (*SuperClassFn)(), CClass::FactoryFunctionType FactoryFunc)
    {
        DEBUG_ASSERT(*OutClass == nullptr);
        
        CPackage* PackageObject = nullptr;

        if (Package && Package[0] != '\0')
        {
            PackageObject = FindObject<CPackage>(Package);
            if (PackageObject == nullptr)
            {
                PackageObject = NewObject<CPackage>(nullptr, Package);
            }
        }

        *OutClass = Memory::New<CClass>(PackageObject, FName(Name), Size, Alignment, OF_None, FactoryFunc);
        
        CClass* NewClass = *OutClass;
        CClass* SuperClass = SuperClassFn();
        bool bValidSuperClass = (SuperClass != NewClass);
        
        NewClass->SetSuperStruct(bValidSuperClass ? SuperClass : nullptr);

        NewClass->RegisterDependencies();
        NewClass->BeginRegister();
    }
    

    IMPLEMENT_INTRINSIC_CLASS(CClass, CStruct, RUNTIME_API)

    bool CField::HasMeta(const FName& Key) const
    {
        return Metadata.HasMetadata(Key);
    }

    const FString& CField::GetMeta(const FName& Key) const
    {
        return Metadata.GetMetadata(Key);
    }

    bool CClass::ConstructScriptProperties(void* Object) const
    {
        if (Object == nullptr || ScriptProperties.empty())
        {
            return false;
        }

        uint8* Base = static_cast<uint8*>(Object);
        for (FProperty* Property : ScriptLifecycleProperties)
        {
            Property->ConstructValue(Base + Property->Offset);
        }
        
        if (ClassDefaultObject != nullptr && ClassDefaultObject != Object)
        {
            const uint8* DefaultBase = reinterpret_cast<const uint8*>(ClassDefaultObject);
            for (FProperty* Property : ScriptProperties)
            {
                Property->CopyCompleteValue(Base + Property->Offset, DefaultBase + Property->Offset);
            }
        }
        return true;
    }

    void CClass::DestructScriptProperties(void* Object) const
    {
        if (Object == nullptr)
        {
            return;
        }
        for (FProperty* Property : ScriptLifecycleProperties)
        {
            Property->DestructValue(static_cast<uint8*>(Object) + Property->Offset);
        }
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

    CObject* CClass::GetDefaultObject() const
    {
        if (ClassDefaultObject == nullptr)
        {
            CClass* MutableThis = const_cast<CClass*>(this);
            MutableThis->CreateDefaultObject();
        }

        return ClassDefaultObject;
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
        DEBUG_ASSERT(ClassDefaultObject == nullptr);
        
        Link();
        
        FString DefaultObjectName = GetName().c_str();
        DefaultObjectName += "_CDO";
        
        FConstructCObjectParams Params(this);
        Params.Flags    |= OF_DefaultObject;
        Params.Name     = FName(DefaultObjectName);
        Params.Package  = GetPackage();
        Params.Guid     = FGuid::New();
        
        // It used to need a re-stamp here because construction dropped the flag.
        ClassDefaultObject = StaticAllocateObject(Params);
        ClassDefaultObject->AddToRoot();

        ClassDefaultObject->PostCreateCDO();
        
        return ClassDefaultObject;
    }
    
    void CClass::DiscardDefaultObject()
    {
        if (ClassDefaultObject == nullptr)
        {
            return;
        }
        // That root reference is the only strong one a CDO has, so un-rooting IS the destruction.
        CObject* Discarded = ClassDefaultObject;
        ClassDefaultObject = nullptr;
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
