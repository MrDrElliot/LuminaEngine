#pragma once

#include "ObjectFlags.h"
#include "GUID/GUID.h"
#include "Containers/Name.h"


namespace Lumina
{
    class CPackage;
    class CClass;
    class CObject;

    struct FConstructCObjectParams
    {
        FConstructCObjectParams(const CClass* InClass)
            : Class(InClass)
            , Package(nullptr)
            , Flags(OF_None)
        {}
        
        const CClass*   Class;
        FGuid           Guid;
        CPackage*       Package;
        FName           Name;
        EObjectFlags    Flags;
    };
}
