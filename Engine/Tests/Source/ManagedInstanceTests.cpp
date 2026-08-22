#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ManagedInstance.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// Covers the native side table without a .NET host, where a leak would actually live.

namespace
{
    int32 GManagedInstanceFreeCount = 0;

    void CountingFreeHandle(void*)
    {
        ++GManagedInstanceFreeCount;
    }

    // Each test starts from a drained table so tests do not observe each other's handles.
    struct FManagedInstanceFixture : public ::testing::Test
    {
        void SetUp() override
        {
            ManagedInstances::ReleaseAll();
            ManagedInstances::SetFreeHandleFn(&CountingFreeHandle);
            GManagedInstanceFreeCount = 0;
        }

        void TearDown() override
        {
            ManagedInstances::ReleaseAll();
            ManagedInstances::SetFreeHandleFn(nullptr);
        }

        static CObject* NewTestObject()
        {
            return NewObject(CScriptableTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
        }

        static void* FakeHandle(uintptr_t Value)
        {
            return reinterpret_cast<void*>(Value);
        }
    };
}

TEST_F(FManagedInstanceFixture, CachesAndReturnsTheSameHandle)
{
    CObject* Object = NewTestObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(ManagedInstances::Find(Object), nullptr) << "a fresh object should have no cached instance";

    ManagedInstances::Set(Object, FakeHandle(0x1000));
    EXPECT_EQ(ManagedInstances::Find(Object), FakeHandle(0x1000));
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 1);
    EXPECT_EQ(GManagedInstanceFreeCount, 0) << "caching a first handle must not free anything";

    Object->ForceDestroyNow();
}

TEST_F(FManagedInstanceFixture, ReplacingAHandleFreesTheOldOneExactlyOnce)
{
    CObject* Object = NewTestObject();
    ASSERT_NE(Object, nullptr);

    ManagedInstances::Set(Object, FakeHandle(0x1000));
    ManagedInstances::Set(Object, FakeHandle(0x2000));   // the wrapper was collected; a new one replaces it

    EXPECT_EQ(ManagedInstances::Find(Object), FakeHandle(0x2000));
    EXPECT_EQ(GManagedInstanceFreeCount, 1) << "the replaced handle must be freed, and only once";
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 1) << "replacing is not a second live instance";

    Object->ForceDestroyNow();
}

// The leak that matters, where an object dying takes its cached wrapper handle with it.
TEST_F(FManagedInstanceFixture, DestroyingTheObjectFreesItsHandle)
{
    CObject* Object = NewTestObject();
    ASSERT_NE(Object, nullptr);
    ManagedInstances::Set(Object, FakeHandle(0x3000));
    ASSERT_EQ(ManagedInstances::GetLiveCount(), 1);

    Object->ForceDestroyNow();

    EXPECT_EQ(GManagedInstanceFreeCount, 1) << "~CObjectBase must release the cached managed instance";
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 0) << "the object's slot was not reclaimed -- leak";
}

// An object that was never wrapped must not touch the table at all (the destructor guard).
TEST_F(FManagedInstanceFixture, UnwrappedObjectsCostNothing)
{
    const int32 CapacityBefore = ManagedInstances::GetSlotCapacity();

    for (int32 i = 0; i < 32; ++i)
    {
        CObject* Object = NewTestObject();
        ASSERT_NE(Object, nullptr);
        Object->ForceDestroyNow();
    }

    EXPECT_EQ(ManagedInstances::GetLiveCount(), 0);
    EXPECT_EQ(GManagedInstanceFreeCount, 0) << "objects that were never wrapped must not free anything";
    EXPECT_EQ(ManagedInstances::GetSlotCapacity(), CapacityBefore) << "unwrapped objects should allocate no slot";
}

// Slots must be recycled, or churning objects grows the table without bound.
TEST_F(FManagedInstanceFixture, SlotsAreRecycledAcrossObjectChurn)
{
    // One live object at a time, wrapped then destroyed, 64 times over.
    for (int32 i = 0; i < 64; ++i)
    {
        CObject* Object = NewTestObject();
        ASSERT_NE(Object, nullptr);
        ManagedInstances::Set(Object, FakeHandle(0x4000 + i));
        Object->ForceDestroyNow();
    }

    EXPECT_EQ(ManagedInstances::GetLiveCount(), 0);
    EXPECT_EQ(GManagedInstanceFreeCount, 64) << "every handle must be freed exactly once";
    EXPECT_LE(ManagedInstances::GetSlotCapacity(), 1) << "slots grew per object instead of being recycled";
}

// The native mirror of a collectible script ALC swap, freeing each handle exactly once.
TEST_F(FManagedInstanceFixture, HotReloadDrainsTheTableAndObjectsStillWork)
{
    constexpr int32 Count = 8;
    CObject* Objects[Count] = {};
    for (int32 i = 0; i < Count; ++i)
    {
        Objects[i] = NewTestObject();
        ASSERT_NE(Objects[i], nullptr);
        ManagedInstances::Set(Objects[i], FakeHandle(0x5000 + i));
    }
    ASSERT_EQ(ManagedInstances::GetLiveCount(), Count);
    const int32 CapacityAfterFirstGeneration = ManagedInstances::GetSlotCapacity();

    // The reload point (DotNetHost.cpp calls this right after the managed generation swaps).
    ManagedInstances::ReleaseAll();

    EXPECT_EQ(GManagedInstanceFreeCount, Count) << "every handle from the old generation must be freed once";
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 0);
    for (int32 i = 0; i < Count; ++i)
    {
        EXPECT_EQ(ManagedInstances::Find(Objects[i]), nullptr)
            << "objects must report no cached instance after a reload, so the next access re-creates it";
    }

    // Re-wrapping against the new generation works and reuses the drained slots rather than growing.
    for (int32 i = 0; i < Count; ++i)
    {
        ManagedInstances::Set(Objects[i], FakeHandle(0x6000 + i));
    }
    EXPECT_EQ(ManagedInstances::GetLiveCount(), Count);
    EXPECT_EQ(ManagedInstances::GetSlotCapacity(), CapacityAfterFirstGeneration)
        << "a reload grew the slot table; repeated reloads would leak slots";

    // And the objects are still destructible without a double free of the drained handles.
    for (int32 i = 0; i < Count; ++i)
    {
        Objects[i]->ForceDestroyNow();
    }
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 0);
    EXPECT_EQ(GManagedInstanceFreeCount, Count * 2);
}

// Destroying an object AFTER a reload drained its slot must not free a stale handle or corrupt the table.
TEST_F(FManagedInstanceFixture, DestroyAfterReleaseAllIsNotADoubleFree)
{
    CObject* Object = NewTestObject();
    ASSERT_NE(Object, nullptr);
    ManagedInstances::Set(Object, FakeHandle(0x7000));

    ManagedInstances::ReleaseAll();
    ASSERT_EQ(GManagedInstanceFreeCount, 1);

    Object->ForceDestroyNow();

    EXPECT_EQ(GManagedInstanceFreeCount, 1) << "the drained handle was freed a second time";
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 0);
}

// With no host installed the table must still drain rather than call a null function pointer.
TEST_F(FManagedInstanceFixture, WorksWithNoFreeFunctionInstalled)
{
    ManagedInstances::SetFreeHandleFn(nullptr);

    CObject* Object = NewTestObject();
    ASSERT_NE(Object, nullptr);
    ManagedInstances::Set(Object, FakeHandle(0x8000));
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 1);

    Object->ForceDestroyNow();
    EXPECT_EQ(ManagedInstances::GetLiveCount(), 0);
}
