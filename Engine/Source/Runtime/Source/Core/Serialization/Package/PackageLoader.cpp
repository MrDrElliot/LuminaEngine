#include "RuntimePCH.h"
#include "PackageLoader.h"

#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"

namespace Lumina
{
    FArchive& FPackageLoader::operator<<(CObject*& Value)
    {
        FObjectPackageIndex Index;
        FArchive& Ar = *this;
        Ar << Index;
        
        Value = Package->IndexToObject(Index);
        
        return Ar;
    }

    FArchive& FPackageLoader::operator<<(FObjectHandle& Value)
    {
        FObjectPackageIndex Index;
        FArchive& Ar = *this;
        Ar << Index;

        CObject* Object = Package->IndexToObject(Index);
        Value = GObjectArray.GetHandleByObject(Object);
        Package->LoadObject(Object);
        
        return Ar;    
    }
}
