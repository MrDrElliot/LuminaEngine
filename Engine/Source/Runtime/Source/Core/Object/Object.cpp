#include "RuntimePCH.h"
#include "Object.h"
#include "Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Package/Package.h"

IMPLEMENT_CLASS(Lumina, CObject)

// The root of every class, so it is finished by hand rather than by IMPLEMENT_INTRINSIC_CLASS, which needs a base.
template<> RUNTIME_API Lumina::CClass* Lumina::StaticClass<Lumina::CObject>()
{
    static Lumina::CClass* Class = nullptr;
    if (Class == nullptr)
    {
        Class = Lumina::CObject::StaticClass();
        Lumina::FinishRegistration(Class);
    }
    ASSERT(Class->GetClass() != nullptr);
    return Class;
}

namespace Lumina
{
    void CObject::Serialize(FArchive& Ar)
    {
        if (CClass* Class = GetClass())
        {
            Class->SerializeProperties(Ar, this);
        }
    }

    void CObject::SerializeStructured(IStructuredArchive::FRecord& Record)
    {
        if (CClass* Class = GetClass())
        {
            Class->SerializeProperties(Record.EnterField("Properties").GetArchiver(), this);
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
        if (Duplicate == nullptr)
        {
            return nullptr;
        }
        if (CClass* Class = GetClass())
        {
            for (FProperty* Current : Class->GetProperties())
            {
                if (Current->IsDuplicateTransient())
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
            for (FProperty* Current : Class->GetProperties())
            {
                if (Current->IsDuplicateTransient())
                {
                    continue;
                }
                
                TVector<uint8> Bytes;

                // An array given the OBJECT base reads its size from the vtable pointer and destroys the vptr.
                {
                    void* ValuePtr = Current->GetValuePtr<void>(this);
                    FMemoryWriter Writer(Bytes);
                    FObjectProxyArchiver Proxy(Writer, true);
                    Current->Serialize(Proxy, ValuePtr);
                }

                {
                    void* ValuePtr = Current->GetValuePtr<void>(Other);
                    FMemoryReader Reader(Bytes);

                    // The buffer holds the source's GUIDs either way, so the swap applies once each has resolved.
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
