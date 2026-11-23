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

        FUNCTION()
        static EHDROutput GetHDROutput();

        FUNCTION()
        static void SetHDROutput(EHDROutput Mode);

        // Brightness of the UI and SDR white in HDR, where 0 follows the operating system.
        FUNCTION()
        static float GetHDRPaperWhiteNits();

        FUNCTION()
        static void SetHDRPaperWhiteNits(float Nits);

        // Whether the game presents HDR10 right now, so a menu can hide its HDR options on an SDR display.
        FUNCTION()
        static bool IsHDROutputActive();

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

        // Off hands anti-aliasing and render resolution back to the project's renderer settings; every setter below turns it on.
        FUNCTION()
        static bool GetOverrideAntiAliasing();

        FUNCTION()
        static void SetOverrideAntiAliasing(bool bOverride);

        FUNCTION()
        static EAntiAliasingMethod GetAntiAliasing();

        FUNCTION()
        static void SetAntiAliasing(EAntiAliasingMethod Method);

        FUNCTION()
        static ERenderResolution GetRenderResolution();

        FUNCTION()
        static void SetRenderResolution(ERenderResolution Resolution);

        // Read while the render resolution is Custom, from 25 to 100.
        FUNCTION()
        static float GetScreenPercentage();

        FUNCTION()
        static void SetScreenPercentage(float Percent);

        // The methods this machine can run, for a menu to offer, with DLSS listed only where its plugin supports the GPU.
        FUNCTION()
        static int32 GetAvailableAntiAliasingCount();

        FUNCTION()
        static EAntiAliasingMethod GetAvailableAntiAliasing(int32 Index);

        //~ Input, laid over the project's authored actions. Rebinding lives on CInputLibrary since it captures a key.

        // A multiplier on mouse look, from 0.01 to 20.
        FUNCTION()
        static float GetMouseSensitivity();

        FUNCTION()
        static void SetMouseSensitivity(float Sensitivity);

        // A multiplier on right stick look, from 0.01 to 20.
        FUNCTION()
        static float GetGamepadLookSensitivity();

        FUNCTION()
        static void SetGamepadLookSensitivity(float Sensitivity);

        FUNCTION()
        static bool GetInvertMouseY();

        FUNCTION()
        static void SetInvertMouseY(bool bInvert);

        FUNCTION()
        static bool GetInvertGamepadLookY();

        FUNCTION()
        static void SetInvertGamepadLookY(bool bInvert);

        FUNCTION()
        static bool GetRawMouseInput();

        FUNCTION()
        static void SetRawMouseInput(bool bRaw);

        // A toggled digital action flips on each press instead of following the button, such as crouch or aim.
        FUNCTION()
        static bool IsActionToggled(const FName& Action);

        FUNCTION()
        static void SetActionToggled(const FName& Action, bool bToggled);

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
