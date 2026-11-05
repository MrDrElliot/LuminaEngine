#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectCore.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptableTest.h"
#include "World/ECS/EventQueue.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/World.h"

using namespace Lumina;

namespace ManagedSystemParamTestsInternal
{
    template<typename T>
    uint32 IdOf()
    {
        return static_cast<uint32>(ECS::GetComponentTypeID<T>());
    }

    using FBindFn    = void*(*)(void*);
    using FRunFn     = int32(*)(void*, uint32, uint32, int32*);
    using FReleaseFn = void(*)(void*);
}

using namespace ManagedSystemParamTestsInternal;

class FManagedSystemParamTest : public ::testing::Test
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
    }
};

TEST_F(FManagedSystemParamTest, CSharpFieldsDeclareBindAndIterateLikeCppMembers)
{
    auto* Bind    = (FBindFn)DotNet::ResolveManagedExport("Test_SystemParamsBind");
    auto* Run     = (FRunFn)DotNet::ResolveManagedExport("Test_SystemParamsRun");
    auto* Release = (FReleaseFn)DotNet::ResolveManagedExport("Test_SystemParamsRelease");
    ASSERT_NE(Bind, nullptr);
    ASSERT_NE(Run, nullptr);
    ASSERT_NE(Release, nullptr);

    RegisterPendingTypes();
    TStrongObjectPtr<CWorld> World(NewObject<CWorld>(nullptr, FName("ManagedSystemParamWorld"), FGuid::New(), OF_Transient));
    TStrongObjectPtr<CEntitySystemTest> System(NewObject<CEntitySystemTest>(nullptr, NAME_None, FGuid::New(), OF_Transient));
    System->SetOwningWorld(World.Get());
    System->ConfigureSystem();

    void* Managed = Bind(System.Get());
    ASSERT_NE(Managed, nullptr);

    const FSystemAccess& Access = System->GetAccess();
    EXPECT_TRUE(Algo::Contains(Access.Writes, IdOf<STransformComponent>()));
    EXPECT_TRUE(Algo::Contains(Access.Reads, IdOf<SNameComponent>()));
    EXPECT_FALSE(Algo::Contains(Access.Reads, IdOf<SDisabledTag>())) << "an excluded component declares nothing";
    EXPECT_FALSE(Algo::Contains(Access.Reads, IdOf<SLifetimeComponent>())) << "a With component declares nothing";
    EXPECT_FALSE(Algo::Contains(Access.Writes, IdOf<SLifetimeComponent>()));
    EXPECT_TRUE(Algo::Contains(Access.Reads, IdOf<SCharacterJumpedEvent>()));
    const ECS::FComponentTypeID Ping = ECS::FindEventType(FName("cs:LuminaSharp.TestSystemPing"));
    ASSERT_NE(Ping, ECS::InvalidComponentTypeID);
    EXPECT_TRUE(Algo::Contains(Access.SentEvents, static_cast<uint32>(Ping)));
    EXPECT_FALSE(Access.bExclusive);

    EXPECT_FALSE(System->HasWork(SSystemContext())) << "no match and no unread event, so the default skips it";

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Mover = Registry.Create();
    Registry.Emplace<STransformComponent>(Mover);
    Registry.Emplace<SNameComponent>(Mover);
    Registry.Emplace<SLifetimeComponent>(Mover);
    const ECS::FEntity Disabled = Registry.Create();
    Registry.Emplace<STransformComponent>(Disabled);
    Registry.Emplace<SNameComponent>(Disabled);
    Registry.Emplace<SDisabledTag>(Disabled);
    const ECS::FEntity Unnamed = Registry.Create();
    Registry.Emplace<STransformComponent>(Unnamed);

    EXPECT_TRUE(System->HasWork(SSystemContext())) << "a matching query gives the system work";

    SCharacterJumpedEvent Jump;
    Jump.JumpCount = 3;
    ECS::SendEvent(World->GetEvents(), Jump);
    Jump.JumpCount = 4;
    ECS::SendEvent(World->GetEvents(), Jump);
    World->FlushCommands();

    int32 Results[10] = {};
    ASSERT_EQ(Run(Managed, (uint32)Mover, (uint32)Disabled, Results), 1);
    EXPECT_EQ(Results[0], 1) << "ForEach visits only the entity that has every include and no exclude";
    EXPECT_EQ(Results[1], 1) << "ParallelForEach sees the same matches";
    EXPECT_EQ(Results[2], 7) << "the reader reads the native queue in place";
    EXPECT_EQ(Results[3], 0) << "reading moves the reader's native cursor";
    EXPECT_EQ(Results[4], 1) << "TryGet reaches a component the query names";
    EXPECT_EQ(Results[5], 0) << "Contains honours the exclude";
    EXPECT_EQ(Results[6], 1) << "TryGet refuses write access the query only grants as read";
    EXPECT_EQ(Results[7], 0);
    EXPECT_EQ(Results[8], 1) << "a With field visits only entities carrying the component";
    EXPECT_EQ(Results[9], 0) << "Contains honours With";
    EXPECT_NEAR(Registry.Get<STransformComponent>(Mover).GetLocalLocation().x, 1.0f, 1e-4f) << "a Write term writes the pool in place";
    EXPECT_NEAR(Registry.Get<STransformComponent>(Disabled).GetLocalLocation().x, 0.0f, 1e-4f);

    World->FlushCommands();
    const ECS::FEventQueue* Pings = World->GetEvents().Find(Ping);
    ASSERT_NE(Pings, nullptr);
    EXPECT_EQ(Pings->NumUnread(0), 1u) << "the writer's send lands after the flush";

    Release(Managed);
}
