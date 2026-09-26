#include <gtest/gtest.h>

#include "Core/Delegates/ScriptDelegate.h"
#include "Memory/Memory.h"

using namespace Lumina;

// A managed binding hands its GCHandle to the delegate, so these pin down who releases it and when.
namespace
{
    int32 GFreedContexts = 0;
    void* GLastFreed = nullptr;

    void CountingFree(void* Context)
    {
        ++GFreedContexts;
        GLastFreed = Context;
    }

    void NoopThunk(void*, const void*) {}

    struct FOwnershipFixture : ::testing::Test
    {
        void SetUp() override
        {
            Previous = GFreeManagedDelegateContext;
            GFreeManagedDelegateContext = &CountingFree;
            GFreedContexts = 0;
            GLastFreed = nullptr;
        }

        void TearDown() override { GFreeManagedDelegateContext = Previous; }

        void (*Previous)(void*) = nullptr;
    };
}

TEST_F(FOwnershipFixture, UnbindingReleasesTheManagedContext)
{
    TScriptDelegate<int32> Event;
    int32 Context = 0;

    const uint64 Id = Event.BindManaged(&NoopThunk, &Context);
    ASSERT_NE(Id, 0u);
    EXPECT_EQ(GFreedContexts, 0);

    EXPECT_TRUE(Event.UnbindManaged(Id));
    EXPECT_EQ(GFreedContexts, 1);
    EXPECT_EQ(GLastFreed, &Context);
}

// The case the managed registry used to exist for: the delegate dies while a binding is still live.
TEST_F(FOwnershipFixture, DestroyingTheDelegateReleasesEveryManagedContext)
{
    auto* Event = Memory::New<TScriptDelegate<int32>>();
    int32 First = 0;
    int32 Second = 0;

    Event->BindManaged(&NoopThunk, &First);
    Event->BindManaged(&NoopThunk, &Second);
    EXPECT_EQ(GFreedContexts, 0);

    Memory::Delete(Event);
    EXPECT_EQ(GFreedContexts, 2);
}

// A native listener owns its own callable, so retiring one must not run the managed release.
TEST_F(FOwnershipFixture, ANativeListenerIsNotReleasedAsManaged)
{
    TScriptDelegate<int32> Event;
    int32 Context = 0;

    Event.BindManaged(&NoopThunk, &Context);
    const FDelegateHandle Native = Event.AddLambda([](const int32&) {});

    EXPECT_EQ(Event.GetBindingCount(), 2u);
    EXPECT_EQ(Event.GetManagedBindingCount(), 1u);

    EXPECT_TRUE(Event.Remove(Native));
    EXPECT_EQ(GFreedContexts, 0) << "retiring a native listener released a managed context";
    EXPECT_EQ(Event.GetManagedBindingCount(), 1u);
}

// The unload diagnostic replaces the registry, so the count has to be exact in both directions.
TEST_F(FOwnershipFixture, TheLiveManagedCountTracksBindAndUnbind)
{
    const size_t Before = GetLiveManagedBindingCount();

    TScriptDelegate<int32> Event;
    int32 Context = 0;

    const uint64 Id = Event.BindManaged(&NoopThunk, &Context);
    EXPECT_EQ(GetLiveManagedBindingCount(), Before + 1);

    Event.UnbindManaged(Id);
    EXPECT_EQ(GetLiveManagedBindingCount(), Before);

    // A binding left alive is the leak the unload report names, so destruction has to settle it too.
    Event.BindManaged(&NoopThunk, &Context);
    EXPECT_EQ(GetLiveManagedBindingCount(), Before + 1);
}

TEST_F(FOwnershipFixture, TheLiveCountSettlesWhenAnAbandonedDelegateDies)
{
    const size_t Before = GetLiveManagedBindingCount();
    {
        TScriptDelegate<int32> Event;
        int32 Context = 0;
        Event.BindManaged(&NoopThunk, &Context);
        EXPECT_EQ(GetLiveManagedBindingCount(), Before + 1);
    }
    EXPECT_EQ(GetLiveManagedBindingCount(), Before);
}

// A reload drops the load context a handler lives in, so nothing may survive it still holding a GCHandle.
TEST_F(FOwnershipFixture, ClearingForReloadReleasesEveryManagedContextEverywhere)
{
    TScriptDelegate<int32> First;
    TScriptDelegate<int32> Second;
    int32 ContextA = 0;
    int32 ContextB = 0;
    int32 ContextC = 0;

    First.BindManaged(&NoopThunk, &ContextA);
    First.BindManaged(&NoopThunk, &ContextB);
    Second.BindManaged(&NoopThunk, &ContextC);

    const FDelegateHandle Native = Second.AddLambda([](const int32&) {});

    ClearAllManagedDelegateBindings();

    EXPECT_EQ(GFreedContexts, 3);
    EXPECT_EQ(GetLiveManagedBindingCount(), 0u);
    EXPECT_EQ(First.GetBindingCount(), 0u);

    // The native listener is the script's own C++ side and has nothing to do with the load context.
    EXPECT_EQ(Second.GetBindingCount(), 1u);
    EXPECT_TRUE(Second.Remove(Native));
}

// The set behind the sweep must forget a delegate the moment its last managed listener goes.
TEST_F(FOwnershipFixture, AnUnboundDelegateIsNotVisitedByTheReloadSweep)
{
    int32 Context = 0;
    {
        TScriptDelegate<int32> Event;
        const uint64 Id = Event.BindManaged(&NoopThunk, &Context);
        Event.UnbindManaged(Id);
        EXPECT_EQ(GFreedContexts, 1);
    }

    // A stale entry here would be a sweep over freed memory rather than a no-op.
    ClearAllManagedDelegateBindings();
    EXPECT_EQ(GFreedContexts, 1);
}
