#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

#include "Scripting/ScriptableTest.h"
#include "World/Subsystems/WorldSubsystem.h"

using namespace Lumina;

namespace
{
    // The driver only ever stores and hands back the pointer, so a sentinel proves ownership without a world.
    CWorld* const SentinelWorld = reinterpret_cast<CWorld*>(0x5150);

    // Flips the gate for one test and puts it back, so discovery in any other test still skips the type.
    struct FCreationGate
    {
        FCreationGate()
        {
            // The harness only starts the object system, so the deferred class pass has to run here.
            ProcessNewlyLoadedCObjects();
            CWorldSubsystemTest::bAllowCreation = true;
        }

        ~FCreationGate() { CWorldSubsystemTest::bAllowCreation = false; }
    };

    CWorldSubsystemTest* FindTestSubsystem(const TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems)
    {
        return Cast<CWorldSubsystemTest>(
            WorldSubsystems::Find(Subsystems, CWorldSubsystemTest::StaticClass()));
    }
}

TEST(WorldSubsystem, CreateMissingHonorsShouldCreate)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(*SentinelWorld, Subsystems);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr) << "a subsystem class that wants to exist was not created";

    EXPECT_EQ(Created->GetWorld(), SentinelWorld) << "the owning world is set before OnInitialize runs";

    EXPECT_EQ(WorldSubsystems::Find(Subsystems, CWorldSubsystemDeclineTest::StaticClass()), nullptr)
        << "a class whose ShouldCreate returns false must be skipped";

    WorldSubsystems::DestroyAll(Subsystems);
}

TEST(WorldSubsystem, InitializeRunsBeforeWorldReady)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(*SentinelWorld, Subsystems);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr);

    EXPECT_EQ(Created->InitializeCount, 1);
    EXPECT_EQ(Created->ReadyCount, 1);
    EXPECT_TRUE(Created->bReadySawInitialize) << "OnWorldReady must run after every OnInitialize";

    WorldSubsystems::DestroyAll(Subsystems);
}

TEST(WorldSubsystem, CreateMissingIsIdempotent)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    const int32 First = WorldSubsystems::CreateMissing(*SentinelWorld, Subsystems);
    ASSERT_GT(First, 0);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr);

    const size_t CountAfterFirst = Subsystems.size();

    EXPECT_EQ(WorldSubsystems::CreateMissing(*SentinelWorld, Subsystems), 0)
        << "a second pass must not duplicate a subsystem that already exists";
    EXPECT_EQ(Subsystems.size(), CountAfterFirst);
    EXPECT_EQ(Created->InitializeCount, 1) << "an existing subsystem must not be initialized twice";

    WorldSubsystems::DestroyAll(Subsystems);
}

TEST(WorldSubsystem, UpdateAndTeardownReachEverySubsystem)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(*SentinelWorld, Subsystems);

    CWorldSubsystemTest* Created = FindTestSubsystem(Subsystems);
    ASSERT_NE(Created, nullptr);

    WorldSubsystems::Update(Subsystems, 0.25f);
    WorldSubsystems::Update(Subsystems, 0.25f);

    EXPECT_EQ(Created->UpdateCount, 2);
    EXPECT_FLOAT_EQ(Created->AccumulatedTime, 0.5f);

    WorldSubsystems::DestroyAll(Subsystems);

    EXPECT_EQ(Created->TeardownCount, 1);
    EXPECT_EQ(Created->GetWorld(), nullptr) << "teardown must unlink the world it no longer belongs to";
    EXPECT_TRUE(Subsystems.empty());
}

TEST(WorldSubsystem, DropScriptedLeavesNativeSubsystemsAlone)
{
    FCreationGate Gate;

    TVector<TStrongObjectPtr<CWorldSubsystem>> Subsystems;
    WorldSubsystems::CreateMissing(*SentinelWorld, Subsystems);

    const size_t Before = Subsystems.size();
    ASSERT_GT(Before, 0u);

    // Nothing here is backed by a script class, so a reload rebuild must not disturb the native ones.
    WorldSubsystems::DropScripted(Subsystems);

    EXPECT_EQ(Subsystems.size(), Before);
    EXPECT_NE(FindTestSubsystem(Subsystems), nullptr);

    WorldSubsystems::DestroyAll(Subsystems);
}
