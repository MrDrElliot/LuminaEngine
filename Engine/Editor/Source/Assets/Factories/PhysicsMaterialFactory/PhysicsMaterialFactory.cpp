#include "EditorPCH.h"
#include "PhysicsMaterialFactory.h"

namespace Lumina
{
    CObject* CPhysicsMaterialFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CPhysicsMaterial>(Package, Name);
    }
}
