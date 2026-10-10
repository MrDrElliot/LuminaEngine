#include <gtest/gtest.h>

#include "Core/Delegates/Delegate.h"

using namespace Lumina;

// A plugin's handle once matched another module's on a shared delegate, so removing one dropped the other's entry.
TEST(DelegateHandle, EveryDelegateTypeDrawsFromOneCounter)
{
    TMulticastDelegate<void> First;
    TMulticastDelegate<void, int32> Second;

    const FDelegateHandle A = First.AddLambda([]() {});
    const FDelegateHandle B = Second.AddLambda([](int32) {});
    const FDelegateHandle C = First.AddLambda([]() {});

    EXPECT_NE(A, B);
    EXPECT_NE(B, C);
    EXPECT_NE(A, C);
}

TEST(DelegateHandle, RemovingOneHandleLeavesTheOthersBound)
{
    TMulticastDelegate<void> Delegate;
    int32 Calls = 0;

    const FDelegateHandle Kept    = Delegate.AddLambda([&Calls]() { ++Calls; });
    const FDelegateHandle Removed = Delegate.AddLambda([&Calls]() { Calls += 10; });

    EXPECT_TRUE(Delegate.Remove(Removed));
    Delegate.Broadcast();
    EXPECT_EQ(Calls, 1);

    EXPECT_TRUE(Delegate.Remove(Kept));
    EXPECT_FALSE(Delegate.Remove(Kept));
}
