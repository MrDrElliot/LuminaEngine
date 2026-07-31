#include "pch.h"
#include "WorldFactory.h"

#include "UI/Tools/EditorEntityUtils.h"


namespace Lumina
{
    CObject* CWorldFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CWorld* World = NewObject<CWorld>(Package, Name);

        EditorEntityUtils::PopulateDefaultScene(World);

        return World;
    }
}
