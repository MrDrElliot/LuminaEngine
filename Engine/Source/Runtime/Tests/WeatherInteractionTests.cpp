#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "LogCountTestHelpers.h"
#include "World/World.h"
#include "World/Entity/Components/StaticMeshComponent.h"
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
        CWeatherInteractionSystem* System = nullptr;
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
            FWeatherContactPoint& Contact = Interactor.Contacts.EmplaceGetRef();
            Contact.Radius = 0.2f;

            System = NewObject<CWeatherInteractionSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->ConfigureSystem();
            System->OnStartup(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
        }

        ~FPondWorld()
        {
            System->OnTeardown(SExclusiveContext(FWorldTestAccess::SystemContext(*World)));
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

        ECS::FEntity SpawnWater(float X)
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity).SetLocation(FVector3(X, 0.0f, 0.0f));
            Registry().Emplace<SWaterComponent>(Entity).Extent = FVector2(20.0f, 20.0f);
            return Entity;
        }
    };
}

TEST(WeatherInteraction, AContactOverWaterReadsTheWaterSurface)
{
    FPondWorld Pond;
    const FWeatherInteractionState& State = Pond.Step(FVector3(1.0f, 0.5f, 2.0f));
    ASSERT_EQ(State.Contacts.Num(), 1);
    EXPECT_EQ(State.Contacts[0].Surface, EWeatherSurface::Water);
    EXPECT_NEAR(State.Contacts[0].SurfaceY, 0.0f, 1e-4f);
    EXPECT_TRUE(State.Impacts.IsEmpty());
}

TEST(WeatherInteraction, FallingIntoWaterSplashesOnce)
{
    FPondWorld Pond;
    Pond.Step(FVector3(0.0f, 1.0f, 0.0f));
    const FWeatherInteractionState& Entering = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    ASSERT_EQ(Entering.Impacts.Num(), 1);
    EXPECT_EQ(Entering.Impacts[0].Surface, EWeatherSurface::Water);
    EXPECT_NEAR(Entering.Impacts[0].Speed, 10.0f, 1e-3f);
    EXPECT_EQ(Entering.Impacts[0].Entity, Pond.Wader.Value);

    const FWeatherInteractionState& Resting = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(Resting.Impacts.IsEmpty());
}

TEST(WeatherInteraction, ATeleportDoesNotSplash)
{
    FPondWorld Pond;
    Pond.Step(FVector3(0.0f, 5.0f, 0.0f));
    const FWeatherInteractionState& State = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(State.Impacts.IsEmpty());
}

TEST(WeatherInteraction, WadingLeavesAWaterlineThatDriesOff)
{
    FPondWorld Pond;
    const FWeatherInteractionState& Wading = Pond.Step(FVector3(0.0f, -0.6f, 0.0f));
    ASSERT_EQ(Wading.Interactors.Num(), 1);
    EXPECT_EQ(Wading.Interactors[0].EntityID, Pond.Wader.Value);
    EXPECT_NEAR(Wading.Interactors[0].WaterlineHeight, 0.6f, 1e-4f);
    EXPECT_FLOAT_EQ(Wading.Interactors[0].WaterlineWetness, 1.0f);

    // Out of the pond the water line stays wet but starts to fade.
    const FWeatherInteractionState& Out = Pond.Step(FVector3(30.0f, 0.0f, 0.0f));
    ASSERT_EQ(Out.Interactors.Num(), 1);
    EXPECT_LT(Out.Interactors[0].WaterlineWetness, 1.0f);
    EXPECT_GT(Out.Interactors[0].WaterlineWetness, 0.9f);
}

TEST(WeatherInteraction, AWaterBodyCanOptOut)
{
    FPondWorld Pond;
    Pond.Registry().Get<SWaterComponent>(Pond.Pond).bInteractive = false;
    const FWeatherInteractionState& State = Pond.Step(FVector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(State.Contacts.IsEmpty());
    EXPECT_TRUE(State.Interactors.IsEmpty());
}

TEST(WeatherInteraction, WaterBodyPastTheSixteenthIsDroppedWithAWarning)
{
    FPondWorld Pond;

    // The pond plus sixteen more is one past the budget, and which body the cap drops depends on iteration order.
    TVector<float> PositionsX;
    PositionsX.Add(0.0f);
    for (int32 Index = 0; Index < 16; ++Index)
    {
        const float X = (float)(Index + 1) * 100.0f;
        Pond.SpawnWater(X);
        PositionsX.Add(X);
    }

    Logging::Flush();
    Logging::ClearLogQueue();
    int32 NumWithContact = 0;
    for (const float X : PositionsX)
    {
        const FWeatherInteractionState& State = Pond.Step(FVector3(X, 0.0f, 0.0f));
        if (!State.Contacts.IsEmpty())
        {
            ++NumWithContact;
        }
    }
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "CWeatherInteractionSystem", "interactive water bodies"), 1);
    EXPECT_EQ(NumWithContact, 16) << "exactly one of the seventeen water bodies, past the cap, is dropped and produces no contact";
}

TEST(WeatherInteraction, WetChildPastTheSixteenthIsNotMarkedWet)
{
    FPondWorld Pond;
    SWeatherInteractorComponent& Interactor = Pond.Registry().Get<SWeatherInteractorComponent>(Pond.Wader);
    Interactor.Soak = 1.0f;

    ECS::FRegistry& R = Pond.Registry();
    const auto AttachChild = [&](bool bMesh)
    {
        const ECS::FEntity Child = R.Create();
        R.Emplace<STransformComponent>(Child);
        if (bMesh)
        {
            R.Emplace<SStaticMeshComponent>(Child);
        }
        R.AttachChild(Child, Pond.Wader);
        return Child;
    };

    // Non-mesh children at both ends of the sibling list must not count toward the cap or trip the warning.
    TVector<ECS::FEntity> Children;
    AttachChild(false);
    for (int32 Index = 0; Index < 16; ++Index)
    {
        Children.Add(AttachChild(true));
    }
    AttachChild(false);

    Logging::Flush();
    Logging::ClearLogQueue();
    EXPECT_EQ(Pond.Step(FVector3(1000.0f, 0.0f, 0.0f)).Interactors.Num(), 17);
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "CWeatherInteractionSystem", "wet mesh children"), 0) << "sixteen mesh children fit the budget";

    Children.Add(AttachChild(true));
    const FWeatherInteractionState& State = Pond.Step(FVector3(1000.0f, 0.0f, 0.0f));
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "CWeatherInteractionSystem", "wet mesh children"), 1);

    // The wader plus sixteen of its seventeen mesh children are marked wet, whichever one the cap drops.
    ASSERT_EQ(State.Interactors.Num(), 17);
    int32 NumChildrenMarkedWet = 0;
    for (const ECS::FEntity Child : Children)
    {
        for (int32 StateIndex = 0; StateIndex < State.Interactors.Num(); ++StateIndex)
        {
            if (State.Interactors[StateIndex].EntityID == Child.Value)
            {
                ++NumChildrenMarkedWet;
                break;
            }
        }
    }
    EXPECT_EQ(NumChildrenMarkedWet, 16) << "exactly one of the seventeen mesh children, past the cap, is not marked wet";
}
