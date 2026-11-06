#include <gtest/gtest.h>

#include "Config/InputSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Events/Event.h"
#include "Input/InputActionMap.h"
#include "Input/InputContext.h"

using namespace Lumina;

namespace
{
    // Exact in binary, so timers land on their boundaries without float drift.
    constexpr float kStep = 0.125f;

    void Tick(FInputContext& Context, float DeltaSeconds = kStep)
    {
        FInputActionMap::Get().UpdateContext(Context, DeltaSeconds);
        Context.EndFrame();
    }

    void PressKey(FInputContext& Context, Lumina::EKey Key)
    {
        FKeyPressedEvent Event(Key, false, false, false, false);
        Context.OnEvent(Event);
    }

    void ReleaseKey(FInputContext& Context, Lumina::EKey Key)
    {
        FKeyReleasedEvent Event(Key, false, false, false, false);
        Context.OnEvent(Event);
    }

    void MoveMouse(FInputContext& Context, float DeltaX, float DeltaY)
    {
        FMouseMovedEvent Event(0.0f, 0.0f, DeltaX, DeltaY);
        Context.OnEvent(Event);
    }

    const SInputActionState& ActionState(const char* Name, const FInputContext& Context)
    {
        return FInputActionMap::Get().GetActionState(SInputActionHandle(FName(Name)), Context);
    }

    void Author(const TVector<SInputAction>& Actions)
    {
        GetMutableDefault<CInputSettings>()->Actions = Actions;
        GetMutableDefault<CInputSettings>()->MappingContexts = {};
        FInputActionMap::Get().RebuildFromSettings();
    }

    SInputActionBinding KeyBinding(const SKey& Key)
    {
        SInputActionBinding Binding;
        Binding.Key = Key;
        return Binding;
    }

    SInputActionBinding SourceBinding(Lumina::EInputAxisSource Source)
    {
        SInputActionBinding Binding;
        Binding.Source = Source;
        return Binding;
    }

    SInputAction MakeAction(const char* Name, Lumina::EInputActionType Type, const TVector<SInputActionBinding>& Bindings)
    {
        SInputAction Action;
        Action.Name = FName(Name);
        Action.Type = Type;
        Action.Bindings = Bindings;
        return Action;
    }

    SInputAction MakeDigital(const char* Name, Lumina::EKey Key)
    {
        return MakeAction(Name, Lumina::EInputActionType::Digital, { KeyBinding(SKey(Key)) });
    }

    void Tap(FInputContext& Context, Lumina::EKey Key)
    {
        PressKey(Context, Key);
        Tick(Context);
        ReleaseKey(Context, Key);
        Tick(Context);
    }

    FGamepadState PadWithAxis(Lumina::EGamepadAxis Axis, float Value)
    {
        FGamepadState State;
        State.bConnected = true;
        State.Axes[(uint32)Axis] = Value;
        return State;
    }

    // The map is a singleton, so every test starts and ends with the player's defaults.
    class InputTriggerTests : public ::testing::Test
    {
    protected:

        void SetUp() override    { FInputActionMap::Get().SetPlayerSettings(SPlayerInputSettings()); }
        void TearDown() override { FInputActionMap::Get().SetPlayerSettings(SPlayerInputSettings()); }
    };
}

TEST_F(InputTriggerTests, DoubleTapFiresOnASecondPressInsideTheWindow)
{
    SInputAction Dodge = MakeDigital("Dodge", Lumina::EKey::Q);
    Dodge.DoubleTapTime = 0.3f;
    Author({ Dodge });
    FInputContext Context;

    PressKey(Context, Lumina::EKey::Q);
    Tick(Context);
    EXPECT_FALSE(ActionState("Dodge", Context).IsDoubleTapped());
    ReleaseKey(Context, Lumina::EKey::Q);
    Tick(Context);

    PressKey(Context, Lumina::EKey::Q);
    Tick(Context);
    EXPECT_TRUE(ActionState("Dodge", Context).IsDoubleTapped());
    ReleaseKey(Context, Lumina::EKey::Q);
    Tick(Context);

    // The third press starts a new pair rather than doubling again.
    PressKey(Context, Lumina::EKey::Q);
    Tick(Context);
    EXPECT_FALSE(ActionState("Dodge", Context).IsDoubleTapped());
}

TEST_F(InputTriggerTests, DoubleTapWindowExpires)
{
    SInputAction Dodge = MakeDigital("Dodge", Lumina::EKey::Q);
    Dodge.DoubleTapTime = 0.2f;
    Author({ Dodge });
    FInputContext Context;

    Tap(Context, Lumina::EKey::Q);
    Tick(Context);
    Tick(Context);

    PressKey(Context, Lumina::EKey::Q);
    Tick(Context);
    EXPECT_TRUE(ActionState("Dodge", Context).IsPressed());
    EXPECT_FALSE(ActionState("Dodge", Context).IsDoubleTapped());
}

TEST_F(InputTriggerTests, RepeatFiresOnPressThenAfterTheDelayAtTheInterval)
{
    SInputAction Scroll = MakeDigital("Scroll", Lumina::EKey::Down);
    Scroll.RepeatDelay = 0.25f;
    Scroll.RepeatInterval = 0.125f;
    Author({ Scroll });
    FInputContext Context;

    PressKey(Context, Lumina::EKey::Down);
    TVector<bool> Fired;
    for (int i = 0; i < 6; ++i)
    {
        Tick(Context);
        Fired.Add(ActionState("Scroll", Context).IsRepeated());
    }
    const TVector<bool> Expected = { true, false, true, true, true, true };
    EXPECT_EQ(Fired, Expected);

    ReleaseKey(Context, Lumina::EKey::Down);
    Tick(Context);
    EXPECT_FALSE(ActionState("Scroll", Context).IsRepeated());
}

TEST_F(InputTriggerTests, BufferedPressOutlivesTheReleaseUntilConsumedOrExpired)
{
    SInputAction Jump = MakeDigital("Jump", Lumina::EKey::Space);
    Jump.BufferTime = 0.25f;
    Author({ Jump });
    FInputContext Context;
    const int32 JumpIndex = FInputActionMap::Get().FindActionIndex(FName("Jump"));

    Tap(Context, Lumina::EKey::Space);
    EXPECT_TRUE(ActionState("Jump", Context).IsBuffered()) << "still buffered on the release frame";

    EXPECT_TRUE(Context.ConsumeBufferedPress(JumpIndex));
    EXPECT_FALSE(ActionState("Jump", Context).IsBuffered());
    EXPECT_FALSE(Context.ConsumeBufferedPress(JumpIndex)) << "a press acts once";
    Tick(Context);
    EXPECT_FALSE(ActionState("Jump", Context).IsBuffered());

    Tap(Context, Lumina::EKey::Space);
    Tick(Context);
    EXPECT_FALSE(ActionState("Jump", Context).IsBuffered()) << "expired after BufferTime";
    EXPECT_FALSE(Context.ConsumeBufferedPress(JumpIndex));
}

TEST_F(InputTriggerTests, ChordActionNeedsItsPartnerDown)
{
    SInputAction Special = MakeDigital("Special", Lumina::EKey::E);
    Special.ChordAction = FName("Modifier");
    Author({ MakeDigital("Modifier", Lumina::EKey::LeftShift), Special });
    FInputContext Context;

    PressKey(Context, Lumina::EKey::E);
    Tick(Context);
    EXPECT_FALSE(ActionState("Special", Context).IsDown());

    PressKey(Context, Lumina::EKey::LeftShift);
    Tick(Context);
    EXPECT_TRUE(ActionState("Special", Context).IsPressed());

    ReleaseKey(Context, Lumina::EKey::LeftShift);
    Tick(Context);
    EXPECT_TRUE(ActionState("Special", Context).IsReleased());
}

TEST_F(InputTriggerTests, ChordOnAMissingActionNeverFires)
{
    SInputAction Special = MakeDigital("Special", Lumina::EKey::E);
    Special.ChordAction = FName("NotAuthored");
    Author({ Special });
    FInputContext Context;

    PressKey(Context, Lumina::EKey::E);
    Tick(Context);
    EXPECT_FALSE(ActionState("Special", Context).IsDown());
}

TEST_F(InputTriggerTests, ToggleFlipsOnEachPressAndSurvivesASettingsChange)
{
    Author({ MakeDigital("Crouch", Lumina::EKey::C) });
    SPlayerInputSettings Settings;
    Settings.ToggleActions.Emplace("Crouch");
    FInputActionMap::Get().SetPlayerSettings(Settings);
    FInputContext Context;

    Tap(Context, Lumina::EKey::C);
    EXPECT_TRUE(ActionState("Crouch", Context).IsDown());
    Tick(Context);
    EXPECT_TRUE(ActionState("Crouch", Context).IsDown());

    Settings.MouseSensitivity = 2.0f;
    FInputActionMap::Get().SetPlayerSettings(Settings);
    Tick(Context);
    EXPECT_TRUE(ActionState("Crouch", Context).IsDown()) << "a sensitivity change keeps the toggle";

    PressKey(Context, Lumina::EKey::C);
    Tick(Context);
    EXPECT_FALSE(ActionState("Crouch", Context).IsDown());
    EXPECT_TRUE(ActionState("Crouch", Context).IsReleased());
}

TEST_F(InputTriggerTests, BindingOverrideReplacesOnlyItsKeySlot)
{
    SInputAction Jump = MakeAction("Jump", Lumina::EInputActionType::Digital, {
        KeyBinding(SKey(Lumina::EKey::Space)), KeyBinding(SKey(Lumina::EGamepadButton::FaceBottom)) });
    SInputAction Look = MakeAction("Look", Lumina::EInputActionType::Axis1D, { SourceBinding(Lumina::EInputAxisSource::MouseX) });
    Author({ Jump, Look });

    SPlayerInputSettings Settings;
    Settings.BindingOverrides.Emplace(FName("Jump"), 0, SKey(Lumina::EKey::J));
    Settings.BindingOverrides.Emplace(FName("Look"), 0, SKey(Lumina::EKey::L));
    Settings.BindingOverrides.Emplace(FName("Jump"), 7, SKey(Lumina::EKey::K));
    FInputActionMap::Get().SetPlayerSettings(Settings);

    const SInputAction* Rebound = FInputActionMap::Get().FindAction(FName("Jump"));
    ASSERT_NE(Rebound, nullptr);
    EXPECT_TRUE(Rebound->Bindings[0].Key == SKey(Lumina::EKey::J));
    EXPECT_TRUE(Rebound->Bindings[1].Key == SKey(Lumina::EGamepadButton::FaceBottom));
    EXPECT_FALSE(FInputActionMap::Get().FindAction(FName("Look"))->Bindings[0].Key.IsValid()) << "an axis source is not rebindable";

    FInputContext Context;
    PressKey(Context, Lumina::EKey::Space);
    Tick(Context);
    EXPECT_FALSE(ActionState("Jump", Context).IsDown());
    PressKey(Context, Lumina::EKey::J);
    Tick(Context);
    EXPECT_TRUE(ActionState("Jump", Context).IsDown());

    FInputActionMap::Get().SetPlayerSettings(SPlayerInputSettings());
    EXPECT_TRUE(FInputActionMap::Get().FindAction(FName("Jump"))->Bindings[0].Key == SKey(Lumina::EKey::Space)) << "dropping the override restores the authored key";
}

TEST_F(InputTriggerTests, FindActionUsingKeyReportsAConflict)
{
    Author({ MakeDigital("Jump", Lumina::EKey::Space), MakeDigital("Use", Lumina::EKey::E) });
    const FInputActionMap& Map = FInputActionMap::Get();

    EXPECT_EQ(Map.FindActionUsingKey(SKey(Lumina::EKey::E), FName("Jump")), FName("Use"));
    EXPECT_TRUE(Map.FindActionUsingKey(SKey(Lumina::EKey::E), FName("Use")).IsNone());
    EXPECT_TRUE(Map.FindActionUsingKey(SKey(Lumina::EKey::Z), FName()).IsNone());
}

TEST_F(InputTriggerTests, MouseSensitivityAndInvertApplyToMouseLookOnly)
{
    Author({ MakeAction("Look", Lumina::EInputActionType::Axis2D, {
        SourceBinding(Lumina::EInputAxisSource::MouseX),
        [] { SInputActionBinding B = SourceBinding(Lumina::EInputAxisSource::MouseY); B.Channel = Lumina::EInputAxisChannel::Y; return B; }() }) });
    SPlayerInputSettings Settings;
    Settings.MouseSensitivity = 2.0f;
    Settings.bInvertMouseY = true;
    FInputActionMap::Get().SetPlayerSettings(Settings);
    FInputContext Context;

    MoveMouse(Context, 3.0f, 4.0f);
    Tick(Context);
    EXPECT_FLOAT_EQ(ActionState("Look", Context).X, 6.0f);
    EXPECT_FLOAT_EQ(ActionState("Look", Context).Y, -8.0f);
}

TEST_F(InputTriggerTests, GamepadLookSettingsLeaveTheLeftStickAlone)
{
    Author({
        MakeAction("LookY", Lumina::EInputActionType::Axis1D, { SourceBinding(Lumina::EInputAxisSource::GamepadRightY) }),
        MakeAction("MoveY", Lumina::EInputActionType::Axis1D, { SourceBinding(Lumina::EInputAxisSource::GamepadLeftY) }) });
    SPlayerInputSettings Settings;
    Settings.GamepadLookSensitivity = 2.0f;
    Settings.bInvertGamepadLookY = true;
    FInputActionMap::Get().SetPlayerSettings(Settings);
    FInputContext Context;

    FGamepadState Pad = PadWithAxis(Lumina::EGamepadAxis::RightY, 0.25f);
    Pad.Axes[(uint32)Lumina::EGamepadAxis::LeftY] = 0.25f;
    Context.SetGamepadState(Pad);
    FInputActionMap::Get().UpdateContext(Context, kStep);
    EXPECT_FLOAT_EQ(ActionState("LookY", Context).X, -0.5f);
    EXPECT_FLOAT_EQ(ActionState("MoveY", Context).X, 0.25f);
}

TEST_F(InputTriggerTests, BindingDeadZoneAndResponseCurveShapeAPadAxis)
{
    SInputActionBinding Throttle = SourceBinding(Lumina::EInputAxisSource::GamepadRightTrigger);
    Throttle.DeadZone = 0.2f;
    Throttle.ResponseExponent = 2.0f;
    Author({ MakeAction("Throttle", Lumina::EInputActionType::Axis1D, { Throttle }) });
    FInputContext Context;

    Context.SetGamepadState(PadWithAxis(Lumina::EGamepadAxis::RightTrigger, 0.1f));
    FInputActionMap::Get().UpdateContext(Context, kStep);
    EXPECT_FLOAT_EQ(ActionState("Throttle", Context).X, 0.0f);

    // Rescaled past the dead zone to 0.5, then squared.
    Context.SetGamepadState(PadWithAxis(Lumina::EGamepadAxis::RightTrigger, 0.6f));
    FInputActionMap::Get().UpdateContext(Context, kStep);
    EXPECT_NEAR(ActionState("Throttle", Context).X, 0.25f, 1e-5f);
}

TEST_F(InputTriggerTests, LastUsedDeviceFollowsRealInputAndIgnoresJitter)
{
    FInputContext Context;
    EXPECT_EQ(Context.GetLastUsedDevice(), Lumina::EInputDevice::KeyboardMouse);

    FGamepadState Pad;
    Pad.bConnected = true;
    Pad.Buttons[(uint32)Lumina::EGamepadButton::FaceBottom] = Input::EButtonState::Pressed;
    Context.SetGamepadState(Pad);
    EXPECT_EQ(Context.GetLastUsedDevice(), Lumina::EInputDevice::Gamepad);

    MoveMouse(Context, 1.0f, 0.0f);
    EXPECT_EQ(Context.GetLastUsedDevice(), Lumina::EInputDevice::Gamepad) << "a nudged mouse is not a device switch";

    MoveMouse(Context, 20.0f, 0.0f);
    EXPECT_EQ(Context.GetLastUsedDevice(), Lumina::EInputDevice::KeyboardMouse);

    Context.SetGamepadState(PadWithAxis(Lumina::EGamepadAxis::LeftX, 0.8f));
    EXPECT_EQ(Context.GetLastUsedDevice(), Lumina::EInputDevice::Gamepad);

    PressKey(Context, Lumina::EKey::W);
    EXPECT_EQ(Context.GetLastUsedDevice(), Lumina::EInputDevice::KeyboardMouse);
}

TEST_F(InputTriggerTests, PromptLabelsFollowThePadFamily)
{
    EXPECT_EQ(GetGamepadButtonDisplayName(Lumina::EGamepadButton::FaceBottom, Lumina::EGamepadFamily::Xbox), FString("A"));
    EXPECT_EQ(GetGamepadButtonDisplayName(Lumina::EGamepadButton::FaceBottom, Lumina::EGamepadFamily::PlayStation), FString("Cross"));
    EXPECT_EQ(GetGamepadButtonDisplayName(Lumina::EGamepadButton::FaceBottom, Lumina::EGamepadFamily::Switch), FString("B"));
    EXPECT_EQ(GetGamepadButtonDisplayName(Lumina::EGamepadButton::FaceBottom, Lumina::EGamepadFamily::Generic), FString("A"));
    EXPECT_EQ(GetGamepadButtonDisplayName(Lumina::EGamepadButton::DPadUp, Lumina::EGamepadFamily::PlayStation), FString("D-Pad Up"));

    EXPECT_EQ(SKey(Lumina::EGamepadButton::RightShoulder).GetDisplayName(Lumina::EGamepadFamily::PlayStation), FString("Gamepad R1"));
    EXPECT_EQ(SourceBinding(Lumina::EInputAxisSource::GamepadLeftTrigger).GetDisplayName(Lumina::EGamepadFamily::Switch), FString("Gamepad ZL"));
}
