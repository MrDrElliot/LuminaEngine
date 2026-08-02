#include "RuntimePCH.h"
#include "ObjectHandle.h"

#include "ObjectArray.h"

namespace Lumina
{
    FObjectHandle::FObjectHandle(const CObjectBase* InObject)
    {
        if (InObject == nullptr)
        {
            *this = FObjectHandle();
            return;
        }
        *this = GObjectArray.GetHandleByIndex(InObject->GetInternalIndex());
    }

    CObject* FObjectHandle::Resolve() const
    {
        return (CObject*)GObjectArray.ResolveHandle(*this);
    }
}
