# Input system: shortcomings and a proposed architecture

Written 2026-08-17, from a read of the current code. Nothing here has been implemented.

## Summary

The action *model* is good and should survive: authored actions, `SKey` chords, dead zone, sensitivity,
invert, hold/tap, 2D channels, the editor picker, and a POD state struct mirrored byte for byte into C#.

The *delivery* is the problem, and one specific fact explains most of the pain: **the event-driven path is
not wired up at all**. Polling is not the recommended way to use this system, it is the only way that
currently works for C# scripts.

---

## Shortcomings

### 1. The push API is dead code

`Engine/Source/Runtime/Source/Scripting/DotNet/DotNetHost.h` declares three entry points:

- `GetScriptCallbackFlags(void*)`
- `DispatchScriptInput(void*, ...)`
- `PollScriptInput(void*, const FInputActionState*, int32, uint32, float)`

None of the three has a definition in any `.cpp`, and none has a caller anywhere in the tree.
`EntityScriptSystem.cpp` is 87 lines and dispatches only `Tick` and `OnFixedUpdate`.

Consequences for C# scripts, which is the primary gameplay language here:

- `SInputAction.Pressed` / `.Released` / `.Held` / `.Tapped` never fire.
- `SInputAxis.Changed` / `.Changed2D` never fire, and `.Value` never leaves its default.
- `EntityScript.OnInput` is never called.

The managed half is complete and waiting: `EntityScriptRuntime.PollInput` and
`TypeDescription.PollInputBindings` both exist and are correct. Only the native caller is missing.

C++ scripts are fine: `SInputSystem::Update` calls `EntityScripts::DispatchInput`, which walks
`CEntityScript` instances and invokes `OnInput`. That path never reaches a C# script, because those live on
`SEntityScriptComponent` behind the DotNet host.

**This is the single highest-value fix, and it is small.** Everything below is real but secondary.

### 2. The component stores a per-entity copy of state that is not per-entity

```cpp
TArray<Input::EKeyState,   (uint32)EKey::Num>      KeyStates;    // EKey::Num == 349
TArray<Input::EMouseState, (uint32)EMouseKey::Num> MouseStates;  // 9
double SnapMouseX, SnapMouseY, SnapMouseZ, SnapMouseDeltaX, SnapMouseDeltaY;
```

That is roughly 416 bytes per entity, of which about 358 are memcpy'd every frame by
`SInputComponent::SnapshotFrom`. `SInputSystem` is registered at **two** update stages
(`FrameStart` and `PrePhysics`), so the copy happens twice per frame per entity.

None of it is per-entity data. It is the viewport's `FInputContext`, identical for every entity in the
world. With N input entities you pay N copies of one truth, and they can never disagree in a useful way.

### 3. Two input models on one component, with different gates

The component exposes two families that do not agree:

| Query | Reads from | Honours `bReceivingInput`? |
| --- | --- | --- |
| `IsKeyDown` / `GetMouseX` / ... | the per-entity snapshot | yes |
| `IsActionDown` / `GetActionAxis` / ... | the live `FInputContext`, refetched per call | **no** |

`GetActionState` checks `bEnabled`, resolves `FindViewportForWorld(World)`, and reads the context directly.
It never looks at `bReceivingInput`, which is the flag `SInputSystem` computes from
`bGameInputFocused && V == Active`.

`FInputViewportRegistry::BeginFrame` evaluates actions into **every** registered viewport's context, and
`FInputViewportRegistry::OnEvent` routes key events to the focused or active viewport with no
`bGameInputFocused` check. So on a code read, pressing Shift+F1 to hand input back to the editor stops
`IsKeyDown` but leaves `IsActionDown` firing, and typing in the editor drives gameplay actions. Worth
confirming in a PIE session before treating it as a repro, but the code path allows it.

### 4. The component does four unrelated jobs

1. Routing policy (`bEnabled`, `PlayerIndex`, `World`).
2. A raw device snapshot.
3. An action-query facade that ignores that snapshot.
4. A Lua-era string-keyed raw key API.

Only (1) is genuinely per-entity, and (1) is 13 bytes.

### 5. Name lookup on every query

`GetActionState(FName)` performs a hash lookup into `FInputActionMap::Lookup` per call per frame. The C#
binding layer already solved this properly with a cached index plus a settings serial. That solution is in
the dead path.

### 6. `KeyNameToEKey` understands about forty keys

Single letters, digits, `Space`, `Shift` / `LeftShift`, `Ctrl` / `LeftControl`. Everything else silently
returns false forever, so `IsKeyDown("F1")` and `IsKeyDown("Escape")` are permanent no-ops with no
diagnostic. It is also a linear `strcmp` chain per call.

### 7. Missing pieces a game engine is expected to have

- **No gamepad.** `EKeyDevice` is None/Keyboard/Mouse and nothing polls GLFW joysticks.
- **No mapping-context stack.** `bRunsInUI` is a per-action bool, so "while the pause menu is open, only
  menu actions fire" cannot be expressed. It has to be spelled out on every action individually.
- **No consumption or priority.** Two entities bound to the same action both get it; neither can swallow it.
- **No real multiplayer routing.** `PlayerIndex` is declared "Reserved for future split-screen routing;
  currently unused".
- **No negative edge for axes.** An axis binding has no equivalent of `Released`.

---

## Proposed architecture

Four layers, each with one job. Layers 0 and 1 already exist and are kept almost as-is.

```
L0  Devices        raw OS events  ->  FInputContext (per viewport)        EXISTS, keep
L1  Evaluation     FInputActionMap::UpdateContext -> FInputActionState[]  EXISTS, keep
L2  Players        FInputPlayer: context + mapping-context stack          NEW
L3  Delivery       FInputRouter: subscriptions, priority, consumption     NEW
L4  Surface        C++ Input::Bind / C# SInputAction                      C# EXISTS, wire it
```

### L2: the player is the unit of input, not the entity

```cpp
class FInputPlayer
{
    int32                        PlayerIndex;
    FInputViewport*              Viewport;         // owns the FInputContext
    TVector<SInputMappingContext> ContextStack;    // highest priority last
    TVector<FInputActionState>   PreviousStates;   // for edge diffing
};
```

Split-screen, "which viewport gets input", and the editor's Shift plus F1 gate all collapse into one
question: which player owns this viewport, and is that player enabled. One place to gate, so the two
families in shortcoming 3 cannot drift apart again.

### L2: mapping contexts replace `bRunsInUI` and `EInputMode`

```cpp
REFLECT()
struct SInputMappingContext
{
    PROPERTY(Editable) FName        Name;
    PROPERTY(Editable) TVector<FName> Actions;
    PROPERTY(Editable) int32        Priority = 0;
    PROPERTY(Editable) bool         bBlockLower = false;
};

Input::PushContext(PlayerIndex, "Menu");   // menu actions live, gameplay blocked
Input::PopContext(PlayerIndex, "Menu");
```

This is what RmlUi should call when a document takes focus, instead of the current global
`EInputMode::UI` that each action opts out of individually.

### L3: the component becomes a routing tag

```cpp
REFLECT(Component, Category = "Gameplay")
struct RUNTIME_API SInputComponent
{
    GENERATED_BODY()

    PROPERTY(Editable) bool  bEnabled = true;
    PROPERTY(Editable) int32 PlayerIndex = 0;

    // Higher priority receives an action first and may consume it.
    PROPERTY(Editable) int32 Priority = 0;
};
```

Thirteen bytes, trivially copyable, serialises cleanly, and is a sane thing to replicate. No snapshot, no
world back-pointer, no key array, no query methods.

### L3: one dispatch pass, driven by change

`SInputDispatchSystem` at `FrameStart / Highest`, replacing the current `SInputSystem`:

1. For each player, diff this frame's `FInputActionState` array against `PreviousStates`.
2. Build the list of actions with an edge or a non-zero axis. Usually a handful.
3. For each such action, walk its subscriber list in descending priority, invoking each handler and
   stopping if one consumes.

Cost is proportional to the subscribers of actions that actually changed, instead of
`entities * 358 bytes * 2 stages`. A world with two hundred idle input entities costs nothing.

### L4: resolved handles instead of names

```cpp
// Resolved once; carries the settings serial so a rebind re-resolves exactly once.
FInputActionHandle Jump = Input::ResolveAction("Jump");

const FInputActionState& S = Input::GetAction(World, PlayerIndex, Jump);
```

### L4: subscription as the default, polling as the escape hatch

```cpp
// RAII: unbinds on destruction, so a destroyed entity cannot leave a dangling handler.
FInputSubscription Sub = Input::Bind(World, PlayerIndex, Jump, EInputEdge::Pressed,
                                     [this] { DoJump(); });
```

Polling stays available through `Input::GetAction` for the cases where it is genuinely right, such as
reading a movement axis inside a fixed-step update. The difference is that it stops being the only thing
that works.

The C# surface needs no redesign. `SInputAction` and `SInputAxis` already have the right shape; they need
the native poll pass to exist.

---

## What to keep

Worth stating explicitly, because a rewrite would be tempted to touch these and should not:

- `SKey` and its click-to-capture property customization.
- `CInputSettings` authoring plus the searchable action picker.
- `FInputActionMap` evaluation: dead zone rescaling, sensitivity, invert, hold, tap, 2D channels,
  mouse and wheel axis sources.
- `FInputActionState` as a 16-byte POD with a byte-for-byte C# mirror, so a bound action costs zero
  crossings per frame.
- The `SInputAction` / `SInputAxis` C# API shape.
- `InputActionTests.cpp`, which covers the evaluation rules and should keep passing throughout.

---

## Suggested order

Each phase is independently shippable and leaves the tree working.

**Phase 0, bug fixes, small and high value. DONE 2026-08-17.**
Dropped the duplicate `SInputSystem` registration, which was rebuilding the snapshot and re-delivering every
discrete event twice per frame. Gated the action queries on `IsInputActive()` so they agree with the key
queries. Reflected `SInputEvent` (flattened and bool-free, since a ScriptEvent parameter must be blittable)
so C# `OnInput` is emitted at all. Implemented the binding poll end to end, so `SInputAction.Pressed` and
friends fire. Removed eight orphaned declarations in `DotNetHost.h` and the managed callback-mask plumbing
they fed. Gameplay can stop polling now.

**Phase 1, handles. DONE 2026-08-17.**
`FInputActionHandle` (Input/InputAction.h) holds the action NAME plus a cached index and the table serial it
was resolved against, so a steady-state query is an array read and a settings rebuild costs exactly one
re-resolve. `Input::` (Input/InputQuery.h) is the free-function surface over it.

The gate moved with it. `Input::GetReceivingContext(World)` is now the single definition of "is this world
receiving input"; `SInputSystem` derives `bReceiving` from it and `SInputComponent`'s queries call it instead
of each re-deriving the viewport lookup. That is what makes the Phase 0 drift unrepeatable rather than just
fixed. The component's query methods were kept working and now delegate, so nothing had to be deprecated.

**Phase 2, shrink the component. DONE 2026-08-17.**
`SInputComponent` went from 424 bytes to 12: `bEnabled`, `PlayerIndex`, `Priority`. Gone are the 349-entry
key snapshot, the mouse state, the world back-pointer, `SnapshotFrom` / `ResetSnapshot`, `KeyNameToEKey` and
all twenty query methods. `SInputSystem` no longer copies anything; it takes the world from the registry
context, resolves the gate once per frame instead of once per entity, and declares `Read` rather than
`Write`. The two `CWorld` hooks that existed only to populate the back-pointer went with it.

Raw device reads moved to explicit `Input::IsKeyDown` / `IsMouseButtonDown` / `GetMousePosition` /
`GetMouseDelta` / `GetMouseWheel`, which are honest that they read shared per-viewport state. Scripts get a
`World.Input` facade (LuminaSharp/Input/Input.cs) as the poll escape hatch, alongside the declarative
bindings that remain the recommended path.

**Phase 3, mapping contexts. DONE 2026-08-17, simplified.**
`FInputPlayer` was dropped: this engine does not do split-screen, so a player abstraction would have been
ceremony around a single context that `Input::GetReceivingContext` already resolves. `SInputComponent::PlayerIndex`
went with it, leaving the component a single `bEnabled` bool.

What shipped is the part that earns its keep: `SInputMappingContext` (Name, Actions, bBlockLower) authored on
`CInputSettings` beside the actions, and a per-world stack of them on `FInputContext`. The gate walks the
stack top down; the first layer listing the action allows it, and a blocking layer that does not list it
swallows it before anything underneath is consulted. Stack order IS the priority, so no priority field.

`bRunsInUI` was NOT retired. It stays the fallback when no layers are pushed, because removing it would
silently change behaviour for content that relies on it, and `EInputMode` still has a second job gating raw
device reads. A pushed layer takes precedence over it.

**Phase 4, priority and consumption**, then **Phase 5, gamepad** behind the same action model.

Phases 0 through 2 remove the complaint. Phases 3 through 5 are what make it a game engine input system
rather than a working one.
