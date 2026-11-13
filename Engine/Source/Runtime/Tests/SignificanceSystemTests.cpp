#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <functional>

#include "Containers/Algorithm.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/World.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/PathFollowComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SignificanceSystem.h"
#include "World/Entity/Systems/SystemContext.h"

using namespace Lumina;

namespace
{
    struct FSignificanceTestWorld
    {
        CWorld*                  World  = nullptr;
        TStrongObjectPtr<CWorld> WorldOwner;
        CSignificanceSystem*     System = nullptr;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }

        FSignificanceTestWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("SignificanceSystemTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            Registry().Ctx().Emplace<CWorld*>(World);

            System = NewObject<CSignificanceSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->ConfigureSystem();

            Registry().Ctx().Get<FResolvedSceneView>().bHasView = true;
        }

        ~FSignificanceTestWorld()
        {
            WorldOwner.Reset();
        }

        ECS::FEntity SpawnAudioSource()
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity);
            Registry().Emplace<SAudioSourceComponent>(Entity);
            return Entity;
        }

        ECS::FEntity SpawnPathFollow()
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity);
            Registry().Emplace<SPathFollowComponent>(Entity);
            return Entity;
        }

        // Both pools claim it, the exact overlap CSignificanceSystem's Score lambda must not double-score.
        ECS::FEntity SpawnAudioSourceAndPathFollow()
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity);
            Registry().Emplace<SAudioSourceComponent>(Entity);
            Registry().Emplace<SPathFollowComponent>(Entity);
            return Entity;
        }

        void Update()
        {
            System->OnUpdate(SSystemContext());
        }

        const FEntitySignificance* Find(ECS::FEntity Entity) const
        {
            return Registry().Ctx().Get<FSignificanceState>().Scores.Find(Entity);
        }
    };

    double TimeUpdate(FSignificanceTestWorld& W)
    {
        // The first pass grows the score cache, so only a later pass times scoring alone.
        W.Update();
        const auto T0 = std::chrono::high_resolution_clock::now();
        W.Update();
        const auto T1 = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(T1 - T0).count();
    }

    double MedianOf(const std::function<double()>& Sample, int32 Repeats)
    {
        TVector<double> Samples;
        for (int32 Run = 0; Run < Repeats; ++Run)
        {
            Samples.Add(Sample());
        }
        Algo::Sort(Samples.begin(), Samples.end());
        return Samples[Samples.Num() / 2];
    }
}

TEST(SignificanceSystem, EntityInTwoConsumerPoolsIsScored)
{
    FSignificanceTestWorld W;
    const ECS::FEntity Entity = W.SpawnAudioSourceAndPathFollow();

    W.Update();

    const FEntitySignificance* Score = W.Find(Entity);
    ASSERT_NE(Score, nullptr);
    EXPECT_EQ(Score->Owner, Entity);
}

// A pool that revisits an already-stamped entity must pay for the IsFresh check only, not the full score.
TEST(SignificanceSystem, DoublePoolMembershipCostsNoMoreThanSinglePoolMembership)
{
    // Stays under the system's parallel grain, so both passes run on one thread.
    constexpr int32 kEntityCount = 2000;
    constexpr int32 kRepeats = 7;

    const double OnePoolMs = MedianOf([&]
    {
        FSignificanceTestWorld W;
        for (int32 Index = 0; Index < kEntityCount; ++Index)
        {
            W.SpawnAudioSource();
        }
        return TimeUpdate(W);
    }, kRepeats);

    const double TwoPoolMs = MedianOf([&]
    {
        FSignificanceTestWorld W;
        for (int32 Index = 0; Index < kEntityCount; ++Index)
        {
            W.SpawnAudioSourceAndPathFollow();
        }
        return TimeUpdate(W);
    }, kRepeats);

    std::printf("[bench] SignificanceSystem one pool %.4f ms, two pools same entities %.4f ms\n", OnePoolMs, TwoPoolMs);

    const double Ratio = TwoPoolMs / Math::Max(OnePoolMs, 0.0001);
    EXPECT_LT(Ratio, 1.3) << "an entity rescored by its second pool would cost close to 2x, not the near-1x of a single IsFresh check";
}
