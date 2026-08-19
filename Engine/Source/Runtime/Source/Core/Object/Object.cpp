#include "RuntimePCH.h"
#include "Object.h"
#include "Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Package/Package.h"

/** Low level CObject registration. */
extern Lumina::FClassRegistrationInfo Registration_Info_CClass_Lumina_CObject;
    
Lumina::CClass* Construct_CClass_Lumina_CObject()
{
    if (!Registration_Info_CClass_Lumina_CObject.OuterSingleton)
    {
        Registration_Info_CClass_Lumina_CObject.OuterSingleton = Lumina::CObject::StaticClass();
        Lumina::CObjectForceRegistration(Registration_Info_CClass_Lumina_CObject.OuterSingleton);
    }
    ASSERT(Registration_Info_CClass_Lumina_CObject.OuterSingleton->GetClass() != nullptr);
    return Registration_Info_CClass_Lumina_CObject.OuterSingleton;
}
    
IMPLEMENT_CLASS(Lumina, CObject)

namespace Lumina
{
    void CObject::Serialize(FArchive& Ar)
    {
        if (CClass* Class = GetClass())
        {
            Class->SerializeTaggedProperties(Ar, this);
        }
    }

    void CObject::SerializeStructured(IStructuredArchive::FRecord& Record)
    {
        if (CClass* Class = GetClass())
        {
            Class->SerializeTaggedProperties(Record.EnterField("Properties").GetArchiver(), this);
        }
    }

    void CObject::PostInitProperties()
    {
        
    }

    bool CObject::Rename(const FName& NewName, CPackage* NewPackage)
    {
		if (NewName == GetName() && NewPackage == GetPackage())
        {
            return true;
        }
        
        HandleNameChange(NewName, NewPackage);
        return true;
    }

    CObject* CObject::Duplicate()
    {
        CObject* Duplicate = NewObject(GetClass(), GetPackage());
        if (CClass* Class = GetClass())
        {
            for (FProperty* Current = Class->LinkedProperty; Current; Current = (FProperty*)Current->Next)
            {
                if (Current->HasMetadata("DuplicateTransient"))
                {
                    continue;
                }
                
                
                TVector<uint8> Bytes;
                
                {
                    void* ValuePtr = Current->GetValuePtr<void>(this);
                    FMemoryWriter Writer(Bytes);
                    FObjectProxyArchiver WriterProxy(Writer, true);
                    Current->Serialize(WriterProxy, ValuePtr);
                }

                {
                    void* ValuePtr = Current->GetValuePtr<void>(Duplicate);
                    FMemoryReader Reader(Bytes);
                    FObjectProxyArchiver ReaderProxy(Reader, true);
                    Current->Serialize(ReaderProxy, ValuePtr);
                }
            }
        }
        
        return Duplicate;
    }

    void CObject::CopyPropertiesTo(CObject* Other, const THashMap<CObject*, CObject*>* Remap)
    {
        ASSERT(Other->GetClass() == GetClass());
        
        if (CClass* Class = GetClass())
        {
            for (FProperty* Current = Class->LinkedProperty; Current; Current = (FProperty*)Current->Next)
            {
                if (Current->HasMetadata("DuplicateTransient"))
                {
                    continue;
                }
                
                TVector<uint8> Bytes;
                
                const bool bContainer = Current->IsA(EPropertyTypeFlags::Vector) || Current->IsA(EPropertyTypeFlags::Map);
                {
                    void* ValuePtr = bContainer ? this : Current->GetValuePtr<void>(this);
                    FMemoryWriter Writer(Bytes);
                    FObjectProxyArchiver Proxy(Writer, true);
                    Current->Serialize(Proxy, ValuePtr);
                }

                {
                    void* ValuePtr = bContainer ? Other : Current->GetValuePtr<void>(Other);
                    FMemoryReader Reader(Bytes);

                    // Remapping happens on the READ: the buffer holds the source's GUIDs either way,
                    // and the swap is applied once each GUID has resolved back to an object.
                    if (Remap != nullptr)
                    {
                        FObjectRemapArchiver Proxy(Reader, *Remap);
                        Current->Serialize(Proxy, ValuePtr);
                    }
                    else
                    {
                        FObjectProxyArchiver Proxy(Reader, true);
                        Current->Serialize(Proxy, ValuePtr);
                    }
                }
            }
        }
    }
}
