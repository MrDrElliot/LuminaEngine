#pragma once

#include "ObjectBase.h"
#include "ObjectMacros.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Serialization/Structured/StructuredArchive.h"


namespace Lumina
{
    class FProperty;
    class IStructuredArchive;
    class CClass;
    class CObject;
}

RUNTIME_API Lumina::CClass* Construct_CClass_Lumina_CObject();

namespace Lumina
{

    // Base object for all reflected Lumina types. Path format: <package-path>.<object-name>
    class CObject : public CObjectBase
    {
    public:

        friend CObject* StaticAllocateObject();

        DECLARE_CLASS(Lumina, CObject, CObject, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CObject)

        CObject() = default;
        
        RUNTIME_API CObject(EObjectFlags InFlags)
            : CObjectBase(InFlags)
        {}

        RUNTIME_API CObject(CClass* InClass, EObjectFlags InFlags, CPackage* Package, const FName& InName, const FGuid& GUID)
            :CObjectBase(InClass, InFlags, Package, InName, GUID)
        {}

        RUNTIME_API virtual void Serialize(FArchive& Ar);

        /** Structured-archive serialize (packaging, network). */
        RUNTIME_API virtual void Serialize(IStructuredArchive::FRecord Record);

        /** Called after constructor + property init. */
        RUNTIME_API virtual void PostInitProperties();

        /** Called after the CDO is created. */
        RUNTIME_API virtual void PostCreateCDO() {}

        RUNTIME_API virtual bool IsAsset() const { return false; }

        /** Asset binary vs text serialization. */
        RUNTIME_API virtual bool IsBinary() const { return true; }

        RUNTIME_API virtual void PreLoad() {}
        RUNTIME_API virtual void PostLoad() {}

        /** Property modified externally (e.g. editor). */
        RUNTIME_API virtual void PostPropertyChange(FProperty* ChangedProperty) {}

        RUNTIME_API virtual bool Rename(const FName& NewName, CPackage* NewPackage = nullptr);

        /** Templates a new object from this one; copies reflected properties only. */
        RUNTIME_API CObject* Duplicate();

        /** Copies reflected properties onto Other. Remap, when supplied, rewrites any object reference
         *  that lands in it -- pass the source -> copy table when duplicating a whole package, or the
         *  copy keeps pointing at the source's sub-objects. References outside the table stay as-is. */
        RUNTIME_API void CopyPropertiesTo(CObject* Other, const THashMap<CObject*, CObject*>* Remap = nullptr);
        
    private:

    };
}
