#include "pch.h"
#include "WorldFactory.h"

#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"


namespace Lumina
{
    CObject* CWorldFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CWorld* World = NewObject<CWorld>(Package, Name);
        
        auto Entity = World->ConstructEntity("Environment");
        World->EmplaceComponent<SEnvironmentComponent>(Entity);
        
        Entity = World->ConstructEntity("DirectionalLight");
        World->EmplaceComponent<SDirectionalLightComponent>(Entity);

        Entity = World->ConstructEntity("SkyLight");
        World->EmplaceComponent<SSkyLightComponent>(Entity);
        
        return World;
    }
}
