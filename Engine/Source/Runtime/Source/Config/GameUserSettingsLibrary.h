#pragma once

#include "Config/GameUserSettings.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "GameUserSettingsLibrary.generated.h"

namespace Lumina
{
    // What a settings menu needs. Setters only stage a value, ApplySettings makes them take effect, SaveSettings keeps them.
    REFLECT()
    class RUNTIME_API CGameUserSettingsLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        FUNCTION()
        static EWindowMode GetWindowMode();

        FUNCTION()
        static void SetWindowMode(EWindowMode Mode);

        // Zero means the monitor's own resolution.
        FUNCTION()
        static FIntVector2 GetResolution();

        FUNCTION()
        static void SetResolution(FIntVector2 Resolution);

        // Zero picks the highest rate the resolution offers. Only exclusive fullscreen reads it.
        FUNCTION()
        static int32 GetRefreshRate();

        FUNCTION()
        static void SetRefreshRate(int32 RefreshRate);

        // How many resolution and refresh rate pairs the game's monitor offers.
        FUNCTION()
        static int32 GetDisplayModeCount();

        // Widest first, and a list of structs does not cross to C#, so a menu walks them by index.
        FUNCTION()
        static SDisplayMode GetDisplayMode(int32 Index);

        FUNCTION()
        static bool GetVSync();

        FUNCTION()
        static void SetVSync(bool bEnabled);

        // Zero is unlimited.
        FUNCTION()
        static int32 GetFrameRateLimit();

        FUNCTION()
        static void SetFrameRateLimit(int32 FramesPerSecond);

        FUNCTION()
        static EQualityLevel GetQuality(EScalabilityGroup Group);

        FUNCTION()
        static void SetQuality(EScalabilityGroup Group, EQualityLevel Level);

        // Custom when the groups disagree.
        FUNCTION()
        static EQualityLevel GetOverallQuality();

        // Sets every group, where Default hands each back to the project's own configuration.
        FUNCTION()
        static void SetOverallQuality(EQualityLevel Level);

        FUNCTION()
        static void ApplySettings();

        FUNCTION()
        static void SaveSettings();

        // Discards staged changes by reading the saved file again, then applies it.
        FUNCTION()
        static void RevertSettings();

        // Stages the project defaults; ApplySettings and SaveSettings make it stick.
        FUNCTION()
        static void ResetToDefaults();
    };
}
