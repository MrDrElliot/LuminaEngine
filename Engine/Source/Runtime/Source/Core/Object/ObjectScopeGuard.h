#pragma once
#include "ObjectBase.h"


namespace Lumina
{
    class CObjectBase;

    /** Temporarily stops an object from being destroyed. */
    struct FObjectScopeGuard
    {
        FObjectScopeGuard(CObjectBase* InObject)
            : Object(InObject)
        {
            if (Object)
            {
                Object->AddToRoot();
            }
        }

        ~FObjectScopeGuard()
        {
            if (Object)
            {
                Object->RemoveFromRoot();
            }
        }
        
        CObjectBase* Object;
    };
}
