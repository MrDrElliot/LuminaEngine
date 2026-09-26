#include <gtest/gtest.h>

#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Memory/Memory.h"

using namespace Lumina;

namespace
{
    // Stands in for a component that a handler destroys, which is what "destroy on hit" does to a rigid body.
    struct FOwner
    {
        TScriptDelegate<int32> Event;
    };

    void ManagedThunk(void* Context, const void* Payload)
    {
        auto* Fn = static_cast<TFunction<void()>*>(Context);
        (void)Payload;
        (*Fn)();
    }
}

// A handler that frees its own delegate used to leave Broadcast writing into the freed object.
TEST(DelegateReentrancy, AManagedHandlerMayDestroyItsOwnDelegate)
{
    FOwner* Owner = Memory::New<FOwner>();
    int32 Calls = 0;

    TFunction<void()> Handler = [&]()
    {
        ++Calls;
        Memory::Delete(Owner);
        Owner = nullptr;
    };

    Owner->Event.BindManaged(&ManagedThunk, &Handler);
    Owner->Event.Broadcast(7);

    EXPECT_EQ(Calls, 1) << "the handler must have run exactly once";
    EXPECT_EQ(Owner, nullptr) << "the delegate's owner was destroyed from inside the broadcast";
}

// The same, from a native listener, which runs before the managed fan-out gets its turn.
TEST(DelegateReentrancy, ANativeHandlerMayDestroyItsOwnDelegate)
{
    FOwner* Owner = Memory::New<FOwner>();
    int32 NativeCalls = 0;
    int32 ManagedCalls = 0;

    TFunction<void()> ManagedHandler = [&]() { ++ManagedCalls; };
    Owner->Event.BindManaged(&ManagedThunk, &ManagedHandler);

    Owner->Event.AddLambda([&](const int32&)
    {
        ++NativeCalls;
        Memory::Delete(Owner);
        Owner = nullptr;
    });

    Owner->Event.Broadcast(3);

    EXPECT_EQ(NativeCalls, 1);
    EXPECT_EQ(ManagedCalls, 0) << "the managed fan-out must not run against a destroyed delegate";
    EXPECT_EQ(Owner, nullptr);
}

// Only the first of several listeners destroys the owner; the rest must simply not run.
TEST(DelegateReentrancy, DestroyingMidFanOutStopsTheRemainingListeners)
{
    FOwner* Owner = Memory::New<FOwner>();
    int32 First = 0;
    int32 Second = 0;

    TFunction<void()> FirstHandler = [&]()
    {
        ++First;
        Memory::Delete(Owner);
        Owner = nullptr;
    };
    TFunction<void()> SecondHandler = [&]() { ++Second; };

    Owner->Event.BindManaged(&ManagedThunk, &FirstHandler);
    Owner->Event.BindManaged(&ManagedThunk, &SecondHandler);
    Owner->Event.Broadcast(1);

    EXPECT_EQ(First, 1);
    EXPECT_EQ(Second, 0) << "a listener after the destroying one must not be invoked";
}

// Unbinding during a broadcast still has to defer, which is the case the lock count already covered.
TEST(DelegateReentrancy, UnbindingDuringBroadcastIsStillDeferred)
{
    FOwner Owner;
    int32 Calls = 0;
    uint64 SecondId = 0;

    TFunction<void()> FirstHandler = [&]()
    {
        ++Calls;
        Owner.Event.UnbindManaged(SecondId);
    };
    TFunction<void()> SecondHandler = [&]() { ++Calls; };

    Owner.Event.BindManaged(&ManagedThunk, &FirstHandler);
    SecondId = Owner.Event.BindManaged(&ManagedThunk, &SecondHandler);

    Owner.Event.Broadcast(0);
    EXPECT_EQ(Calls, 1) << "the unbound listener must be skipped, not invoked";

    Owner.Event.Broadcast(0);
    EXPECT_EQ(Calls, 2) << "only the surviving listener runs on the next broadcast";
}

// Assignment must not leave one half of the listener set replaced and the other half preserved.
TEST(DelegateReentrancy, AssignmentLeavesBothHalvesOfTheListenerSetAlone)
{
    FOwner Destination;
    int32 ManagedCalls = 0;
    int32 NativeCalls = 0;

    TFunction<void()> ManagedHandler = [&]() { ++ManagedCalls; };
    Destination.Event.BindManaged(&ManagedThunk, &ManagedHandler);
    Destination.Event.AddLambda([&](const int32&) { ++NativeCalls; });

    FOwner Source;
    Destination.Event = Source.Event;

    Destination.Event.Broadcast(0);
    EXPECT_EQ(ManagedCalls, 1) << "assignment must not drop the managed listeners";
    EXPECT_EQ(NativeCalls, 1) << "assignment must not drop the native listeners either";
}

// Multi-argument script delegates, and the arg pack layout the marshalling side reads.
namespace
{
    struct FBlittablePayload { float X; float Y; };
}

TEST(ScriptDelegateMultiArg, CarriesSeveralArgumentsToNativeListeners)
{
    TScriptDelegate<int32, float, FBlittablePayload> Event;

    int32              SeenInt = 0;
    float              SeenFloat = 0.0f;
    FBlittablePayload  SeenPayload{};
    int32              Calls = 0;

    Event.AddLambda([&](const int32& A, const float& B, const FBlittablePayload& C)
    {
        SeenInt = A;
        SeenFloat = B;
        SeenPayload = C;
        ++Calls;
    });

    Event.Broadcast(7, 2.5f, FBlittablePayload{ 1.0f, 3.0f });

    EXPECT_EQ(Calls, 1);
    EXPECT_EQ(SeenInt, 7);
    EXPECT_FLOAT_EQ(SeenFloat, 2.5f);
    EXPECT_FLOAT_EQ(SeenPayload.X, 1.0f);
    EXPECT_FLOAT_EQ(SeenPayload.Y, 3.0f);
}

TEST(ScriptDelegateMultiArg, NoArgumentDelegateStillBroadcasts)
{
    FScriptDelegate Event;
    int32 Calls = 0;
    Event.AddLambda([&]() { ++Calls; });
    Event.Broadcast();
    EXPECT_EQ(Calls, 1);
}

// The managed side reads arguments out of this layout, so it has to be plain C struct order.
TEST(ScriptDelegateMultiArg, ArgPackIsDeclarationOrderedCStructLayout)
{
    using FPack = TArgPack<int32, float, FBlittablePayload>;

    EXPECT_EQ(offsetof(FPack, V0), 0u);
    EXPECT_EQ(offsetof(FPack, V1), sizeof(int32));
    EXPECT_EQ(offsetof(FPack, V2), sizeof(int32) + sizeof(float));
    EXPECT_EQ(sizeof(FPack), sizeof(int32) + sizeof(float) + sizeof(FBlittablePayload));

    // A single-argument pack must stay byte-identical to the bare payload, which is what keeps the
    // existing generated C# accessors reading the right bytes.
    EXPECT_EQ(sizeof(TArgPack<FBlittablePayload>), sizeof(FBlittablePayload));
    EXPECT_EQ(offsetof(TArgPack<FBlittablePayload>, V0), 0u);
}
