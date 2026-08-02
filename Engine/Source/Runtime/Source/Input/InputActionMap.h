#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Input/InputAction.h"

namespace Lumina
{
    class FInputContext;

    // Runtime evaluator for the project's input actions. Actions are authored on CInputSettings (the
    // Settings panel); this caches them with an O(1) name lookup and answers down/pressed/released/axis
    // queries against a per-viewport FInputContext.
    class FInputActionMap
    {
    public:

        RUNTIME_API static FInputActionMap& Get();

        // Pull the action list from CInputSettings' default object and rebuild the lookup. Called after
        // config load and whenever the Input settings are saved in the editor.
        RUNTIME_API void RebuildFromSettings();

        RUNTIME_API const SInputAction* FindAction(FName Name) const;

        RUNTIME_API bool  IsActionDown    (FName Name, const FInputContext& Context) const;
        RUNTIME_API bool  IsActionPressed (FName Name, const FInputContext& Context) const;
        RUNTIME_API bool  IsActionReleased(FName Name, const FInputContext& Context) const;
        RUNTIME_API float GetActionAxis   (FName Name, const FInputContext& Context) const;

        const TVector<SInputAction>& GetAllActions() const { return Actions; }

        RUNTIME_API static FString   KeyToString(EKey Key);
        RUNTIME_API static FString   MouseButtonToString(EMouseKey Button);
        RUNTIME_API static EKey      KeyFromString(FStringView Token);
        RUNTIME_API static EMouseKey MouseButtonFromString(FStringView Token);
        RUNTIME_API static const TVector<EKey>&      AllSupportedKeys();
        RUNTIME_API static const TVector<EMouseKey>& AllSupportedMouseButtons();

    private:

        bool  PassesUIGate(const SInputAction& Action, const FInputContext& Context) const;
        bool  EvaluateDown(const SInputAction& Action, const FInputContext& Context) const;
        float EvaluateAxis(const SInputAction& Action, const FInputContext& Context) const;

        TVector<SInputAction>  Actions;
        THashMap<FName, int32> Lookup;
    };
}
