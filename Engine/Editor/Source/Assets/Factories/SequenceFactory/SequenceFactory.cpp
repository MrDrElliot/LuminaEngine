#include "EditorPCH.h"
#include "SequenceFactory.h"

namespace Lumina
{
    CObject* CSequenceFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // Bindings and tracks are authored against a live world, so there is nothing to ask for up front.
        return NewObject<CSequence>(Package, Name);
    }
}
