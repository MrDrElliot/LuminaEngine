#include "EditorPCH.h"
#include "MaterialParameterCollectionFactory.h"

namespace Lumina
{
    CObject* CMaterialParameterCollectionFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CMaterialParameterCollection>(Package, Name);
    }
}
