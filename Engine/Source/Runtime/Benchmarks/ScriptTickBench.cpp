#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include <cstdio>

#include "Platform/Time/PlatformTime.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableTest.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaScriptTickBench
{
    using namespace Lumina;

    constexpr uint32 kEntities = 20'000;
    constexpr int32  kFrames   = 200;

    double NanosPerOp(uint64 Start, uint64 End, size_t Ops)
    {
        return Ops > 0 ? (PlatformTime::ToSeconds(End - Start) * 1e9) / (double)Ops : 0.0;
    }

    // The driver's own cost per script, with a C++ body that does nothing but count.
    TEST(ScriptTickBench, TickPerScript)
    {
        ECS::FRegistry Registry{};
        for (uint32 i = 0; i < kEntities; ++i)
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<STransformComponent>(Entity).Bind(Registry, Entity);
            EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass());
        }

        // Drains OnReady, so the timed frames are the steady state.
        EntityScripts::Tick(Registry, 0.016f, EScriptUpdatePhase::PrePhysics);

        const uint64 PreStart = PlatformTime::Cycles();
        for (int32 Frame = 0; Frame < kFrames; ++Frame)
        {
            EntityScripts::Tick(Registry, 0.016f, EScriptUpdatePhase::PrePhysics);
        }
        const uint64 PreEnd = PlatformTime::Cycles();

        for (int32 Frame = 0; Frame < kFrames; ++Frame)
        {
            EntityScripts::Tick(Registry, 0.016f, EScriptUpdatePhase::PostPhysics);
        }
        const uint64 PostEnd = PlatformTime::Cycles();

        std::printf("\n  tick %u scripts: PrePhysics %6.2f ns/script   PostPhysics %6.2f ns/script\n", kEntities,
            NanosPerOp(PreStart, PreEnd, (size_t)kEntities * kFrames), NanosPerOp(PreEnd, PostEnd, (size_t)kEntities * kFrames));
    }

    void RunMoverWrites(bool bPublish)
    {
        ECS::FRegistry Registry{};
        TVector<ECS::FEntity> Entities;
        Entities.reserve(kEntities);
        for (uint32 i = 0; i < kEntities; ++i)
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<STransformComponent>(Entity).Bind(Registry, Entity);
            Entities.push_back(Entity);
        }

        ECS::Utils::SetPublishMovedTransforms(Registry, bPublish);
        auto Storage = Registry.GetStorage<STransformComponent>();
        TVector<ECS::FEntity> Drained;

        double LocationNs = 0.0;
        double YawNs = 0.0;
        double DrainNs = 0.0;
        for (int32 Frame = 0; Frame < kFrames; ++Frame)
        {
            const uint64 Start = PlatformTime::Cycles();
            for (ECS::FEntity Entity : Entities)
            {
                Storage.Get(Entity).SetLocalLocation(FVector3((float)Frame, 1.0f, 2.0f));
            }
            const uint64 LocationEnd = PlatformTime::Cycles();
            for (ECS::FEntity Entity : Entities)
            {
                Storage.Get(Entity).AddYaw(1.5f);
            }
            const uint64 YawEnd = PlatformTime::Cycles();

            Drained.clear();
            ECS::Utils::DrainMovedTransforms(Registry, Drained);
            ECS::Utils::ResolveAllDirtyTransforms(Registry);
            const uint64 DrainEnd = PlatformTime::Cycles();

            LocationNs += NanosPerOp(Start, LocationEnd, kEntities);
            YawNs      += NanosPerOp(LocationEnd, YawEnd, kEntities);
            DrainNs    += NanosPerOp(YawEnd, DrainEnd, kEntities);
        }

        std::printf("\n  %u movers (publish %d): SetLocalLocation %6.2f ns   AddYaw %6.2f ns   drain %6.2f ns (drained %zu)\n", kEntities, (int)bPublish,
            LocationNs / kFrames, YawNs / kFrames, DrainNs / kFrames, Drained.size());
    }

    // What the bench's Mover asks of the engine per entity, with and without the render scene's moved channel.
    TEST(ScriptTickBench, MoverWrites)
    {
        RunMoverWrites(false);
        RunMoverWrites(true);
    }
}
