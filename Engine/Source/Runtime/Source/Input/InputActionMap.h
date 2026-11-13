#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Input/InputAction.h"

namespace Lumina
{
    class FInputContext;

    // The project's authored actions with the player's overrides applied, evaluated once a frame into every input context.
    class FInputActionMap
    {
    public:

        RUNTIME_API static FInputActionMap& Get();

        // Reloads the actions from CInputSettings, after config load and whenever the Input settings are saved.
        RUNTIME_API void RebuildFromSettings();

        // Applies a player's sensitivity, invert, toggles and rebinds, which CGameUserSettings pushes whenever they change.
        RUNTIME_API void SetPlayerSettings(const SPlayerInputSettings& Settings);
        RUNTIME_API const SPlayerInputSettings& GetPlayerSettings() const { return PlayerSettings; }

        // Bumped whenever the action list may have changed, so a cached action index stays valid exactly while it is unchanged.
        RUNTIME_API uint32 GetSerial() const { return Serial; }

        RUNTIME_API const SInputAction* FindAction(FName Name) const;

        // Index into GetAllActions() and every context's state array, or Constants::kIndexNone.
        RUNTIME_API int32 FindActionIndex(FName Name) const;

        // The handle's index, re-resolved only after a rebuild or a rename.
        RUNTIME_API int32 ResolveIndex(const SInputActionHandle& Handle) const;

        // An action other than IgnoreAction with a key binding equal to Key, for flagging a rebind conflict, or none.
        RUNTIME_API FName FindActionUsingKey(const SKey& Key, FName IgnoreAction) const;

        // Runs before the world update, so every query in the frame reads one consistent snapshot.
        RUNTIME_API void UpdateContext(FInputContext& Context, float DeltaSeconds) const;

        // Zeroed when the handle names no action or the context has not been updated since the last rebuild.
        RUNTIME_API const SInputActionState& GetActionState(const SInputActionHandle& Handle, const FInputContext& Context) const;

        // The effective actions, with the player's rebinds already in their Bindings.
        const TVector<SInputAction>& GetAllActions() const { return Actions; }

    private:

        void Rebuild();

        const SInputMappingContext* FindMappingContext(FName Name) const;

        // Whether the context's pushed layers, or failing those its input mode, let this action fire.
        bool PassesGate(const SInputAction& Action, const FInputContext& Context) const;

        // Sums the bindings per channel and shapes the result. bOutAnyKeyDown is what a Digital action keys off.
        void EvaluateRaw(const SInputAction& Action, const FInputContext& Context,
            float& OutX, float& OutY, bool& bOutAnyKeyDown) const;

        // Gated and evaluated, which is all a chord needs to know about its partner.
        bool IsRawDown(const SInputAction& Action, const FInputContext& Context) const;

        // Every action up the ChordAction chain is raw down; a missing, self or cyclic link never holds.
        bool IsChordHeld(const SInputAction& Action, const FInputContext& Context) const;

        TVector<SInputAction>  Actions;
        TVector<SInputMappingContext> MappingContexts;
        THashMap<FName, int32> Lookup;

        SPlayerInputSettings   PlayerSettings;

        // Parallel to Actions, true where the player chose toggle over hold.
        TVector<bool>          ToggleActions;
        uint32                 Serial = 0;
    };
}
