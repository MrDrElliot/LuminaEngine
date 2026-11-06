#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Paths/Paths.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "World/ECS/CommandBus.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/World.h"

using namespace Lumina;

namespace ManagedSystemContextTestsInternal
{
    using FRunFn         = int32(*)(void*, uint32, uint32, void*, uint32, int32*);
    using FDeferredFn    = void(*)(int32*);
    using FDiscardFn     = void(*)(void*);
    using FMarkerAliveFn = int32(*)();
    using FRaycastFn     = int32(*)(void*, int32*);

    struct FScene
    {
        TStrongObjectPtr<CWorld> World;
        ECS::FEntity Existing;
        ECS::FEntity Doomed;
        ECS::FEntity Source;
        ECS::FEntity CopyTarget;

        FScene()
        {
            RegisterPendingTypes();
            World = TStrongObjectPtr<CWorld>(NewObject<CWorld>(nullptr, FName("ManagedSystemContextWorld"), FGuid::New(), OF_Transient));
            ECS::FRegistry& Registry = World->GetEntityRegistry();

            Existing = Registry.Create();
            Registry.Emplace<STransformComponent>(Existing);
            Registry.Emplace<SNameComponent>(Existing);
            Doomed = Registry.Create();
            Source = Registry.Create();
            Registry.Emplace<SLifetimeComponent>(Source).Lifetime = 7.0f;
            CopyTarget = Registry.Create();
        }

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }

        void* SourceLifetime() const { return &Registry().Get<SLifetimeComponent>(Source); }
    };
}

using namespace ManagedSystemContextTestsInternal;

class FManagedSystemContextTest : public ::testing::Test
{
public:

    static void SetUpTestSuite()
    {
        Paths::InitializePaths();
        DotNet::Initialize();
    }

    static void TearDownTestSuite()
    {
        if (DotNet::IsInitialized())
        {
            DotNet::Shutdown();
        }
    }

    void SetUp() override
    {
        if (!DotNet::IsInitialized())
        {
            GTEST_SKIP() << "the managed host did not start in this process";
        }
        Run      = (FRunFn)DotNet::ResolveManagedExport("Test_SystemContextRun");
        Deferred = (FDeferredFn)DotNet::ResolveManagedExport("Test_SystemContextDeferred");
        ASSERT_NE(Run, nullptr);
        ASSERT_NE(Deferred, nullptr);
    }

    void ExpectDeferredRanWithTheWorld(const FScene& Scene) const
    {
        int32 Results[4] = {};
        Deferred(Results);
        EXPECT_EQ(Results[0], 1) << "Defer runs at the flush";
        EXPECT_EQ(Results[1], 1) << "the deferred callback's exclusive context reaches the world";
        EXPECT_EQ(Results[2], 1) << "a create inside the flush applies at once";
        EXPECT_TRUE(Scene.Registry().IsValid(static_cast<ECS::FEntity>((uint32)Results[3])));
    }

    void ExpectApplied(const FScene& Scene, ECS::FEntity Created) const
    {
        ECS::FRegistry& Registry = Scene.Registry();
        const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Created);
        ASSERT_NE(Transform, nullptr) << "Create builds the entity's transform";
        EXPECT_NEAR(Transform->GetLocalLocation().y, 2.0f, 1e-4f);
        ASSERT_NE(Registry.TryGet<SLifetimeComponent>(Created), nullptr);
        EXPECT_FLOAT_EQ(Registry.Get<SLifetimeComponent>(Created).Lifetime, 3.0f);
        ASSERT_NE(Registry.TryGet<SLifetimeComponent>(Scene.Existing), nullptr);
        EXPECT_FLOAT_EQ(Registry.Get<SLifetimeComponent>(Scene.Existing).Lifetime, 5.0f) << "the value filled through Emplace's ref lands";
        EXPECT_TRUE(Registry.HasAll<SDisabledTag>(Scene.Existing));
        EXPECT_FALSE(Registry.HasAll<SNameComponent>(Scene.Existing));
        EXPECT_NEAR(Registry.Get<STransformComponent>(Scene.Existing).GetLocalLocation().z, 4.0f, 1e-4f) << "Patch edits a component the system does not query";
        ASSERT_NE(Registry.TryGet<SLifetimeComponent>(Scene.CopyTarget), nullptr);
        EXPECT_FLOAT_EQ(Registry.Get<SLifetimeComponent>(Scene.CopyTarget).Lifetime, 7.0f) << "Emplace copies a component reached in place";
        EXPECT_FALSE(Registry.IsValid(Scene.Doomed));
    }

    FRunFn      Run = nullptr;
    FDeferredFn Deferred = nullptr;
};

TEST_F(FManagedSystemContextTest, ParallelStructuralChangesWaitForTheFlush)
{
    FScene Scene;
    const SSystemContext Context(Scene.World.Get());

    int32 Out[2] = {};
    {
        const ECS::FCommandBus::FDeferScope Defer(true);
        ASSERT_EQ(Run((void*)&Context, (uint32)Scene.Existing, (uint32)Scene.Doomed, Scene.SourceLifetime(), (uint32)Scene.CopyTarget, Out), 1);
    }
    EXPECT_EQ(Out[1], 1) << "an exclusive context forged inside a parallel system refuses the world";

    ECS::FRegistry& Registry = Scene.Registry();
    const ECS::FEntity Created = static_cast<ECS::FEntity>((uint32)Out[0]);
    EXPECT_EQ(Registry.TryGet<STransformComponent>(Created), nullptr) << "a parallel create is built at the flush";
    EXPECT_EQ(Registry.TryGet<SLifetimeComponent>(Scene.Existing), nullptr);
    EXPECT_FALSE(Registry.HasAll<SDisabledTag>(Scene.Existing));
    EXPECT_TRUE(Registry.HasAll<SNameComponent>(Scene.Existing));
    EXPECT_NEAR(Registry.Get<STransformComponent>(Scene.Existing).GetLocalLocation().z, 0.0f, 1e-4f) << "a parallel Patch waits for the flush";
    EXPECT_EQ(Registry.TryGet<SLifetimeComponent>(Scene.CopyTarget), nullptr);
    EXPECT_TRUE(Registry.IsValid(Scene.Doomed));

    int32 Before[4] = {};
    Deferred(Before);
    EXPECT_EQ(Before[0], 0) << "Defer waits for the flush";

    Scene.World->FlushCommands();
    ExpectApplied(Scene, Created);
    ExpectDeferredRanWithTheWorld(Scene);
}

TEST_F(FManagedSystemContextTest, ExclusiveStructuralChangesApplyAtOnce)
{
    FScene Scene;
    const SSystemContext Context(Scene.World.Get());

    int32 Out[2] = {};
    ASSERT_EQ(Run((void*)&Context, (uint32)Scene.Existing, (uint32)Scene.Doomed, Scene.SourceLifetime(), (uint32)Scene.CopyTarget, Out), 1);
    EXPECT_EQ(Out[1], 0) << "an exclusive system reaches the world";
    ExpectApplied(Scene, static_cast<ECS::FEntity>((uint32)Out[0]));

    int32 Before[4] = {};
    Deferred(Before);
    EXPECT_EQ(Before[0], 0) << "Defer still waits for the flush from an exclusive system";

    Scene.World->FlushCommands();
    ExpectDeferredRanWithTheWorld(Scene);
}

TEST_F(FManagedSystemContextTest, DiscardedDeferReleasesItsCallback)
{
    auto* Discard     = (FDiscardFn)DotNet::ResolveManagedExport("Test_SystemContextDeferDiscarded");
    auto* MarkerAlive = (FMarkerAliveFn)DotNet::ResolveManagedExport("Test_SystemContextMarkerAlive");
    ASSERT_NE(Discard, nullptr);
    ASSERT_NE(MarkerAlive, nullptr);

    FScene Scene;
    const SSystemContext Context(Scene.World.Get());
    {
        const ECS::FCommandBus::FDeferScope Defer(true);
        Discard((void*)&Context);
    }
    EXPECT_EQ(MarkerAlive(), 1) << "the pending command holds the callback";

    Scene.World->GetCommandBus().Discard();
    EXPECT_EQ(MarkerAlive(), 0) << "dropping the command frees the callback's handle";
}

TEST_F(FManagedSystemContextTest, ParallelRaycastHitsABody)
{
    auto* Raycast = (FRaycastFn)DotNet::ResolveManagedExport("Test_SystemContextRaycast");
    ASSERT_NE(Raycast, nullptr);

    if (Physics::GetPhysicsContext() == nullptr)
    {
        Physics::Initialize();
    }

    FScene Scene;
    ECS::FRegistry& Registry = Scene.Registry();
    Physics::IPhysicsScene* PhysicsScene = Scene.World->EnsurePhysicsScene();
    ASSERT_NE(PhysicsScene, nullptr);

    const ECS::FEntity Ball = Registry.Create();
    Registry.Emplace<STransformComponent>(Ball);
    SRigidBodyComponent& Body = Registry.Emplace<SRigidBodyComponent>(Ball);
    Body.bUseGravity = false;
    Registry.Emplace<SSphereColliderComponent>(Ball).Radius = 0.5f;
    PhysicsScene->Update(0.0);

    const SSystemContext Context(Scene.World.Get());
    int32 HitEntity = -1;
    int32 Hit = 0;
    {
        const ECS::FCommandBus::FDeferScope Defer(true);
        Hit = Raycast((void*)&Context, &HitEntity);
    }
    EXPECT_EQ(Hit, 1) << "a parallel system's ray reaches the physics scene";
    EXPECT_EQ((uint32)HitEntity, (uint32)Ball);
}
