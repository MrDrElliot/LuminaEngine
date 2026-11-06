#pragma once

#include "ObjectBase.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
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


namespace Lumina
{

    // Base object for all reflected Lumina types. Path format: <package-path>.<object-name>
    class LUMINA_VISIBLE_TYPE CObject : public CObjectBase
    {
    public:

        friend CObject* StaticAllocateObject();

        DECLARE_CLASS(Lumina, CObject, CObject, "/Types/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CObject)

        CObject() = default;
        
        RUNTIME_API CObject(EObjectFlags InFlags)
            : CObjectBase(InFlags)
        {}

        RUNTIME_API CObject(CClass* InClass, EObjectFlags InFlags, CPackage* Package, const FName& InName, const FGuid& GUID)
            :CObjectBase(InClass, InFlags, Package, InName, GUID)
        {}

        RUNTIME_API virtual void Serialize(FArchive& Ar);

        // Reports references reflection cannot see, such as an ECS registry, so reinstancing and replacement reach them too.
        RUNTIME_API virtual void VisitAdditionalObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) {}

        RUNTIME_API virtual void SerializeStructured(IStructuredArchive::FRecord& Record);

        RUNTIME_API virtual void PostInitProperties();

        RUNTIME_API virtual void PostCreateCDO() {}

        RUNTIME_API virtual bool IsAsset() const { return false; }

        RUNTIME_API virtual void PreLoad() {}
        RUNTIME_API virtual void PostLoad() {}

        // Last chance to make streamed-out state resident, since anything still on disk at Serialize is saved empty.
        RUNTIME_API virtual void PreSave() {}

        RUNTIME_API virtual void PostPropertyChange(FProperty* ChangedProperty) {}

        // A reference sweep (delete, replace) rewrote at least one object this one points at, so state derived from it is stale.
        RUNTIME_API virtual void OnReferencesReplaced() {}

        // A reload that reshaped this object's class built it to replace Previous, whose values it already carries.
        RUNTIME_API virtual void OnReinstanced(CObject& Previous) {}

        RUNTIME_API virtual bool Rename(const FName& NewName, CPackage* NewPackage = nullptr);

        // Copies reflected properties only.
        RUNTIME_API CObject* Duplicate();

        // Remap rewrites references into a duplicated package, or the copy keeps pointing at the source's sub-objects.
        RUNTIME_API void CopyPropertiesTo(CObject* Other, const THashMap<CObject*, CObject*>* Remap = nullptr);
        
    private:

    };
}
