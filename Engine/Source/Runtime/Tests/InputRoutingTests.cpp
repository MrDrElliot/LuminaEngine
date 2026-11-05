#include <gtest/gtest.h>

#include "Events/Event.h"
#include "Input/InputContext.h"
#include "Input/InputViewport.h"

using namespace Lumina;

namespace
{
    // Registry state is global, so each test leaves it with no viewports and editor focus as it found it.
    struct FRoutingFixture
    {
        FInputViewportRegistry& Registry = FInputViewportRegistry::Get();
        FInputViewport A;
        FInputViewport B;
        const bool bWasGameFocused = Registry.IsGameInputFocused();

        FRoutingFixture()
        {
            A.SetWindowRect(0, 0, 100, 100);
            B.SetWindowRect(100, 0, 200, 100);
            Registry.Register(&A);
            Registry.Register(&B);
            Registry.SetGameInputFocused(false);
            Registry.SetActiveViewport(&A);
            Registry.SetHoveredViewport(nullptr);
        }

        ~FRoutingFixture()
        {
            Registry.Unregister(&A);
            Registry.Unregister(&B);
            Registry.SetGameInputFocused(bWasGameFocused);
        }

        void Key(Lumina::EKey Key, bool bPressed)
        {
            if (bPressed)
            {
                FKeyPressedEvent Event(Key, false, false, false, false);
                Registry.OnEvent(Event);
            }
            else
            {
                FKeyReleasedEvent Event(Key, false, false, false, false);
                Registry.OnEvent(Event);
            }
        }

        void Button(Lumina::EMouseKey Button, bool bPressed, float X)
        {
            if (bPressed)
            {
                FMouseButtonPressedEvent Event(Button, X, 10.0f);
                Registry.OnEvent(Event);
            }
            else
            {
                FMouseButtonReleasedEvent Event(Button, X, 10.0f);
                Registry.OnEvent(Event);
            }
        }
    };
}

TEST(InputRouting, KeysGoOnlyToTheActiveViewport)
{
    FRoutingFixture F;
    F.Registry.SetHoveredViewport(&F.B);

    F.Key(Lumina::EKey::W, true);
    EXPECT_TRUE(F.A.GetContext().IsKeyDownRaw(Lumina::EKey::W));
    EXPECT_FALSE(F.B.GetContext().IsKeyDownRaw(Lumina::EKey::W));
    F.Key(Lumina::EKey::W, false);
}

TEST(InputRouting, APressKeepsTheMouseUntilEveryButtonIsUp)
{
    FRoutingFixture F;
    F.Registry.SetHoveredViewport(&F.A);
    F.Button(Lumina::EMouseKey::ButtonLeft, true, 50.0f);
    EXPECT_TRUE(F.A.GetContext().IsMouseButtonDownRaw(Lumina::EMouseKey::ButtonLeft));

    // The drag leaves A for B, and the release must still reach A.
    F.Registry.SetHoveredViewport(&F.B);
    F.Button(Lumina::EMouseKey::ButtonLeft, false, 150.0f);
    EXPECT_FALSE(F.A.GetContext().IsMouseButtonDownRaw(Lumina::EMouseKey::ButtonLeft));
    EXPECT_FALSE(F.B.GetContext().GetFrameEvents().Num() > 0);

    // Once released, the next press goes to whatever is hovered.
    F.Button(Lumina::EMouseKey::ButtonLeft, true, 150.0f);
    EXPECT_TRUE(F.B.GetContext().IsMouseButtonDownRaw(Lumina::EMouseKey::ButtonLeft));
    F.Button(Lumina::EMouseKey::ButtonLeft, false, 150.0f);
}

TEST(InputRouting, MouseWithNothingHoveredFallsBackToTheActiveViewport)
{
    FRoutingFixture F;
    F.Registry.SetActiveViewport(&F.B);
    F.Button(Lumina::EMouseKey::ButtonRight, true, 500.0f);
    EXPECT_TRUE(F.B.GetContext().IsMouseButtonDownRaw(Lumina::EMouseKey::ButtonRight));
    F.Button(Lumina::EMouseKey::ButtonRight, false, 500.0f);
}

TEST(InputRouting, ChangingTheOwnerDropsHeldKeys)
{
    FRoutingFixture F;
    F.Key(Lumina::EKey::Space, true);
    ASSERT_TRUE(F.A.GetContext().IsKeyDownRaw(Lumina::EKey::Space));

    F.Registry.SetActiveViewport(&F.B);
    EXPECT_FALSE(F.A.GetContext().IsKeyDownRaw(Lumina::EKey::Space));
}

TEST(InputRouting, EditorFocusMovesOwnershipAndHover)
{
    FRoutingFixture F;
    F.Registry.UpdateViewportFocus(&F.B, true, true);
    EXPECT_EQ(F.Registry.GetActiveViewport(), &F.B);
    EXPECT_EQ(F.Registry.GetHoveredViewport(), &F.B);

    F.Registry.UpdateViewportFocus(&F.B, false, false);
    EXPECT_EQ(F.Registry.GetHoveredViewport(), nullptr);
    EXPECT_EQ(F.Registry.GetActiveViewport(), &F.B);
}

TEST(InputRouting, TakeGameFocusOwnsAndFocuses)
{
    FRoutingFixture F;
    F.Registry.TakeGameFocus(&F.B);
    EXPECT_EQ(F.Registry.GetActiveViewport(), &F.B);
    EXPECT_TRUE(F.Registry.IsGameInputFocused());

    // A viewport with no world is never a game world, so the editor keeps its UI.
    EXPECT_FALSE(F.Registry.IsGameOwningInput());
}

TEST(InputRouting, UnregisteringThePressedViewportReleasesTheLock)
{
    FRoutingFixture F;
    {
        FInputViewport C;
        F.Registry.Register(&C);
        F.Registry.SetHoveredViewport(&C);
        F.Button(Lumina::EMouseKey::ButtonLeft, true, 50.0f);
    }
    F.Registry.SetHoveredViewport(&F.B);
    F.Button(Lumina::EMouseKey::ButtonLeft, false, 150.0f);
    F.Button(Lumina::EMouseKey::ButtonLeft, true, 150.0f);
    EXPECT_TRUE(F.B.GetContext().IsMouseButtonDownRaw(Lumina::EMouseKey::ButtonLeft));
    F.Button(Lumina::EMouseKey::ButtonLeft, false, 150.0f);
}
