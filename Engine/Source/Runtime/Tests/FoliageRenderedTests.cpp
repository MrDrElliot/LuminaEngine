#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/World.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"

using namespace Lumina;

TEST(FoliageRendered, UnrenderedFoliageLeavesTheSceneAndKeepsItsVersion)
{
    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, FName("FoliageRenderedTestWorld"), FGuid::New(), OF_Transient);
    TStrongObjectPtr<CWorld> WorldOwner = World;
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    const ECS::FEntity Entity = Registry.Create();
    SFoliageComponent& Foliage = Registry.Emplace<SFoliageComponent>(Entity);
    Foliage.Types.AddDefaulted();
    Foliage.Instances.SetNum(3);

    FScenePrimitiveSet Scene;
    Scene.Reset(&Registry);
    Scene.Sync(*World);
    ASSERT_EQ(Scene.GetFoliageInstanceCount(), 3u);

    const uint32 Version = Foliage.InstancesVersion;
    SetFoliageRendered(*World, Entity, Foliage, false);
    Scene.Sync(*World);
    EXPECT_EQ(Scene.GetFoliageInstanceCount(), 0u);
    EXPECT_EQ(Foliage.InstancesVersion, Version);
    EXPECT_EQ(Foliage.Instances.Num(), 3);

    SetFoliageRendered(*World, Entity, Foliage, true);
    Scene.Sync(*World);
    EXPECT_EQ(Scene.GetFoliageInstanceCount(), 3u);
    EXPECT_EQ(Foliage.InstancesVersion, Version);

    Scene.Shutdown(Registry);
}
