#include "EditorPCH.h"
#include "SequenceFactory.h"

namespace Lumina
{
    CObject* CSequenceFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // No creation dialogue: bindings and tracks are authored against a live world in the Sequencer
        // edit mode, so there is nothing useful to ask for up front.
        return NewObject<CSequence>(Package, Name);
    }
}
