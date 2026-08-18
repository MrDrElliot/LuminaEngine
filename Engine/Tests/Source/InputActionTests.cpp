#include <gtest/gtest.h>

#include "Config/InputSettings.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Events/Event.h"
#include "Input/InputActionMap.h"
#include "Input/InputContext.h"

using namespace Lumina;

// The Reflector forward-declares reflected enums at global scope as well as in Lumina, so an unqualified
// EKey / EInputActionType is ambiguous under the using-directive above. Enum values are spelled out.

namespace
{
    // One frame of the real input flow: whatever the caller pumped through OnEvent is evaluated into the
    // context's action states, then EndFrame rolls Pressed->Held and clears the frame's motion. Mirrors
    // FInputViewportRegistry::BeginFrame / EndFrame around the world update.
    void Tick(FInputContext& Context, float DeltaSeconds = 1.0f / 60.0f)
    {
        FInputActionMap::Get().UpdateContext(Context, DeltaSeconds);
        Context.EndFrame(DeltaSeconds);
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

    SInputActionBinding KeyBinding(Lumina::EKey Key, float Scale = 1.0f)
    {
        SInputActionBinding Binding;
        Binding.Key.SetKey(Key);
        Binding.Scale = Scale;
        return Binding;
    }

    // Replaces the project's action list with this test's and rebuilds the map (which also bumps the
    // serial, so every context re-sizes its state array on the next update).
    void Author(const TVector<SInputAction>& Actions, const TVector<SInputMappingContext>& Layers = {})
    {
        GetMutableDefault<CInputSettings>()->Actions = Actions;
        GetMutableDefault<CInputSettings>()->MappingContexts = Layers;
        FInputActionMap::Get().RebuildFromSettings();
    }

    SInputMappingContext MakeLayer(const char* Name, const TVector<FName>& Actions, bool bBlockLower = true)
    {
        SInputMappingContext Layer;
        Layer.Name = FName(Name);
        Layer.Actions = Actions;
        Layer.bBlockLower = bBlockLower;
        return Layer;
    }

    SInputAction MakeDigital(const char* Name, Lumina::EKey Key)
    {
        SInputAction Action;
        Action.Name = FName(Name);
        Action.Type = Lumina::EInputActionType::Digital;
        Action.Bindings.push_back(KeyBinding(Key));
        return Action;
    }
}

TEST(InputActionTests, DigitalEdgesFireExactlyOnce)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    const FName Fire("Fire");

    Tick(Context);
    EXPECT_FALSE(Map.IsActionDown(Fire, Context));

    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(Fire, Context));
    EXPECT_TRUE(Map.IsActionPressed(Fire, Context));
    EXPECT_FALSE(Map.IsActionReleased(Fire, Context));

    // Held: still down, but the press edge must not repeat.
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(Fire, Context));
    EXPECT_FALSE(Map.IsActionPressed(Fire, Context));

    ReleaseKey(Context, Lumina::EKey::F);
    Tick(Context);
    EXPECT_FALSE(Map.IsActionDown(Fire, Context));
    EXPECT_TRUE(Map.IsActionReleased(Fire, Context));

    Tick(Context);
    EXPECT_FALSE(Map.IsActionReleased(Fire, Context));
}

TEST(InputActionTests, HoldTimeGatesHeld)
{
    SInputAction Action = MakeDigital("Charge", Lumina::EKey::C);
    Action.HoldTime = 0.5f;
    Author({ Action });

    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    const FName Charge("Charge");

    PressKey(Context, Lumina::EKey::C);
    Tick(Context, 0.1f);
    EXPECT_TRUE(Map.IsActionDown(Charge, Context));
    EXPECT_FALSE(Map.IsActionHeld(Charge, Context)) << "held must wait for HoldTime";

    for (int i = 0; i < 4; ++i)
    {
        Tick(Context, 0.1f);
    }
    EXPECT_FLOAT_EQ(Map.GetActionHeldTime(Charge, Context), 0.4f);
    EXPECT_FALSE(Map.IsActionHeld(Charge, Context));

    Tick(Context, 0.2f);
    EXPECT_TRUE(Map.IsActionHeld(Charge, Context));
}

TEST(InputActionTests, HoldTimeZeroIsHeldFromTheFirstFrame)
{
    Author({ MakeDigital("Move", Lumina::EKey::W) });
    FInputContext Context;

    PressKey(Context, Lumina::EKey::W);
    Tick(Context);
    EXPECT_TRUE(FInputActionMap::Get().IsActionHeld(FName("Move"), Context));
}

TEST(InputActionTests, TapOnlyOnAShortPress)
{
    SInputAction Action = MakeDigital("Interact", Lumina::EKey::E);
    Action.TapTime = 0.2f;
    Author({ Action });

    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    const FName Interact("Interact");

    PressKey(Context, Lumina::EKey::E);
    Tick(Context, 0.1f);
    ReleaseKey(Context, Lumina::EKey::E);
    Tick(Context, 0.1f);
    EXPECT_TRUE(Map.WasActionTapped(Interact, Context));

    // Same action held well past TapTime: released, but not a tap.
    PressKey(Context, Lumina::EKey::E);
    Tick(Context, 0.1f);
    for (int i = 0; i < 5; ++i)
    {
        Tick(Context, 0.1f);
    }
    ReleaseKey(Context, Lumina::EKey::E);
    Tick(Context, 0.1f);
    EXPECT_TRUE(Map.IsActionReleased(Interact, Context));
    EXPECT_FALSE(Map.WasActionTapped(Interact, Context));
}

TEST(InputActionTests, Axis1DSumsOpposingBindings)
{
    SInputAction Action;
    Action.Name = FName("MoveForward");
    Action.Type = Lumina::EInputActionType::Axis1D;
    Action.Bindings.push_back(KeyBinding(Lumina::EKey::W, 1.0f));
    Action.Bindings.push_back(KeyBinding(Lumina::EKey::S, -1.0f));
    Author({ Action });

    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    const FName Move("MoveForward");

    PressKey(Context, Lumina::EKey::W);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Move, Context), 1.0f);

    PressKey(Context, Lumina::EKey::S);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Move, Context), 0.0f) << "opposing keys cancel";

    ReleaseKey(Context, Lumina::EKey::W);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Move, Context), -1.0f);
}

TEST(InputActionTests, Axis2DSplitsChannels)
{
    SInputAction Action;
    Action.Name = FName("Move");
    Action.Type = Lumina::EInputActionType::Axis2D;

    SInputActionBinding Right = KeyBinding(Lumina::EKey::D, 1.0f);
    Right.Channel = Lumina::EInputAxisChannel::X;
    SInputActionBinding Forward = KeyBinding(Lumina::EKey::W, 1.0f);
    Forward.Channel = Lumina::EInputAxisChannel::Y;
    Action.Bindings.push_back(Right);
    Action.Bindings.push_back(Forward);
    Author({ Action });

    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    const FName Move("Move");

    PressKey(Context, Lumina::EKey::D);
    PressKey(Context, Lumina::EKey::W);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Move, Context), 1.0f);
    EXPECT_FLOAT_EQ(Map.GetActionAxisY(Move, Context), 1.0f);
    EXPECT_TRUE(Map.IsActionDown(Move, Context));
}

TEST(InputActionTests, MouseSourceFeedsAnAxisWithSensitivity)
{
    SInputAction Action;
    Action.Name = FName("Look");
    Action.Type = Lumina::EInputActionType::Axis1D;
    Action.Sensitivity = 0.5f;
    SInputActionBinding Binding;
    Binding.Source = Lumina::EInputAxisSource::MouseX;
    Binding.Scale = 1.0f;
    Action.Bindings.push_back(Binding);
    Author({ Action });

    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    const FName Look("Look");

    MoveMouse(Context, 10.0f, 0.0f);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Look, Context), 5.0f);
    EXPECT_TRUE(Map.IsActionDown(Look, Context)) << "a moving axis reads as down";

    // Motion is per frame: with no new movement the axis falls back to zero and reports its release.
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Look, Context), 0.0f);
    EXPECT_TRUE(Map.IsActionReleased(Look, Context));
}

TEST(InputActionTests, DeadZoneCutsSmallInputAndRescalesTheRest)
{
    SInputAction Action;
    Action.Name = FName("Look");
    Action.Type = Lumina::EInputActionType::Axis1D;
    Action.DeadZone = 0.5f;
    SInputActionBinding Binding;
    Binding.Source = Lumina::EInputAxisSource::MouseX;
    Binding.Scale = 1.0f;
    Action.Bindings.push_back(Binding);
    Author({ Action });

    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    const FName Look("Look");

    MoveMouse(Context, 0.25f, 0.0f);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Look, Context), 0.0f) << "inside the dead zone";

    // 0.75 with a 0.5 dead zone rescales to (0.75 - 0.5) / (1 - 0.5) == 0.5.
    MoveMouse(Context, 0.75f, 0.0f);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Look, Context), 0.5f);

    // Sign is preserved by the rescale.
    MoveMouse(Context, -0.75f, 0.0f);
    Tick(Context);
    EXPECT_FLOAT_EQ(Map.GetActionAxis(Look, Context), -0.5f);
}

TEST(InputActionTests, InvertFlipsTheValue)
{
    SInputAction Action;
    Action.Name = FName("Look");
    Action.Type = Lumina::EInputActionType::Axis1D;
    Action.bInvert = true;
    SInputActionBinding Binding;
    Binding.Source = Lumina::EInputAxisSource::MouseY;
    Action.Bindings.push_back(Binding);
    Author({ Action });

    FInputContext Context;
    MoveMouse(Context, 0.0f, 3.0f);
    Tick(Context);
    EXPECT_FLOAT_EQ(FInputActionMap::Get().GetActionAxis(FName("Look"), Context), -3.0f);
}

TEST(InputActionTests, UIModeGatesActionsUnlessTheyRunInUI)
{
    SInputAction Gated = MakeDigital("Fire", Lumina::EKey::F);
    SInputAction Ungated = MakeDigital("Pause", Lumina::EKey::P);
    Ungated.bRunsInUI = true;
    Author({ Gated, Ungated });

    FInputContext Context;
    Context.SetInputMode(Lumina::EInputMode::UI);
    const FInputActionMap& Map = FInputActionMap::Get();

    PressKey(Context, Lumina::EKey::F);
    PressKey(Context, Lumina::EKey::P);
    Tick(Context);

    EXPECT_FALSE(Map.IsActionDown(FName("Fire"), Context));
    EXPECT_TRUE(Map.IsActionDown(FName("Pause"), Context));
}

TEST(InputActionTests, LegacyAxisFlagMigratesToTheAxisType)
{
    SInputAction Action;
    Action.Name = FName("Legacy");
    Action.bAxis = true;   // authored before EInputActionType existed
    Action.Bindings.push_back(KeyBinding(Lumina::EKey::W, 2.0f));
    Author({ Action });

    const SInputAction* Resolved = FInputActionMap::Get().FindAction(FName("Legacy"));
    ASSERT_NE(Resolved, nullptr);
    EXPECT_EQ(Resolved->Type, Lumina::EInputActionType::Axis1D);

    FInputContext Context;
    PressKey(Context, Lumina::EKey::W);
    Tick(Context);
    EXPECT_FLOAT_EQ(FInputActionMap::Get().GetActionAxis(FName("Legacy"), Context), 2.0f);
}

TEST(InputActionTests, UnknownActionReadsAsZero)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F) });
    FInputContext Context;
    Tick(Context);

    const FInputActionMap& Map = FInputActionMap::Get();
    EXPECT_EQ(Map.FindActionIndex(FName("NoSuchAction")), INDEX_NONE);
    EXPECT_FALSE(Map.IsActionDown(FName("NoSuchAction"), Context));
    EXPECT_FLOAT_EQ(Map.GetActionAxis(FName("NoSuchAction"), Context), 0.0f);
}

TEST(InputActionTests, RebuildInvalidatesStaleContextState)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    ASSERT_TRUE(Map.IsActionDown(FName("Fire"), Context));

    // A settings change bumps the serial; the context's states are stale until its next update, and must
    // not be read against the new indices in the meantime.
    const uint32 Before = Map.GetSerial();
    Author({ MakeDigital("Jump", Lumina::EKey::Space), MakeDigital("Fire", Lumina::EKey::F) });
    EXPECT_GT(Map.GetSerial(), Before);
    EXPECT_FALSE(Map.IsActionDown(FName("Fire"), Context)) << "stale state must not be read";

    // The state array is rebuilt from scratch rather than remapped by name, so a key still physically
    // down re-reports its press. Only an input-settings save can cause this, and re-arming a held action
    // is the safe side of the trade: the alternative is reading a stale index and reporting the wrong
    // action's state.
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(FName("Fire"), Context));
    EXPECT_TRUE(Map.IsActionPressed(FName("Fire"), Context));
}

TEST(InputActionTests, HandleResolvesOnceAndTracksIndex)
{
    Author({ MakeDigital("Jump", Lumina::EKey::Space), MakeDigital("Fire", Lumina::EKey::F) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    FInputActionHandle Fire(FName("Fire"));
    EXPECT_TRUE(Fire.IsSet());

    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    EXPECT_TRUE(Map.GetActionState(Fire, Context).IsDown());
    EXPECT_TRUE(Map.GetActionState(Fire, Context).IsPressed());

    // The handle and the name overload are the same query; the handle just skips the hash.
    EXPECT_EQ(Map.GetActionState(Fire, Context).X, Map.GetActionState(FName("Fire"), Context).X);
}

TEST(InputActionTests, HandleFollowsActionAcrossReorder)
{
    Author({ MakeDigital("Jump", Lumina::EKey::Space), MakeDigital("Fire", Lumina::EKey::F) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    FInputActionHandle Fire(FName("Fire"));
    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    ASSERT_TRUE(Map.GetActionState(Fire, Context).IsDown());

    // Re-authoring moves Fire from index 1 to index 0. A handle caching the index alone would now read
    // Jump; because it re-resolves from the NAME on a serial bump, it still answers for Fire.
    Author({ MakeDigital("Fire", Lumina::EKey::F), MakeDigital("Jump", Lumina::EKey::Space) });
    Tick(Context);
    EXPECT_TRUE(Map.GetActionState(Fire, Context).IsDown());

    FInputActionHandle Jump(FName("Jump"));
    EXPECT_FALSE(Map.GetActionState(Jump, Context).IsDown());
}

TEST(InputActionTests, HandleForUnknownActionReadsZero)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    FInputActionHandle Missing(FName("NotAnAction"));
    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    EXPECT_FALSE(Map.GetActionState(Missing, Context).IsDown());
    EXPECT_EQ(Map.GetActionState(Missing, Context).X, 0.0f);

    // A default-constructed handle names nothing and must also read as zero rather than index 0.
    FInputActionHandle Unset;
    EXPECT_FALSE(Unset.IsSet());
    EXPECT_FALSE(Map.GetActionState(Unset, Context).IsDown());
}

TEST(InputActionTests, HandleSetNameDropsTheOldAction)
{
    Author({ MakeDigital("Jump", Lumina::EKey::Space), MakeDigital("Fire", Lumina::EKey::F) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    FInputActionHandle Handle(FName("Fire"));
    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    ASSERT_TRUE(Map.GetActionState(Handle, Context).IsDown());

    // Retargeting must not keep answering with the previous action's cached index.
    Handle.SetName(FName("Jump"));
    EXPECT_FALSE(Map.GetActionState(Handle, Context).IsDown());
}

TEST(InputActionTests, NoLayersLeavesEveryActionFiring)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F), MakeDigital("Pause", Lumina::EKey::Escape) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(FName("Fire"), Context)) << "an empty layer stack must gate nothing";
}

TEST(InputActionTests, BlockingLayerSwallowsActionsItDoesNotList)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F), MakeDigital("MenuConfirm", Lumina::EKey::Space) },
           { MakeLayer("Menu", { FName("MenuConfirm") }) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    PressKey(Context, Lumina::EKey::F);
    PressKey(Context, Lumina::EKey::Space);
    Tick(Context);
    ASSERT_TRUE(Map.IsActionDown(FName("Fire"), Context));
    ASSERT_TRUE(Map.IsActionDown(FName("MenuConfirm"), Context));

    // With the menu up, gameplay stops and only the menu's own action survives.
    Context.PushInputLayer(FName("Menu"));
    Tick(Context);
    EXPECT_FALSE(Map.IsActionDown(FName("Fire"), Context));
    EXPECT_TRUE(Map.IsActionDown(FName("MenuConfirm"), Context));

    Context.PopInputLayer(FName("Menu"));
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(FName("Fire"), Context));
}

TEST(InputActionTests, NonBlockingLayerOnlyAdds)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F), MakeDigital("Photo", Lumina::EKey::P) },
           { MakeLayer("Photo", { FName("Photo") }, false) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    Context.PushInputLayer(FName("Photo"));
    PressKey(Context, Lumina::EKey::F);
    PressKey(Context, Lumina::EKey::P);
    Tick(Context);

    // bBlockLower false: the layer adds nothing to the gate, so gameplay underneath keeps firing.
    EXPECT_TRUE(Map.IsActionDown(FName("Photo"), Context));
    EXPECT_TRUE(Map.IsActionDown(FName("Fire"), Context));
}

TEST(InputActionTests, TopmostLayerDecidesFirst)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F), MakeDigital("MenuConfirm", Lumina::EKey::Space) },
           { MakeLayer("Menu", { FName("MenuConfirm") }),
             MakeLayer("Cutscene", { FName("Fire") }) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    PressKey(Context, Lumina::EKey::F);
    PressKey(Context, Lumina::EKey::Space);

    // Cutscene sits above Menu, so its allow-list wins and Menu never gets consulted.
    Context.PushInputLayer(FName("Menu"));
    Context.PushInputLayer(FName("Cutscene"));
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(FName("Fire"), Context));
    EXPECT_FALSE(Map.IsActionDown(FName("MenuConfirm"), Context));
}

TEST(InputActionTests, RepushingALayerDoesNotStackIt)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F) }, { MakeLayer("Menu", {}) });
    FInputContext Context;

    Context.PushInputLayer(FName("Menu"));
    Context.PushInputLayer(FName("Menu"));
    EXPECT_EQ(Context.GetInputLayers().size(), 1u) << "a double push must not need two pops";

    EXPECT_TRUE(Context.PopInputLayer(FName("Menu")));
    EXPECT_FALSE(Context.HasInputLayer(FName("Menu")));
    EXPECT_FALSE(Context.PopInputLayer(FName("Menu")));
}

TEST(InputActionTests, UnknownLayerOnTheStackIsIgnored)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F) }, { MakeLayer("Menu", {}) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    // A layer deleted from the settings while pushed must not gate everything off; it just stops matching.
    Context.PushInputLayer(FName("NotAuthored"));
    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(FName("Fire"), Context));
}

TEST(InputActionTests, UIModeStillGatesWhenNoLayersArePushed)
{
    SInputAction Fire = MakeDigital("Fire", Lumina::EKey::F);
    SInputAction Pause = MakeDigital("Pause", Lumina::EKey::Escape);
    Pause.bRunsInUI = true;
    Author({ Fire, Pause });

    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();
    Context.SetInputMode(Lumina::EInputMode::UI);

    PressKey(Context, Lumina::EKey::F);
    PressKey(Context, Lumina::EKey::Escape);
    Tick(Context);

    // bRunsInUI predates mapping layers and remains the fallback for projects that author none.
    EXPECT_FALSE(Map.IsActionDown(FName("Fire"), Context));
    EXPECT_TRUE(Map.IsActionDown(FName("Pause"), Context));
}

TEST(InputActionTests, LayersTakePrecedenceOverUIMode)
{
    Author({ MakeDigital("Fire", Lumina::EKey::F) }, { MakeLayer("Cutscene", { FName("Fire") }) });
    FInputContext Context;
    const FInputActionMap& Map = FInputActionMap::Get();

    // A layer that lists the action allows it even in UI mode, where bRunsInUI alone would have refused.
    Context.SetInputMode(Lumina::EInputMode::UI);
    Context.PushInputLayer(FName("Cutscene"));
    PressKey(Context, Lumina::EKey::F);
    Tick(Context);
    EXPECT_TRUE(Map.IsActionDown(FName("Fire"), Context));
}
