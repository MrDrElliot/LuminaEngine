#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/World.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/WeatherInteractorComponent.h"
#include "World/Entity/Systems/WeatherInteractionSystem.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    void BindWeatherTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    // A still pond at height zero and one bare interactor with a single contact sphere to move through it.
    struct FPondWorld
    {
        CWorld*                    World  = nullptr;
        SWeatherInteractionSystem* System = nullptr;
        TStrongObjectPtr<CWorld>   WorldOwner;
        ECS::FEntity               Pond;
        ECS::FEntity               Wader;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }

        FPondWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("WeatherInteractionTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            ECS::FRegistry& R = Registry();
            R.Ctx().Emplace<CWorld*>(World);
            R.GetSignals<STransformComponent>().OnConstruct.Connect<&BindWeatherTransform>();

            Pond = R.Create();
            R.Emplace<STransformComponent>(Pond);
            SWaterComponent& Water = R.Emplace<SWaterComponent>(Pond);
            Water.Extent        = FVector2(20.0f, 20.0f);
            Water.WaveAmplitude = 0.0f;

            Wader = R.Create();
            R.Emplace<STransformComponent>(Wader);
            SWeatherInteractorComponent& Interactor = R.Emplace<SWeatherInteractorComponent>(Wader);
            FWeatherContactPoint& Contact = Interactor.Contacts.emplace_back();
            Contact.Radius = 0.2f;

            System = NewObject<SWeatherInteractionSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->OnStartup(FWorldTestAccess::SystemContext(*World));
        }

        ~FPondWorld()
        {
            System->OnTeardown(FWorldTestAccess::SystemContext(*World));
            System->SetOwningWorld(nullptr);
            WorldOwner.Reset();
        }

        const FWeatherInteractionState& Step(const FVector3& Location, double DeltaTime = 0.1)
        {
            Registry().Get<STransformComponent>(Wader).SetLocation(Location);
            SSystemContext Context = FWorldTestAccess::SystemContext(*World);
            Context.DeltaTime = DeltaTime;
            System->OnUpdate(Context);
            return *World->TryGetSingleton<FWeatherInteractionState>();
        }
    };
}

TEST(WeatherInteraction, AContactOverWaterReadsTheWaterSurface)
{
    FPondWorld Pond;
    const FWeatherInteractionState& State = Pond.Step(FVector3(1.0f, 0.5f, 2.0f));
    ASSERT_EQ(State.Contacts.size(), 1u);
    EXPECT_EQ(State.Contacts[0].Surface, EWeatherSurface::Water);
    EXPECT_NEAR(State.Contacts[0].SurfaceY, 0.0f, 1e-4f);
    EXPECT_TRUE(State.Impacts.empty());
}

TEST(WeatherInteraction, FallingIntoWaterSplashesOnce)
{
    FPondWorld Pond;
    Pond.Step(FVector3(0.0f, 1.0f, 0.0f));
    const FWeatherInteractionState& Entering = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    ASSERT_EQ(Entering.Impacts.size(), 1u);
    EXPECT_EQ(Entering.Impacts[0].Surface, EWeatherSurface::Water);
    EXPECT_NEAR(Entering.Impacts[0].Speed, 10.0f, 1e-3f);
    EXPECT_EQ(Entering.Impacts[0].Entity, Pond.Wader.Value);

    const FWeatherInteractionState& Resting = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(Resting.Impacts.empty());
}

TEST(WeatherInteraction, ATeleportDoesNotSplash)
{
    FPondWorld Pond;
    Pond.Step(FVector3(0.0f, 5.0f, 0.0f));
    const FWeatherInteractionState& State = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(State.Impacts.empty());
}

TEST(WeatherInteraction, WadingLeavesAWaterlineThatDriesOff)
{
    FPondWorld Pond;
    const FWeatherInteractionState& Wading = Pond.Step(FVector3(0.0f, -0.6f, 0.0f));
    ASSERT_EQ(Wading.Interactors.size(), 1u);
    EXPECT_EQ(Wading.Interactors[0].EntityID, Pond.Wader.Value);
    EXPECT_NEAR(Wading.Interactors[0].WaterlineHeight, 0.6f, 1e-4f);
    EXPECT_FLOAT_EQ(Wading.Interactors[0].WaterlineWetness, 1.0f);

    // Out of the pond the water line stays wet but starts to fade.
    const FWeatherInteractionState& Out = Pond.Step(FVector3(30.0f, 0.0f, 0.0f));
    ASSERT_EQ(Out.Interactors.size(), 1u);
    EXPECT_LT(Out.Interactors[0].WaterlineWetness, 1.0f);
    EXPECT_GT(Out.Interactors[0].WaterlineWetness, 0.9f);
}

TEST(WeatherInteraction, AWaterBodyCanOptOut)
{
    FPondWorld Pond;
    Pond.Registry().Get<SWaterComponent>(Pond.Pond).bInteractive = false;
    const FWeatherInteractionState& State = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(State.Contacts.empty());
    EXPECT_TRUE(State.Interactors.empty());
}
