#include <gtest/gtest.h>

#include "Containers/HashTable.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

#include "Scripting/ScriptableTest.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/World.h"

using namespace Lumina;

namespace
{
    // The driver only stores and hands back the pointer, so a sentinel proves ownership without a world.
    CWorld* const SentinelWorld = reinterpret_cast<CWorld*>(0x5150);

    // Flips the gate for one test and puts it back, so discovery in any other test still skips the types.
    struct FCreationGate
    {
        FCreationGate()
        {
            // The harness only starts the object system, so the deferred class pass has to run here.
            ProcessNewlyLoadedCObjects();
            CEntitySystemTest::bAllowCreation = true;
        }

        ~FCreationGate() { CEntitySystemTest::bAllowCreation = false; }
    };

    // Discovery reaches every system class in the process, so everything but the test types is disabled.
    THashSet<FName> AllButTestSystems()
    {
        THashSet<FName> Disabled;
        EntitySystems::ForEachSystemClass([&Disabled](CClass* Class)
        {
            if (Class != CEntitySystemTest::StaticClass() && Class != CEntitySystemExclusiveTest::StaticClass())
            {
                Disabled.insert(Class->GetName());
            }
        });
        return Disabled;
    }

    CEntitySystemTest* FindTestSystem(const TVector<TObjectPtr<CEntitySystem>>& Systems)
    {
        return Cast<CEntitySystemTest>(EntitySystems::Find(Systems, CEntitySystemTest::StaticClass()));
    }
}

TEST(EntitySystem, TeardownOnlyRunsForASystemThatStarted)
{
    FCreationGate Gate;

    TVector<TObjectPtr<CEntitySystem>> Systems;
    EntitySystems::CreateMissing(*SentinelWorld, AllButTestSystems(), Systems);

    CEntitySystemTest* Created = FindTestSystem(Systems);
    ASSERT_NE(Created, nullptr);

    EntitySystems::DestroyAll(Systems);

    EXPECT_EQ(Created->TeardownCount, 0) << "a system that never started must not be torn down";
}

TEST(EntitySystem, ConfigureDeclaresStagesAndAccess)
{
    FCreationGate Gate;

    TVector<TObjectPtr<CEntitySystem>> Systems;
    EntitySystems::CreateMissing(*SentinelWorld, AllButTestSystems(), Systems);

    CEntitySystemTest* Created = FindTestSystem(Systems);
    ASSERT_NE(Created, nullptr) << "a system class that is not disabled was not created";

    EXPECT_EQ(Created->GetWorld(), SentinelWorld) << "the owning world is set before Configure runs";
    EXPECT_EQ(Created->ConfigureCount, 1);

    const FUpdatePriorityList& Priorities = Created->GetPriorities();
    EXPECT_TRUE(Priorities.IsStageEnabled(EUpdateStage::PrePhysics));
    EXPECT_TRUE(Priorities.IsStageEnabled(EUpdateStage::FrameEnd));
    EXPECT_FALSE(Priorities.IsStageEnabled(EUpdateStage::PostPhysics));
    EXPECT_EQ(Priorities.GetPriorityForStage(EUpdateStage::PrePhysics), (uint8)EUpdatePriority::High);
    EXPECT_EQ(Priorities.GetPriorityForStage(EUpdateStage::FrameEnd), (uint8)EUpdatePriority::Default);

    // The typed declaration and the name keyed one a script uses must reach the same set.
    const FSystemAccess& Access = Created->GetAccess();
    EXPECT_FALSE(Access.bExclusive) << "a system that declared access must not fall back to exclusive";
    EXPECT_TRUE(Access.DeclaresWrite((uint32)ECS::GetComponentTypeID<STransformComponent>()));
    EXPECT_TRUE(Access.DeclaresRead((uint32)ECS::GetComponentTypeID<SStaticMeshComponent>()));
    EXPECT_FALSE(Access.DeclaresWrite((uint32)ECS::GetComponentTypeID<SStaticMeshComponent>()));

    // Without one per declared type the lazy assure rehashes the shared pool map inside a parallel batch.
    EXPECT_EQ(Access.PoolAssurers.size(), 2u) << "a name keyed declaration must carry a pool assurer too";

    EntitySystems::DestroyAll(Systems);
}

TEST(EntitySystem, DeclaringNoAccessRunsExclusive)
{
    FCreationGate Gate;

    TVector<TObjectPtr<CEntitySystem>> Systems;
    EntitySystems::CreateMissing(*SentinelWorld, AllButTestSystems(), Systems);

    CEntitySystem* Exclusive = EntitySystems::Find(Systems, CEntitySystemExclusiveTest::StaticClass());
    ASSERT_NE(Exclusive, nullptr);

    EXPECT_TRUE(Exclusive->GetAccess().bExclusive) << "declaring nothing is not a claim to be honest about";
    EXPECT_TRUE(FSystemAccess::Conflicts(Exclusive->GetAccess(), FindTestSystem(Systems)->GetAccess()));

    EntitySystems::DestroyAll(Systems);
}

TEST(EntitySystem, CreateMissingIsIdempotent)
{
    FCreationGate Gate;

    TVector<TObjectPtr<CEntitySystem>> Systems;
    const THashSet<FName> Disabled = AllButTestSystems();

    EXPECT_EQ(EntitySystems::CreateMissing(*SentinelWorld, Disabled, Systems), 2);
    const size_t CountAfterFirst = Systems.size();

    CEntitySystemTest* Created = FindTestSystem(Systems);
    ASSERT_NE(Created, nullptr);

    EXPECT_EQ(EntitySystems::CreateMissing(*SentinelWorld, Disabled, Systems), 0)
        << "a second pass must not duplicate a system that already exists";
    EXPECT_EQ(Systems.size(), CountAfterFirst);
    EXPECT_EQ(Created->ConfigureCount, 1) << "an existing system must not be configured twice";

    EntitySystems::DestroyAll(Systems);
}

TEST(EntitySystem, StartupRunsOnceAndTeardownFollows)
{
    FCreationGate Gate;

    TVector<TObjectPtr<CEntitySystem>> Systems;
    EntitySystems::CreateMissing(*SentinelWorld, AllButTestSystems(), Systems);

    CEntitySystemTest* Created = FindTestSystem(Systems);
    ASSERT_NE(Created, nullptr);
    EXPECT_EQ(Created->StartupCount, 0) << "creating a system must not start it";

    EntitySystems::StartupPending(Systems);
    EntitySystems::StartupPending(Systems);
    EXPECT_EQ(Created->StartupCount, 1) << "a system already started must not start again";

    EntitySystems::DestroyAll(Systems);

    EXPECT_EQ(Created->TeardownCount, 1);
    EXPECT_EQ(Created->GetWorld(), nullptr) << "teardown must unlink the world it no longer belongs to";
    EXPECT_TRUE(Systems.empty());
}

TEST(EntitySystem, DropDisabledRemovesOnlyTheNamedSystem)
{
    FCreationGate Gate;

    TVector<TObjectPtr<CEntitySystem>> Systems;
    EntitySystems::CreateMissing(*SentinelWorld, AllButTestSystems(), Systems);

    EntitySystems::StartupPending(Systems);

    CEntitySystemTest* Created = FindTestSystem(Systems);
    ASSERT_NE(Created, nullptr);

    THashSet<FName> Disabled;
    Disabled.insert(CEntitySystemTest::StaticClass()->GetName());
    EntitySystems::DropDisabled(Disabled, Systems);

    EXPECT_EQ(FindTestSystem(Systems), nullptr);
    EXPECT_EQ(Created->TeardownCount, 1) << "a disabled system tears down rather than being dropped";
    EXPECT_NE(EntitySystems::Find(Systems, CEntitySystemExclusiveTest::StaticClass()), nullptr);

    EntitySystems::DestroyAll(Systems);
}

TEST(EntitySystem, DropScriptedLeavesNativeSystemsAlone)
{
    FCreationGate Gate;

    TVector<TObjectPtr<CEntitySystem>> Systems;
    EntitySystems::CreateMissing(*SentinelWorld, AllButTestSystems(), Systems);

    const size_t Before = Systems.size();
    ASSERT_GT(Before, 0u);

    // Nothing here is backed by a script class, so a reload rebuild must not disturb the native ones.
    EntitySystems::DropScripted(Systems);

    EXPECT_EQ(Systems.size(), Before);
    EXPECT_NE(FindTestSystem(Systems), nullptr);

    EntitySystems::DestroyAll(Systems);
}

namespace
{
    struct FSinkProbeEvent { int Value = 0; };

    struct FSinkProbeListener
    {
        int Seen = 0;
        void OnEvent(const FSinkProbeEvent& Event) { Seen += Event.Value; }
    };
}

// Returning the sink by value swallowed the connect, which left the auto-activate camera unbound.
static_assert(std::is_reference_v<decltype(std::declval<const FSystemContext&>().EventSink<FSinkProbeEvent>())>,
    "FSystemContext::EventSink must hand back the dispatcher's sink, not a copy of it.");

// Every wrapper below forwards a signal that lives in the registry, so a deduced value would drop the connect.
static_assert(std::is_reference_v<decltype(std::declval<CWorld&>().OnConstruct<SStaticMeshComponent>())>,
    "CWorld::OnConstruct must hand back the registry's signal, not a copy of it.");
static_assert(std::is_reference_v<decltype(std::declval<CWorld&>().OnDestroy<SStaticMeshComponent>())>,
    "CWorld::OnDestroy must hand back the registry's signal, not a copy of it.");
static_assert(std::is_reference_v<decltype(std::declval<CWorld&>().OnUpdate<SStaticMeshComponent>())>,
    "CWorld::OnUpdate must hand back the registry's signal, not a copy of it.");
static_assert(std::is_reference_v<decltype(std::declval<CWorld&>().OnEntityConstruct())>,
    "CWorld::OnEntityConstruct must hand back the registry's signal, not a copy of it.");
static_assert(std::is_reference_v<decltype(std::declval<CWorld&>().OnEntityDestroy())>,
    "CWorld::OnEntityDestroy must hand back the registry's signal, not a copy of it.");

TEST(EventDispatcher, ConnectingThroughTheSinkReachesTrigger)
{
    ECS::FEventDispatcher Dispatcher;
    FSinkProbeListener Listener;

    Dispatcher.Sink<FSinkProbeEvent>().Connect<&FSinkProbeListener::OnEvent>(&Listener);
    Dispatcher.Trigger<FSinkProbeEvent>(FSinkProbeEvent{3});

    EXPECT_EQ(Listener.Seen, 3) << "a listener connected to the dispatcher's sink never ran";
}

TEST(EventDispatcher, ConnectingThroughACopyOfTheSinkIsLost)
{
    ECS::FEventDispatcher Dispatcher;
    FSinkProbeListener Listener;

    ECS::TEventSink<FSinkProbeEvent> Copy = Dispatcher.Sink<FSinkProbeEvent>();
    Copy.Connect<&FSinkProbeListener::OnEvent>(&Listener);
    Dispatcher.Trigger<FSinkProbeEvent>(FSinkProbeEvent{3});

    EXPECT_EQ(Listener.Seen, 0) << "the copy must not share the dispatcher's listener list";
}
