#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Input/InputAction.h"

namespace Lumina
{
    class FInputContext;

    // Runtime evaluator for the project's input actions. Actions are authored on CInputSettings (the
    // Settings panel); this caches them with an O(1) name lookup, evaluates every action once per frame
    // into the querying FInputContext, and answers down/pressed/released/axis queries off that cache.
    class FInputActionMap
    {
    public:

        RUNTIME_API static FInputActionMap& Get();

        // Pull the action list from CInputSettings' default object and rebuild the lookup. Called after
        // config load and whenever the Input settings are saved in the editor.
        RUNTIME_API void RebuildFromSettings();

        // Bumped by every rebuild. A cached action index is only valid while this is unchanged, which is
        // what lets the script layer resolve a name to an index once instead of per query.
        RUNTIME_API uint32 GetSerial() const { return Serial; }

        RUNTIME_API const SInputAction* FindAction(FName Name) const;

        // Authored mapping layer by name, or null. Rebuilt alongside the actions.
        RUNTIME_API const SInputMappingContext* FindMappingContext(FName Name) const;

        // Index into GetAllActions() / the context's state array, or INDEX_NONE.
        RUNTIME_API int32 FindActionIndex(FName Name) const;

        // Evaluate every action into Context's state array. Called once per frame per context, before the
        // world update, so every query within the frame sees one consistent snapshot.
        RUNTIME_API void UpdateContext(FInputContext& Context, float DeltaSeconds) const;

        RUNTIME_API const FInputActionState& GetActionState(FName Name, const FInputContext& Context) const;

        // Handle overload: resolves the name only when the action table has been rebuilt since last time, so a steady-state query is an array read.
        RUNTIME_API const FInputActionState& GetActionState(const FInputActionHandle& Handle, const FInputContext& Context) const;

        RUNTIME_API bool  IsActionDown    (FName Name, const FInputContext& Context) const;
        RUNTIME_API bool  IsActionPressed (FName Name, const FInputContext& Context) const;
        RUNTIME_API bool  IsActionReleased(FName Name, const FInputContext& Context) const;
        RUNTIME_API bool  IsActionHeld    (FName Name, const FInputContext& Context) const;
        RUNTIME_API bool  WasActionTapped (FName Name, const FInputContext& Context) const;
        RUNTIME_API float GetActionAxis   (FName Name, const FInputContext& Context) const;
        RUNTIME_API float GetActionAxisY  (FName Name, const FInputContext& Context) const;
        RUNTIME_API float GetActionHeldTime(FName Name, const FInputContext& Context) const;

        const TVector<SInputAction>& GetAllActions() const { return Actions; }

        RUNTIME_API static FString   KeyToString(EKey Key);
        RUNTIME_API static FString   MouseButtonToString(EMouseKey Button);
        RUNTIME_API static EKey      KeyFromString(FStringView Token);
        RUNTIME_API static EMouseKey MouseButtonFromString(FStringView Token);
        RUNTIME_API static const TVector<EKey>&      AllSupportedKeys();
        RUNTIME_API static const TVector<EMouseKey>& AllSupportedMouseButtons();

    private:

        // Whether the context's pushed mapping layers (and, failing those, its input mode) let this action fire.
        bool PassesGate(const SInputAction& Action, const FInputContext& Context) const;

        // Sum this action's bindings into per-channel raw values, then shape them (invert, sensitivity,
        // dead zone). bOutAnyKeyDown reports whether a digital binding is held, which is what a Digital
        // action keys off regardless of the shaped magnitude.
        void EvaluateRaw(const SInputAction& Action, const FInputContext& Context,
            float& OutX, float& OutY, bool& bOutAnyKeyDown) const;

        TVector<SInputAction>  Actions;
        TVector<SInputMappingContext> MappingContexts;
        THashMap<FName, int32> Lookup;
        uint32                 Serial = 0;
    };
}
