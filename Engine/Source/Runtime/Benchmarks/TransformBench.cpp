#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include <cstdio>

using namespace Lumina;

namespace
{
    double Nanos(Lumina::uint64 Start, Lumina::uint64 End, size_t Ops)
    {
        const double Total = (Lumina::PlatformTime::ToSeconds(End - Start) * 1e9);
        return Ops > 0 ? Total / (double)Ops : 0.0;
    }

    void MakeFlat(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Out, uint32 Count)
    {
        Out.reserve(Count);
        for (uint32 i = 0; i < Count; ++i)
        {
            const ECS::FEntity E = Registry.Create();
            Registry.Emplace<STransformComponent>(E).Bind(Registry, E);
            Out.push_back(E);
        }
    }

    // Children of one root, so every setter takes the hierarchical QueueDirtyTransform path.
    void MakeHierarchical(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Out, uint32 Count)
    {
        const ECS::FEntity Root = Registry.Create();
        Registry.Emplace<STransformComponent>(Root).Bind(Registry, Root);

        Out.reserve(Count);
        for (uint32 i = 0; i < Count; ++i)
        {
            const ECS::FEntity E = Registry.Create();
            Registry.Emplace<STransformComponent>(E);
            ECS::Utils::AddToParent(Registry, E, Root);
            Registry.Get<STransformComponent>(E).Bind(Registry, E);
            Out.push_back(E);
        }

        Registry.Get<STransformComponent>(Root).Bind(Registry, Root);
    }

    struct FPhaseTiming
    {
        double SetNs   = 0.0;
        double ResolveNs = 0.0;
    };

    FPhaseTiming RunFrames(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Entities, int32 Frames)
    {
        auto Storage = Registry.GetStorage<STransformComponent>();

        double SetTotal = 0.0;
        double ResolveTotal = 0.0;

        for (int32 Frame = 0; Frame < Frames; ++Frame)
        {
            const auto SetStart = Lumina::PlatformTime::Cycles();
            for (ECS::FEntity E : Entities)
            {
                Storage.Get(E).SetLocalLocation(FVector3((float)Frame, 0.0f, 0.0f));
            }
            const auto SetEnd = Lumina::PlatformTime::Cycles();

            ECS::Utils::ResolveAllDirtyTransforms(Registry);
            const auto ResolveEnd = Lumina::PlatformTime::Cycles();

            SetTotal     += (Lumina::PlatformTime::ToSeconds(SetEnd - SetStart) * 1e9);
            ResolveTotal += (Lumina::PlatformTime::ToSeconds(ResolveEnd - SetEnd) * 1e9);
        }

        FPhaseTiming Timing;
        Timing.SetNs     = SetTotal / (double)(Frames * (int32)Entities.size());
        Timing.ResolveNs = ResolveTotal / (double)(Frames * (int32)Entities.size());
        return Timing;
    }
}

// Run with --gtest_also_run_disabled_tests.
TEST(TransformBench, DISABLED_FlatSetterFrameCost)
{
    constexpr uint32 Count  = 100000;
    constexpr int32  Frames = 20;

    ECS::FRegistry Registry{};
    TVector<ECS::FEntity> Entities;
    MakeFlat(Registry, Entities, Count);

    const FPhaseTiming T = RunFrames(Registry, Entities, Frames);
    std::printf("\n  flat   %u entities x %d frames:  set %6.2f ns/op   resolve %6.2f ns/op\n",
                Count, Frames, T.SetNs, T.ResolveNs);
    SUCCEED();
}

TEST(TransformBench, DISABLED_HierarchicalSetterFrameCost)
{
    constexpr uint32 Count  = 100000;
    constexpr int32  Frames = 20;

    ECS::FRegistry Registry{};
    TVector<ECS::FEntity> Entities;
    MakeHierarchical(Registry, Entities, Count);

    const FPhaseTiming T = RunFrames(Registry, Entities, Frames);
    std::printf("\n  hier   %u entities x %d frames:  set %6.2f ns/op   resolve %6.2f ns/op\n",
                Count, Frames, T.SetNs, T.ResolveNs);
    SUCCEED();
}

// The render scene turns publishing on, so every resolved entity pays the moved channel too.
TEST(TransformBench, DISABLED_HierarchicalWithPublishMoved)
{
    constexpr uint32 Count  = 100000;
    constexpr int32  Frames = 20;

    ECS::FRegistry Registry{};
    TVector<ECS::FEntity> Entities;
    MakeHierarchical(Registry, Entities, Count);

    ECS::Utils::SetPublishMovedTransforms(Registry, true);

    double SetTotal = 0.0;
    double ResolveTotal = 0.0;
    double DrainTotal = 0.0;

    auto Storage = Registry.GetStorage<STransformComponent>();
    TVector<ECS::FEntity> Drained;

    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        const auto SetStart = Lumina::PlatformTime::Cycles();
        for (ECS::FEntity E : Entities)
        {
            Storage.Get(E).SetLocalLocation(FVector3((float)Frame, 0.0f, 0.0f));
        }
        const auto SetEnd = Lumina::PlatformTime::Cycles();

        ECS::Utils::ResolveAllDirtyTransforms(Registry);
        const auto ResolveEnd = Lumina::PlatformTime::Cycles();

        Drained.clear();
        ECS::Utils::DrainMovedTransforms(Registry, Drained);
        const auto DrainEnd = Lumina::PlatformTime::Cycles();

        SetTotal     += (Lumina::PlatformTime::ToSeconds(SetEnd - SetStart) * 1e9);
        ResolveTotal += (Lumina::PlatformTime::ToSeconds(ResolveEnd - SetEnd) * 1e9);
        DrainTotal   += (Lumina::PlatformTime::ToSeconds(DrainEnd - ResolveEnd) * 1e9);
    }

    const double Ops = (double)(Frames * (int32)Count);
    std::printf("\n  hier+publish %u x %d:  set %6.2f   resolve %6.2f   drain %6.2f ns/op\n",
                Count, Frames, SetTotal / Ops, ResolveTotal / Ops, DrainTotal / Ops);
    SUCCEED();
}

TEST(TransformBench, DISABLED_ParallelFlatSetters)
{
    constexpr uint32 Count  = 200000;
    constexpr int32  Frames = 20;

    ECS::FRegistry Registry{};
    TVector<ECS::FEntity> Entities;
    MakeFlat(Registry, Entities, Count);

    auto Storage = Registry.GetStorage<STransformComponent>();

    double Total = 0.0;
    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        const auto Start = Lumina::PlatformTime::Cycles();
        Task::ParallelFor(Count, [&](uint32 Index)
        {
            Storage.Get(Entities[Index]).SetLocalLocation(FVector3((float)Frame, 0.0f, 0.0f));
        }, 2048);
        const auto End = Lumina::PlatformTime::Cycles();

        ECS::Utils::ResolveAllDirtyTransforms(Registry);
        Total += (Lumina::PlatformTime::ToSeconds(End - Start) * 1e9);
    }

    std::printf("\n  parallel flat %u x %d:  set %6.2f ns/op\n",
                Count, Frames, Total / (double)(Frames * (int32)Count));
    SUCCEED();
}

// Isolates what MarkDirty adds on the flat path, which is dominated by the World = Local copy.
TEST(TransformBench, DISABLED_FlatWorldCopyCost)
{
    constexpr uint32 Count  = 100000;
    constexpr int32  Frames = 40;

    ECS::FRegistry Registry{};
    TVector<ECS::FEntity> Entities;
    MakeFlat(Registry, Entities, Count);

    auto Storage = Registry.GetStorage<STransformComponent>();

    double LocalOnly = 0.0;
    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        const auto Start = Lumina::PlatformTime::Cycles();
        for (ECS::FEntity E : Entities)
        {
            Storage.Get(E).LocalTransform.SetLocation(FVector3((float)Frame, 0.0f, 0.0f));
        }
        LocalOnly += (Lumina::PlatformTime::ToSeconds(Lumina::PlatformTime::Cycles() - Start) * 1e9);
    }

    double FullSetter = 0.0;
    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        const auto Start = Lumina::PlatformTime::Cycles();
        for (ECS::FEntity E : Entities)
        {
            Storage.Get(E).SetLocalLocation(FVector3((float)Frame, 0.0f, 0.0f));
        }
        FullSetter += (Lumina::PlatformTime::ToSeconds(Lumina::PlatformTime::Cycles() - Start) * 1e9);
        ECS::Utils::ResolveAllDirtyTransforms(Registry);
    }

    const double Ops = (double)(Frames * (int32)Count);
    std::printf("\n  flat setter breakdown (%u x %d):  local only %5.2f   full %5.2f   MarkDirty %5.2f ns/op\n",
                Count, Frames, LocalOnly / Ops, FullSetter / Ops, (FullSetter - LocalOnly) / Ops);
    std::printf("  sizeof=%zu  offsetof Local=%zu World=%zu (line 2 starts at 64)\n\n",
                sizeof(STransformComponent),
                offsetof(STransformComponent, LocalTransform),
                offsetof(STransformComponent, WorldTransform));
    SUCCEED();
}
