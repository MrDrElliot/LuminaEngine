#pragma once

#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Events/EventProcessor.h"
#include "Events/MouseCodes.h"
#include "Input/Gamepad.h"
#include "Input/InputMode.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CWorld;
    class FEvent;
    class FInputContext;

    class FInputViewport
    {
    public:

        RUNTIME_API FInputViewport();
        RUNTIME_API ~FInputViewport();
        LE_NO_COPYMOVE(FInputViewport);

        // A new world brings a new UI context that has never seen the cursor, so the move dedupe starts over.
        RUNTIME_API void   SetWorld(CWorld* InWorld) { World = InWorld; LastRmlMouseX = -1.0; LastRmlMouseY = -1.0; }
        RUNTIME_API CWorld* GetWorld() const { return World; }

        RUNTIME_API void SetWindowRect(int MinX, int MinY, int MaxX, int MaxY);
        RUNTIME_API void SetRenderTargetSize(uint32 W, uint32 H);

        RUNTIME_API FInputContext&       GetContext()       { return *Context; }
        RUNTIME_API const FInputContext& GetContext() const { return *Context; }

        // The OS window this viewport is drawn into, set each frame by the editor so capture targets a detached preview's own window.
        RUNTIME_API void  SetNativeWindowHandle(void* InHandle) { NativeWindowHandle = InHandle; }
        RUNTIME_API void* GetNativeWindowHandle() const { return NativeWindowHandle; }

        bool RouteEvent(FEvent& Event);
        bool ContainsWindowPoint(int WindowX, int WindowY) const;

    private:

        bool ForwardKeyEventToRmlUi(FEvent& Event);
        bool ForwardMouseEventToRmlUi(FEvent& Event);

        // The window reports moves that go nowhere, and each one would wake the UI for a frame of layout and drawing.
        double LastRmlMouseX = -1.0;
        double LastRmlMouseY = -1.0;
        bool   bLastRmlMoveConsumed = false;

        TUniquePtr<FInputContext> Context;
        CWorld* World = nullptr;
        void*   NativeWindowHandle = nullptr;
    };

    // Decides who gets input. The active viewport owns keyboard, gamepad and gameplay reads, and the mouse follows press then hover.
    class FInputViewportRegistry : public IEventHandler
    {
    public:

        RUNTIME_API static FInputViewportRegistry& Get();

        RUNTIME_API void Register(FInputViewport* Viewport);
        RUNTIME_API void Unregister(FInputViewport* Viewport);

        RUNTIME_API FInputViewport* GetActiveViewport()  const { return ActiveViewport; }
        RUNTIME_API FInputViewport* GetHoveredViewport() const { return HoveredViewport; }

        // Every device unfiltered, for editor tools such as the fly camera that read input outside any viewport's gating.
        RUNTIME_API const FInputContext& GetRawInput() const { return *RawInput; }

        // False while the editor holds input, so no game world reads any even when its viewport is active.
        RUNTIME_API bool IsGameInputFocused() const { return bGameInputFocused; }
        RUNTIME_API void SetGameInputFocused(bool bFocused);

        // A game's developer overlay holds the keyboard, mouse and pad while open, freeing the cursor without touching the game's own mode.
        RUNTIME_API void SetDevOverlayCapturing(bool bCapturing);

        // A game world is the active viewport and holds game focus, so editor UI must leave the keyboard and mouse alone.
        RUNTIME_API bool IsGameOwningInput() const;

        // The cursor mode the active window shows right now, which forces Normal while the editor holds a game world's input.
        RUNTIME_API EMouseMode GetActiveEffectiveMouseMode() const;

        RUNTIME_API FInputViewport* FindViewportForWorld(const CWorld* World) const;

        // Points every viewport showing OldWorld at NewWorld, for a level travel that swapped one world for another.
        RUNTIME_API void ReplaceWorld(const CWorld* OldWorld, CWorld* NewWorld);

        // Changing it drops the old owner's held state, so a key held across the switch never reappears as down.
        RUNTIME_API void SetActiveViewport(FInputViewport* Viewport);
        RUNTIME_API void SetHoveredViewport(FInputViewport* Viewport);

        // Called once a frame per drawn viewport. Ownership follows the focused OS window under game focus, else the editor's focus.
        RUNTIME_API void UpdateViewportFocus(FInputViewport* Viewport, bool bHovered, bool bEditorFocused);

        // Makes Viewport the owner with game focus, as a click into a playing viewport or an injected key does.
        RUNTIME_API void TakeGameFocus(FInputViewport* Viewport);

        // Sets the cursor of CallerWorld's own viewport, or of the active one when null, and applies it to the window.
        RUNTIME_API void SetMouseMode(EMouseMode Mode, const CWorld* CallerWorld = nullptr);

        // Pushes the active viewport's effective cursor mode to its window, which a context mode change alone never does.
        RUNTIME_API void ApplyActiveCursorMode();

        // Sets the cursor of CallerWorld's own viewport, or of the active one when null.
        RUNTIME_API void SetMouseCursor(EMouseCursor Cursor, const CWorld* CallerWorld = nullptr);

        // The viewport whose cursor the window shows, which needs a visible cursor over the game that owns input, or null when the editor or system keeps it.
        RUNTIME_API const FInputViewport* GetGameCursorViewport() const;
        RUNTIME_API bool IsGameCursorShowing() const { return GetGameCursorViewport() != nullptr; }

        // Puts the cursor GetGameCursorViewport asks for on its window, from the project's hardware cursors or the system's own.
        RUNTIME_API void ApplyActiveCursor();

        RUNTIME_API const FGamepadDevices& GetGamepads() const { return Gamepads; }

        // Waits for the next key, mouse button or pad button and saves it as the player's binding, while Escape cancels.
        RUNTIME_API bool BeginRebind(FName Action, int32 BindingIndex);
        RUNTIME_API void CancelRebind() { RebindAction = FName(); }
        RUNTIME_API bool IsRebinding() const { return !RebindAction.IsNone(); }

        // Another action that already used the last captured key, for a menu to show as a conflict, or none.
        RUNTIME_API FName GetRebindConflict() const { return RebindConflict; }

        // Polls the gamepads and evaluates every input action into every context. Runs before the world update; EndFrame runs after.
        RUNTIME_API void BeginFrame(double DeltaSeconds);
        RUNTIME_API void EndFrame();
        RUNTIME_API void OnWindowFocusLost();

        // Pumps the window again just before extract when the captured viewport's camera turns with the mouse, returning whether it did.
        RUNTIME_API bool PumpLateMouseLook();

        // The yaw and pitch in degrees World's rendered view turns by for the motion PumpLateMouseLook held, or zero.
        RUNTIME_API FVector2 GetLateMouseLook(const CWorld* World) const;

        bool OnEvent(FEvent& Event) override;

    private:

        FInputViewportRegistry();

        bool IsActiveCursorCaptured() const;

        // The viewport a mouse event belongs to, keeping a press and its release in the same one.
        FInputViewport* ResolveMouseTarget(FEvent& Event);

        // Pads are global devices, so only the active viewport reads them, and only while its window has focus.
        bool IsActiveViewportReceivingGamepad() const;
        void FeedGamepads();
        void PollRebind();

        TVector<FInputViewport*> Viewports;
        FInputViewport* ActiveViewport  = nullptr;
        FInputViewport* HoveredViewport = nullptr;
        // Took a mouse press and keeps the mouse until every button is up, so a drag that leaves the panel still releases.
        FInputViewport* MousePressViewport = nullptr;
        bool            bGameInputFocused = true;
        bool            bDevOverlayCapturing = false;
        const CWorld*   LateLookWorld = nullptr;
        TUniquePtr<FInputContext> RawInput;
        FGamepadDevices Gamepads;

        FName RebindAction;
        int32 RebindIndex = 0;
        bool  bRebindArmed = false;
        FName RebindConflict;

        void* ResolveCursor(EMouseCursor Cursor);

        // OS cursors by image path and hotspot, and the system's own by type, which the platform frees when it shuts down.
        THashMap<FString, void*> HardwareCursorImages;
        void* SystemCursors[MouseCursorTypeCount] = {};
        void* AppliedCursorWindow = nullptr;
        bool  bGameCursorApplied  = false;
        int32 LastMouseWindowX    = -1;
        int32 LastMouseWindowY    = -1;
    };
}
