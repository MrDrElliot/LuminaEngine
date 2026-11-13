#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectCore.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptableTest.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Entity/Systems/ResourceRegistry.h"
#include "World/Entity/Systems/WeatherState.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/World.h"

using namespace Lumina;

namespace ManagedResourceTestsInternal
{
    template<typename T>
    uint32 IdOf()
    {
        return static_cast<uint32>(ECS::GetComponentTypeID<T>());
    }

    void KeepNew(TVector<uint32>& Into, const TVector<uint32>& From, const TVector<uint32>& Baseline)
    {
        for (const uint32 Id : From)
        {
            if (!Algo::Contains(Baseline, Id))
            {
                Into.Add(Id);
            }
        }
    }

    // The host's own Configure declares components, so only what the C# fields added on top of it is compared.
    FSystemAccess DeclaredByFields(const FSystemAccess& Baseline, const FSystemAccess& Full)
    {
        FSystemAccess Fields;
        KeepNew(Fields.Reads, Full.Reads, Baseline.Reads);
        KeepNew(Fields.Writes, Full.Writes, Baseline.Writes);
        KeepNew(Fields.SentEvents, Full.SentEvents, Baseline.SentEvents);
        for (const ECS::FComponentTypeID Pool : Full.Pools)
        {
            if (!Algo::Contains(Baseline.Pools, Pool))
            {
                Fields.Pools.Add(Pool);
            }
        }
        return Fields;
    }

    using FBindFn    = void*(*)(void*, int32);
    using FRunFn     = int32(*)(void*, float*);
    using FReleaseFn = void(*)(void*);

    enum EKind : int32
    {
        Reader        = 0,
        ScoreWriter   = 1,
        WeatherWriter = 2,
    };
}

using namespace ManagedResourceTestsInternal;

class FManagedResourceTest : public ::testing::Test
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
        Bind    = (FBindFn)DotNet::ResolveManagedExport("Test_ResourcesBind");
        Run     = (FRunFn)DotNet::ResolveManagedExport("Test_ResourcesRun");
        Release = (FReleaseFn)DotNet::ResolveManagedExport("Test_ResourcesRelease");
        ASSERT_NE(Bind, nullptr);
        ASSERT_NE(Run, nullptr);
        ASSERT_NE(Release, nullptr);
        RegisterPendingTypes();
    }

    void TearDown() override
    {
        for (void* Managed : Bound)
        {
            Release(Managed);
        }
    }

    FSystemAccess MakeSystem(CWorld* World, EKind Kind)
    {
        CEntitySystemTest* System = Systems.AddGetRef(TStrongObjectPtr<CEntitySystemTest>(NewObject<CEntitySystemTest>(nullptr, NAME_None, FGuid::New(), OF_Transient))).Get();
        System->SetOwningWorld(World);
        System->ConfigureSystem();
        const FSystemAccess Baseline = System->GetAccess();
        void* Managed = Bind(System, Kind);
        EXPECT_NE(Managed, nullptr);
        Bound.Add(Managed);
        return DeclaredByFields(Baseline, System->GetAccess());
    }

    CWorld* MakeWorld(const char* Name)
    {
        return Worlds.AddGetRef(TStrongObjectPtr<CWorld>(NewObject<CWorld>(nullptr, FName(Name), FGuid::New(), OF_Transient))).Get();
    }

    float RunAt(int32 Index, float* Out)
    {
        EXPECT_EQ(Run(Bound[Index], Out), 1);
        return Out[0];
    }

    FBindFn    Bind    = nullptr;
    FRunFn     Run     = nullptr;
    FReleaseFn Release = nullptr;

    TVector<TStrongObjectPtr<CWorld>>            Worlds;
    TVector<TStrongObjectPtr<CEntitySystemTest>> Systems;
    TVector<void*>                               Bound;
};

TEST_F(FManagedResourceTest, ReflectedResourcesRegisterThemselves)
{
    const ECS::FResourceTypeRegistry& Resources = ECS::FResourceTypeRegistry::Get();
    const ECS::FResourceType* Settings = Resources.Find(FName("SWorldSettingsState"));
    ASSERT_NE(Settings, nullptr);
    EXPECT_EQ(Settings->TypeId, IdOf<SWorldSettingsState>());
    EXPECT_EQ(Resources.Find(SWorldSettingsState::StaticStruct()), Settings);
    EXPECT_NE(Resources.Find(FName("SCameraGlobalState")), nullptr);
    EXPECT_NE(Resources.Find(FName("SWeatherState")), nullptr);
    EXPECT_EQ(Resources.Find(FName("STransformComponent")), nullptr) << "a component is no resource";
}

TEST_F(FManagedResourceTest, NamedAccessTokensAreStableAndApart)
{
    ECS::FResourceTypeRegistry& Resources = ECS::FResourceTypeRegistry::Get();
    const uint32 First = Resources.GetNamedAccessToken(FName("res:ManagedResourceTests.First"));
    const uint32 Second = Resources.GetNamedAccessToken(FName("res:ManagedResourceTests.Second"));
    EXPECT_GE(First, ECS::FirstNamedAccessToken);
    EXPECT_GE(Second, ECS::FirstNamedAccessToken);
    EXPECT_NE(First, Second);
    EXPECT_EQ(Resources.GetNamedAccessToken(FName("res:ManagedResourceTests.First")), First);
    EXPECT_EQ(Resources.FindNamedAccessToken(FName("res:ManagedResourceTests.Second")), Second);
    EXPECT_EQ(Resources.FindNamedAccessToken(FName("res:ManagedResourceTests.Never")), 0u);
}

TEST_F(FManagedResourceTest, CSharpResourceFieldsDeclareAndShareLiveValues)
{
    CWorld* World = MakeWorld("ManagedResourceWorld");
    CWorld* Other = MakeWorld("ManagedResourceOtherWorld");

    const FSystemAccess Read    = MakeSystem(World, Reader);
    const FSystemAccess ReaderB = MakeSystem(World, Reader);
    const FSystemAccess WriterA = MakeSystem(World, ScoreWriter);
    const FSystemAccess WriterB = MakeSystem(World, ScoreWriter);
    const FSystemAccess Weather = MakeSystem(World, WeatherWriter);
    MakeSystem(Other, Reader);
    ASSERT_FALSE(HasFailure());

    const uint32 Score = ECS::FResourceTypeRegistry::Get().FindNamedAccessToken(FName("res:LuminaSharp.TestScore"));
    ASSERT_NE(Score, 0u) << "binding a managed resource field acquires its token";

    EXPECT_TRUE(Algo::Contains(Read.Reads, IdOf<SWorldSettingsState>())) << "a TRes of a native resource declares a read of its type";
    EXPECT_TRUE(Algo::Contains(Read.Reads, Score)) << "a TRes of a managed resource declares a read of its token";
    EXPECT_FALSE(Algo::Contains(Read.Writes, IdOf<SWorldSettingsState>()));
    EXPECT_FALSE(Algo::Contains(Read.Writes, Score));
    EXPECT_TRUE(Read.Pools.IsEmpty()) << "a resource owns no pool";
    EXPECT_TRUE(Read.SentEvents.IsEmpty()) << "a resource is not mistaken for an event";

    EXPECT_TRUE(Algo::Contains(WriterA.Writes, Score)) << "a TResMut of a managed resource declares a write";
    EXPECT_TRUE(Algo::Contains(Weather.Writes, IdOf<SWeatherState>())) << "a TResMut of a native resource declares a write";
    EXPECT_TRUE(Weather.SentEvents.IsEmpty());

    EXPECT_FALSE(FSystemAccess::Conflicts(Read, ReaderB)) << "two readers run together";
    EXPECT_TRUE(FSystemAccess::Conflicts(WriterA, WriterB)) << "two writers of one managed resource conflict";
    EXPECT_TRUE(FSystemAccess::Conflicts(Read, WriterA)) << "a reader and a writer conflict";
    EXPECT_FALSE(FSystemAccess::Conflicts(Read, Weather)) << "different resources do not conflict";

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    ASSERT_NE(Registry.Ctx().Find<SWeatherState>(), nullptr) << "binding emplaces the native resource on the main thread";
    Registry.Ctx().Get<SWorldSettingsState>().Gravity = FVector3(0.0f, -3.5f, 0.0f);

    float Out[2] = {};
    RunAt(0, Out);
    EXPECT_FLOAT_EQ(Out[0], -3.5f) << "C# reads the value C++ wrote, in place";
    EXPECT_FLOAT_EQ(Out[1], 0.0f);

    EXPECT_FLOAT_EQ(RunAt(2, Out), 5.0f);
    EXPECT_FLOAT_EQ(RunAt(3, Out), 10.0f) << "both writers reach the world's one instance";

    RunAt(1, Out);
    EXPECT_FLOAT_EQ(Out[1], 10.0f) << "a reader sees what the writers wrote";

    RunAt(5, Out);
    EXPECT_FLOAT_EQ(Out[1], 0.0f) << "another world holds its own managed resource";

    RunAt(4, Out);
    EXPECT_FLOAT_EQ(Registry.Ctx().Get<SWeatherState>().Wetness, 0.75f) << "a C# write lands in the native resource";
}
