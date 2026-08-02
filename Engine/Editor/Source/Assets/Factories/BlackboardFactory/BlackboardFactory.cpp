#include "EditorPCH.h"
#include "BlackboardFactory.h"

namespace Lumina
{
    CObject* CBlackboardFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CBlackboard>(Package, Name);
    }
}
