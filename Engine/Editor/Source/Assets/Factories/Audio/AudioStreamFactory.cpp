#include "EditorPCH.h"
#include "AudioStreamFactory.h"

#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    CObject* CAudioStreamFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CAudioStream>(Package, Name);
    }
}
