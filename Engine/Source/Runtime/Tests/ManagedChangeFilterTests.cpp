#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "Core/Object/ObjectCore.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptableTest.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/World.h"

using namespace Lumina;

namespace ManagedChangeFilterTestsInternal
{
    template<typename T>
    uint32 IdOf()
    {
        return static_cast<uint32>(ECS::GetComponentTypeID<T>());
    }

    enum EFilteredQuery : int32
    {
        Renamed          = 0,
        Spawned          = 1,
        PlacedAndRenamed = 2,
    };

    using FBindFn    = void*(*)(void*);
    using FRunFn     = int32(*)(void*, int32, int32);
    using FTickFn    = uint32(*)(void*, int32);
    using FEmptyFn   = int32(*)(void*, int32);
    using FProbeFn   = int32(*)(void*, uint32);
    using FReleaseFn = void(*)(void*);
}

using namespace ManagedChangeFilterTestsInternal;

class FManagedChangeFilterTest : public ::testing::Test
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

TEST_F(FManagedChangeFilterTest, CSharpChangedAndAddedFiltersMatchTQuery)
{
    auto* Bind     = (FBindFn)DotNet::ResolveManagedExport("Test_ChangeFilterBind");
    auto* Run      = (FRunFn)DotNet::ResolveManagedExport("Test_ChangeFilterRun");
    auto* LastTick = (FTickFn)DotNet::ResolveManagedExport("Test_ChangeFilterLastRunTick");
    auto* IsEmpty  = (FEmptyFn)DotNet::ResolveManagedExport("Test_ChangeFilterIsEmpty");
    auto* ReadAll  = (FProbeFn)DotNet::ResolveManagedExport("Test_ChangeFilterReadAll");
    auto* WriteOne = (FProbeFn)DotNet::ResolveManagedExport("Test_ChangeFilterWriteOne");
    auto* Release  = (FReleaseFn)DotNet::ResolveManagedExport("Test_ChangeFilterRelease");
    ASSERT_TRUE(Bind && Run && LastTick && IsEmpty && ReadAll && WriteOne && Release);

    RegisterPendingTypes();
    TStrongObjectPtr<CWorld> World(NewObject<CWorld>(nullptr, FName("ManagedChangeFilterWorld"), FGuid::New(), OF_Transient));
    TStrongObjectPtr<CEntitySystemReaderTest> System(NewObject<CEntitySystemReaderTest>(nullptr, NAME_None, FGuid::New(), OF_Transient));
    System->SetOwningWorld(World.Get());
    System->ConfigureSystem();

    void* Managed = Bind(System.Get());
    ASSERT_NE(Managed, nullptr);

    const FSystemAccess& Access = System->GetAccess();
    EXPECT_TRUE(Algo::Contains(Access.TrackedPools, ECS::GetComponentTypeID<SNameComponent>())) << "a Changed filter declares its pool tracked";
    EXPECT_TRUE(Algo::Contains(Access.TrackedPools, ECS::GetComponentTypeID<STransformComponent>())) << "an Added filter declares its pool tracked";
    EXPECT_EQ(Access.TrackedPools.Num(), 2);
    EXPECT_TRUE(Algo::Contains(Access.Reads, IdOf<SNameComponent>()));
    EXPECT_TRUE(Algo::Contains(Access.Reads, IdOf<STransformComponent>()));
    EXPECT_TRUE(Algo::Contains(Access.Writes, IdOf<SNameComponent>()));
    EXPECT_FALSE(Algo::Contains(Access.Writes, IdOf<STransformComponent>())) << "every term naming the transform reads it";

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity A = Registry.Create();
    Registry.Emplace<SNameComponent>(A);
    Registry.Emplace<STransformComponent>(A);
    const ECS::FEntity B = Registry.Create();
    Registry.Emplace<SNameComponent>(B);
    Registry.Emplace<STransformComponent>(B);

    EXPECT_EQ(Run(Managed, Renamed, 0), 2) << "the first run reports every row";
    const uint32 FirstTick = LastTick(Managed, Renamed);
    EXPECT_NE(FirstTick, 0u);
    EXPECT_EQ(Run(Managed, Renamed, 0), 0) << "nothing was written since the last run";
    const uint32 SecondTick = LastTick(Managed, Renamed);
    EXPECT_TRUE(ECS::FSparseSet::IsNewerTick(SecondTick, FirstTick)) << "each full run moves the field's tick";
    EXPECT_EQ(IsEmpty(Managed, Renamed), 0) << "IsEmpty ignores the filters, as TQuery's does";

    EXPECT_EQ(ReadAll(Managed, (uint32)A), 4) << "ForEach and ParallelForEach visit both rows, and both TryGets reach the read";
    EXPECT_EQ(Run(Managed, Renamed, 0), 0) << "Read terms and read TryGets never mark a row changed";

    Registry.Patch<SNameComponent>(B, [](SNameComponent&) {});
    EXPECT_EQ(Run(Managed, Renamed, 1), 1) << "a filtered ParallelForEach sees only the row written since";
    EXPECT_EQ(Run(Managed, Renamed, 1), 0) << "and completes its run";

    EXPECT_EQ(WriteOne(Managed, (uint32)A), 1);
    EXPECT_EQ(Run(Managed, Renamed, 0), 1) << "a Write TryGet still marks the row";

    EXPECT_EQ(Run(Managed, Spawned, 0), 2) << "the first run reports every row as added";
    EXPECT_EQ(Run(Managed, Spawned, 0), 0);
    Registry.Patch<STransformComponent>(A, [](STransformComponent&) {});
    EXPECT_EQ(Run(Managed, Spawned, 0), 0) << "a write is not an add";
    const ECS::FEntity C = Registry.Create();
    Registry.Emplace<STransformComponent>(C);
    EXPECT_EQ(Run(Managed, Spawned, 1), 1) << "only the row added since the last run";
    EXPECT_EQ(Run(Managed, Spawned, 1), 0);

    EXPECT_EQ(Run(Managed, PlacedAndRenamed, 0), 2) << "a filter on a component the query does not name still requires it";
    Registry.Patch<SNameComponent>(A, [](SNameComponent&) {});
    EXPECT_EQ(Run(Managed, PlacedAndRenamed, 0), 1);
    EXPECT_EQ(Run(Managed, PlacedAndRenamed, 0), 0);

    Release(Managed);
}
