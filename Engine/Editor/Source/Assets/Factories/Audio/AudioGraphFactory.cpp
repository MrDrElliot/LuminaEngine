#include "EditorPCH.h"
#include "AudioGraphFactory.h"

namespace Lumina
{
    CObject* CAudioGraphFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CAudioGraph>(Package, Name);
    }
}
