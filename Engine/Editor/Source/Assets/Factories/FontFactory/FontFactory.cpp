#include "EditorPCH.h"
#include "FontFactory.h"

#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    CObject* CFontFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CFont>(Package, Name);
    }
}
