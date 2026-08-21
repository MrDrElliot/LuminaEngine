#include "EditorPCH.h"
#include "GeometryCollectionFactory.h"

namespace Lumina
{
    CObject* CGeometryCollectionFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // Created empty, then the user picks a source mesh and bakes in the Geometry Collection editor.
        return NewObject<CGeometryCollection>(Package, Name);
    }
}
