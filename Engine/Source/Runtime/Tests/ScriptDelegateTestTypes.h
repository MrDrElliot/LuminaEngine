#pragma once

#include "Containers/String.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "ScriptDelegateTestTypes.generated.h"

namespace Lumina
{
    REFLECT()
    struct SDelegateTestPayload
    {
        GENERATED_BODY()

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        int32 Count = 0;
    };

    /** A named delegate signature, so a declaration site does not have to spell the arguments. */
    using FOnTestTwoArgs = TScriptDelegate<SDelegateTestPayload, float>;

    typedef TScriptDelegate<SDelegateTestPayload, float, FString> FOnTestThreeArgs;

    REFLECT()
    struct SDelegateTestHost
    {
        GENERATED_BODY()

        PROPERTY()
        TScriptDelegate<> OnNothing;

        PROPERTY()
        TScriptDelegate<SDelegateTestPayload> OnOne;

        // A lone non-blittable argument, which the arity-one binding path used to skip silently.
        PROPERTY()
        TScriptDelegate<FString> OnOneString;

        PROPERTY()
        TScriptDelegate<SDelegateTestPayload, float> OnTwo;

        // A string argument is the case the old single-blittable-payload design could not express.
        PROPERTY()
        TScriptDelegate<SDelegateTestPayload, float, FString> OnThree;

        PROPERTY()
        FOnTestTwoArgs OnAliasedTwo;

        PROPERTY()
        FOnTestThreeArgs OnTypedefThree;
    };
}
