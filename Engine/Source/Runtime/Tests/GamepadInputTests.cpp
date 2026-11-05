#include <gtest/gtest.h>

#include "Config/InputSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Events/Event.h"
#include "Input/InputActionMap.h"
#include "Input/InputContext.h"

using namespace Lumina;

namespace
{
    void Tick(FInputContext& Context)
    {
        FInputActionMap::Get().UpdateContext(Context, 1.0f / 60.0f);
        Context.EndFrame();
    }

    const SInputActionState& ActionState(FName Name, const FInputContext& Context)
    {
        return FInputActionMap::Get().GetActionState(SInputActionHandle(Name), Context);
    }

    void Author(const TVector<SInputAction>& Actions)
    {
        GetMutableDefault<CInputSettings>()->Actions = Actions;
        GetMutableDefault<CInputSettings>()->MappingContexts = {};
        FInputActionMap::Get().RebuildFromSettings();
    }

    SInputAction MakeAction(const char* Name, Lumina::EInputActionType Type, const TVector<SInputActionBinding>& Bindings)
    {
        SInputAction Action;
        Action.Name = FName(Name);
        Action.Type = Type;
        Action.Bindings = Bindings;
        return Action;
    }

    SInputActionBinding Bind(const SKey& Key)
    {
        SInputActionBinding Binding;
        Binding.Key = Key;
        return Binding;
    }

    SInputActionBinding BindSource(Lumina::EInputAxisSource Source, Lumina::EInputAxisChannel Channel = Lumina::EInputAxisChannel::X)
    {
        SInputActionBinding Binding;
        Binding.Source = Source;
        Binding.Channel = Channel;
        return Binding;
    }

    FGamepadState Pad()
    {
        FGamepadState State;
        State.bConnected = true;
        return State;
    }
}

TEST(InputButtonState, AdvanceAndSettleWalkTheEdges)
{
    using Input::EButtonState;
    EXPECT_EQ(Input::Advance(EButtonState::Up, true), EButtonState::Pressed);
    EXPECT_EQ(Input::Advance(EButtonState::Pressed, true), EButtonState::Held);
    EXPECT_EQ(Input::Advance(EButtonState::Held, false), EButtonState::Released);
    EXPECT_EQ(Input::Advance(EButtonState::Released, false), EButtonState::Up);
    EXPECT_EQ(Input::Advance(EButtonState::Released, true), EButtonState::Pressed);
    EXPECT_EQ(Input::Settle(EButtonState::Pressed), EButtonState::Held);
    EXPECT_EQ(Input::Settle(EButtonState::Released), EButtonState::Up);
}

TEST(InputDeadZone, RadialCutsTheCenterAndRescalesToFullRange)
{
    float X = 0.1f;
    float Y = 0.1f;
    Input::ApplyRadialDeadZone(X, Y, 0.2f);
    EXPECT_FLOAT_EQ(X, 0.0f);
    EXPECT_FLOAT_EQ(Y, 0.0f);

    X = 1.0f;
    Y = 0.0f;
    Input::ApplyRadialDeadZone(X, Y, 0.2f);
    EXPECT_NEAR(X, 1.0f, 1e-5f);

    X = 0.6f;
    Y = 0.0f;
    Input::ApplyRadialDeadZone(X, Y, 0.2f);
    EXPECT_NEAR(X, 0.5f, 1e-5f);

    EXPECT_FLOAT_EQ(Input::ApplyDeadZone(-0.6f, 0.2f), -0.5f);
    EXPECT_FLOAT_EQ(Input::ApplyDeadZone(0.7f, 0.0f), 0.7f);
}

TEST(InputKeyNames, EveryDeviceHasADisplayName)
{
    EXPECT_EQ(SKey(Lumina::EKey::S, true).GetDisplayName(), FString("Ctrl+S"));
    EXPECT_EQ(SKey(Lumina::EMouseKey::ButtonLeft).GetDisplayName(), FString("Left Mouse Button"));
    EXPECT_EQ(SKey(Lumina::EGamepadButton::FaceBottom).GetDisplayName(), FString("Gamepad A"));
    EXPECT_EQ(SKey().GetDisplayName(), FString("None"));

    SInputActionBinding Stick = BindSource(Lumina::EInputAxisSource::GamepadRightY);
    EXPECT_EQ(Stick.GetDisplayName(), FString("Gamepad Right Stick Y"));
}

TEST(InputKeyNames, SettersRebindToOneDevice)
{
    SKey Key(Lumina::EKey::S, true);
    Key.SetGamepadButton(Lumina::EGamepadButton::Start);
    EXPECT_TRUE(Key.IsGamepad());
    EXPECT_FALSE(Key.bCtrl);
    EXPECT_EQ(Key.Key, Lumina::EKey::Num);
    EXPECT_TRUE(Key.IsValid());
}

TEST(InputChords, ChordFiresInGameModeFromHeldModifierKeys)
{
    Author({ MakeAction("Save", Lumina::EInputActionType::Digital, { Bind(SKey(Lumina::EKey::S, true)) }) });
    FInputContext Context;

    FKeyPressedEvent PressS(Lumina::EKey::S, false, false, false, false);
    Context.OnEvent(PressS);
    Tick(Context);
    EXPECT_FALSE(ActionState(FName("Save"), Context).IsDown());

    FKeyPressedEvent PressCtrl(Lumina::EKey::LeftControl, true, false, false, false);
    Context.OnEvent(PressCtrl);
    FInputActionMap::Get().UpdateContext(Context, 1.0f / 60.0f);
    EXPECT_TRUE(ActionState(FName("Save"), Context).IsPressed());
}

TEST(InputGamepad, ButtonBindingDrivesADigitalActionAndQueuesEvents)
{
    Author({ MakeAction("Jump", Lumina::EInputActionType::Digital, { Bind(SKey(Lumina::EGamepadButton::FaceBottom)) }) });
    FInputContext Context;

    FGamepadState State = Pad();
    State.Buttons[(uint32)Lumina::EGamepadButton::FaceBottom] = Input::EButtonState::Pressed;
    Context.SetGamepadState(State);
    FInputActionMap::Get().UpdateContext(Context, 1.0f / 60.0f);

    EXPECT_TRUE(ActionState(FName("Jump"), Context).IsPressed());
    EXPECT_TRUE(Context.IsGamepadButtonPressed(Lumina::EGamepadButton::FaceBottom));
    ASSERT_EQ(Context.GetFrameEvents().Num(), 1);
    EXPECT_EQ(Context.GetFrameEvents()[0].Type, Lumina::EInputEventType::GamepadDown);
    EXPECT_EQ(Context.GetFrameEvents()[0].GamepadButton, Lumina::EGamepadButton::FaceBottom);
    EXPECT_TRUE(Context.GetFrameEvents()[0].ToKey() == SKey(Lumina::EGamepadButton::FaceBottom));
    Context.EndFrame();

    State.Buttons[(uint32)Lumina::EGamepadButton::FaceBottom] = Input::EButtonState::Held;
    Context.SetGamepadState(State);
    FInputActionMap::Get().UpdateContext(Context, 1.0f / 60.0f);
    EXPECT_TRUE(ActionState(FName("Jump"), Context).IsDown());
    EXPECT_FALSE(ActionState(FName("Jump"), Context).IsPressed());
    EXPECT_TRUE(Context.GetFrameEvents().IsEmpty());
}

TEST(InputGamepad, StickSourcesFeedAnAxis2DAction)
{
    Author({ MakeAction("Move", Lumina::EInputActionType::Axis2D, {
        BindSource(Lumina::EInputAxisSource::GamepadLeftX, Lumina::EInputAxisChannel::X),
        BindSource(Lumina::EInputAxisSource::GamepadLeftY, Lumina::EInputAxisChannel::Y) }) });
    FInputContext Context;

    FGamepadState State = Pad();
    State.Axes[(uint32)Lumina::EGamepadAxis::LeftX] = 0.5f;
    State.Axes[(uint32)Lumina::EGamepadAxis::LeftY] = -1.0f;
    Context.SetGamepadState(State);
    FInputActionMap::Get().UpdateContext(Context, 1.0f / 60.0f);

    EXPECT_FLOAT_EQ(ActionState(FName("Move"), Context).X, 0.5f);
    EXPECT_FLOAT_EQ(ActionState(FName("Move"), Context).Y, -1.0f);
    EXPECT_TRUE(ActionState(FName("Move"), Context).IsDown());
}

TEST(InputGamepad, TriggerSourceFeedsAnAxis1DAction)
{
    Author({ MakeAction("Throttle", Lumina::EInputActionType::Axis1D, { BindSource(Lumina::EInputAxisSource::GamepadRightTrigger) }) });
    FInputContext Context;

    FGamepadState State = Pad();
    State.Axes[(uint32)Lumina::EGamepadAxis::RightTrigger] = 0.75f;
    Context.SetGamepadState(State);
    FInputActionMap::Get().UpdateContext(Context, 1.0f / 60.0f);
    EXPECT_FLOAT_EQ(ActionState(FName("Throttle"), Context).X, 0.75f);
}

TEST(InputGamepad, UIModeHidesThePadFromGameplayQueries)
{
    FInputContext Context;
    FGamepadState State = Pad();
    State.Buttons[(uint32)Lumina::EGamepadButton::Start] = Input::EButtonState::Held;
    State.Axes[(uint32)Lumina::EGamepadAxis::LeftX] = 1.0f;
    Context.SetGamepadState(State);

    Context.SetInputMode(Lumina::EInputMode::UI);
    EXPECT_FALSE(Context.IsGamepadButtonDown(Lumina::EGamepadButton::Start));
    EXPECT_FLOAT_EQ(Context.GetGamepadAxis(Lumina::EGamepadAxis::LeftX), 0.0f);
    EXPECT_TRUE(Context.GetGamepadRaw().IsButtonDown(Lumina::EGamepadButton::Start));

    Context.SetInputMode(Lumina::EInputMode::Game);
    EXPECT_TRUE(Context.IsGamepadButtonDown(Lumina::EGamepadButton::Start));
}

TEST(InputGamepad, ResetDropsThePad)
{
    FInputContext Context;
    FGamepadState State = Pad();
    State.Buttons[(uint32)Lumina::EGamepadButton::FaceTop] = Input::EButtonState::Held;
    Context.SetGamepadState(State);

    Context.ResetState();
    EXPECT_FALSE(Context.IsGamepadConnected());
    EXPECT_FALSE(Context.IsGamepadButtonDown(Lumina::EGamepadButton::FaceTop));
    Tick(Context);
}
