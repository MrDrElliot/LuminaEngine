#include <gtest/gtest.h>

#include "Core/Windows/WindowInput.h"
#include "Events/Event.h"
#include "Input/InputContext.h"
#include "Renderer/ViewVolume.h"
#include "World/Entity/Components/CameraComponent.h"

using namespace Lumina;

namespace
{
    constexpr float DegreesPerCount = 0.1f;

    const FVector3 LookForward(0.0f, 0.0f, 1.0f);
    const FVector3 LookUp(0.0f, 1.0f, 0.0f);

    // The window feeding one viewport's context, with the same hold the engine opens between update and extract.
    struct FLatchedInput
    {
        FWindowEventGate Gate;
        FInputContext    Context;
        TVector<uint32>  Chars;

        void Move(float DeltaX, float DeltaY)
        {
            Gate.Dispatch([this, DeltaX, DeltaY]
            {
                FMouseMovedEvent Event(0.0f, 0.0f, DeltaX, DeltaY);
                Context.OnEvent(Event);
            }, FVector2(DeltaX, DeltaY));
        }

        FVector2 UpdateDelta() const
        {
            return FVector2((float)Context.GetMouseDeltaXRaw(), (float)Context.GetMouseDeltaYRaw());
        }
    };

    void ExpectSameDirection(const FVector3& A, const FVector3& B)
    {
        EXPECT_NEAR(A.x, B.x, 1e-4f);
        EXPECT_NEAR(A.y, B.y, 1e-4f);
        EXPECT_NEAR(A.z, B.z, 1e-4f);
    }
}

// Every count reaches the update once, and each frame shows the update's rotation plus only that frame's late motion.
TEST(MouseLookLateLatch, LateMotionRendersEarlyAndReachesTheNextUpdateOnce)
{
    struct FFrameInput
    {
        FVector2 Early;
        FVector2 Late;
    };
    const FFrameInput Frames[] =
    {
        { FVector2(3.0f, 1.0f),   FVector2(5.0f, -2.0f) },
        { FVector2(0.0f, 0.0f),   FVector2(-7.0f, 4.0f) },
        { FVector2(12.0f, -3.0f), FVector2(0.0f, 0.0f) },
        { FVector2(-4.0f, 6.0f),  FVector2(9.0f, 1.0f) },
        { FVector2(0.0f, 0.0f),   FVector2(0.0f, 0.0f) },
    };

    FLatchedInput Input;
    const FQuat Start = Math::QuatLookAt(LookForward, LookUp);
    FQuat    UpdateRotation = Start;
    FVector2 TotalInput(0.0f);
    FVector2 TotalUpdate(0.0f);

    for (const FFrameInput& Frame : Frames)
    {
        Input.Gate.Release();
        Input.Move(Frame.Early.x, Frame.Early.y);
        TotalInput += Frame.Early;

        const FVector2 UpdateDelta = Input.UpdateDelta();
        TotalUpdate += UpdateDelta;
        UpdateRotation = AddMouseLook(UpdateRotation, UpdateDelta * DegreesPerCount);

        Input.Gate.BeginHold();
        Input.Move(Frame.Late.x, Frame.Late.y);
        Input.Gate.EndHold();
        TotalInput += Frame.Late;

        // The update already ran, so the context it read never sees the late motion this frame.
        EXPECT_EQ(Input.UpdateDelta(), UpdateDelta);
        EXPECT_EQ(Input.Gate.GetHeldMouseDelta(), Frame.Late);

        FViewVolume UpdateView;
        UpdateView.SetView(FVector3(1.0f, 2.0f, 3.0f), UpdateRotation * LookForward, UpdateRotation * LookUp);
        const FViewVolume Rendered = AddMouseLook(UpdateView, Input.Gate.GetHeldMouseDelta() * DegreesPerCount);

        const FQuat Expected = AddMouseLook(UpdateRotation, Frame.Late * DegreesPerCount);
        ExpectSameDirection(Rendered.GetForwardVector(), Expected * LookForward);
        ExpectSameDirection(Rendered.GetUpVector(), Expected * LookUp);

        // Nothing builds up, since the rendered view is exactly where all the motion so far points.
        ExpectSameDirection(Rendered.GetForwardVector(), AddMouseLook(Start, TotalInput * DegreesPerCount) * LookForward);

        Input.Context.EndFrame();
    }

    Input.Gate.Release();
    TotalUpdate += Input.UpdateDelta();
    EXPECT_EQ(TotalUpdate, TotalInput);
}

TEST(MouseLookLateLatch, KeysButtonsAndCharsFromALatePumpWaitForTheNextFrame)
{
    FLatchedInput Input;

    Input.Gate.BeginHold();
    Input.Gate.Dispatch([&Input]
    {
        FKeyPressedEvent Event(EKey::W, false, false, false, false);
        Input.Context.OnEvent(Event);
    });
    Input.Gate.Dispatch([&Input]
    {
        FMouseButtonPressedEvent Event(EMouseKey::ButtonLeft, 0.0f, 0.0f);
        Input.Context.OnEvent(Event);
    });
    Input.Gate.Dispatch([&Input] { Input.Chars.Add('w'); });
    Input.Gate.EndHold();

    EXPECT_FALSE(Input.Context.IsKeyDownRaw(EKey::W));
    EXPECT_FALSE(Input.Context.IsMouseButtonDownRaw(EMouseKey::ButtonLeft));
    EXPECT_TRUE(Input.Chars.IsEmpty());
    EXPECT_TRUE(Input.Context.GetFrameEvents().IsEmpty());

    Input.Context.EndFrame();
    Input.Gate.Release();

    EXPECT_TRUE(Input.Context.IsKeyDownRaw(EKey::W));
    EXPECT_TRUE(Input.Context.IsMouseButtonDownRaw(EMouseKey::ButtonLeft));
    ASSERT_EQ(Input.Chars.Num(), 1);
    ASSERT_EQ(Input.Context.GetFrameEvents().Num(), 2);
    EXPECT_EQ(Input.Context.GetFrameEvents()[0].Type, EInputEventType::KeyDown);
    EXPECT_EQ(Input.Context.GetFrameEvents()[1].Type, EInputEventType::MouseDown);

    // Released once, so the next pump replays nothing again.
    Input.Context.EndFrame();
    Input.Gate.Release();
    EXPECT_TRUE(Input.Context.GetFrameEvents().IsEmpty());
    EXPECT_EQ(Input.Chars.Num(), 1);
}

// The latch shares the look's pitch clamp, so a late flick past vertical cannot show a view the update would refuse.
TEST(MouseLookLateLatch, PitchStopsShortOfVertical)
{
    const FQuat Level = Math::QuatLookAt(LookForward, LookUp);

    const float Down = Math::Degrees(Math::Asin((AddMouseLook(Level, FVector2(0.0f, 500.0f)) * LookForward).y));
    const float Raised = Math::Degrees(Math::Asin((AddMouseLook(Level, FVector2(30.0f, -500.0f)) * LookForward).y));

    EXPECT_NEAR(Down, -89.0f, 1e-2f);
    EXPECT_NEAR(Raised, 89.0f, 1e-2f);
}
