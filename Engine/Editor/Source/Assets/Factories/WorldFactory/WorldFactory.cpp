#include "EditorPCH.h"
#include "WorldFactory.h"

#include "Scene/DefaultScene.h"


namespace Lumina
{
    CObject* CWorldFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CWorld* World = NewObject<CWorld>(Package, Name);

        DefaultScene::PopulateStarterLevel(World);

        return World;
    }
}
