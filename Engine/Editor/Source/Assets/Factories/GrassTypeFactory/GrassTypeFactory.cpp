#include "EditorPCH.h"
#include "GrassTypeFactory.h"

namespace Lumina
{
    CObject* CGrassTypeFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CGrassType>(Package, Name);
    }
}
