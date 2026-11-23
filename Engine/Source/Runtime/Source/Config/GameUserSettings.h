#pragma once

#include "Config/EngineSettings.h"
#include "Config/Scalability.h"
#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Windows/WindowMode.h"
#include "Input/InputAction.h"
#include "GameUserSettings.generated.h"

namespace Lumina
{
    // What the player picked in a settings menu, saved per user since a shipped game's own config is read-only.
    REFLECT()
    class RUNTIME_API CGameUserSettings : public CObject
    {
        GENERATED_BODY()

    public:

        PROPERTY(Hidden)
        EWindowMode WindowMode = EWindowMode::Windowed;

        // Zero means the monitor's own resolution.
        PROPERTY(Hidden)
        int32 ResolutionWidth = 0;

        PROPERTY(Hidden)
        int32 ResolutionHeight = 0;

        // Read only in exclusive fullscreen, where zero picks the highest rate the resolution offers.
        PROPERTY(Hidden)
        int32 RefreshRate = 0;

        PROPERTY(Hidden)
        bool bVSync = false;

        // Zero is unlimited.
        PROPERTY(Hidden)
        int32 FrameRateLimit = 0;

        PROPERTY(Hidden)
        EHDROutput HDROutput = EHDROutput::Auto;

        // Zero follows the project, which by default follows the Windows SDR content brightness.
        PROPERTY(Hidden)
        float HDRPaperWhiteNits = 0.0f;

        PROPERTY(Hidden)
        EQualityLevel ViewDistanceQuality = EQualityLevel::Default;

        PROPERTY(Hidden)
        EQualityLevel ShadowQuality = EQualityLevel::Default;

        PROPERTY(Hidden)
        EQualityLevel EffectsQuality = EQualityLevel::Default;

        PROPERTY(Hidden)
        EQualityLevel PostProcessQuality = EQualityLevel::Default;

        PROPERTY(Hidden)
        EQualityLevel TextureQuality = EQualityLevel::Default;

        PROPERTY(Hidden)
        EQualityLevel AntiAliasingQuality = EQualityLevel::Default;

        // Off leaves anti-aliasing and render resolution to the project's renderer settings, which is where a new player starts.
        PROPERTY(Hidden)
        bool bOverrideAntiAliasing = false;

        PROPERTY(Hidden)
        EAntiAliasingMethod AntiAliasing = EAntiAliasingMethod::TAA;

        PROPERTY(Hidden)
        ERenderResolution RenderResolution = ERenderResolution::Native;

        // Read while RenderResolution is Custom, from 25 to 100.
        PROPERTY(Hidden)
        float ScreenPercentage = 100.0f;

        // Sensitivity, invert, toggles and rebinds, laid over the project's authored input actions.
        PROPERTY(Hidden)
        SPlayerInputSettings Input;

        // Loaded on first use, so every caller sees what is on disk without asking.
        static CGameUserSettings& Get();

        // The project's Saved folder while it runs from its project, the user's app data once packaged.
        static FString GetSettingsFilePath();

        // With no saved file, starts from what the game currently runs with, so a menu opens on the truth.
        void Load();
        void Save() const;

        // Window, vsync and frame limit apply only in a game, since the editor owns its own window.
        void Apply();
        void ApplyQuality();

        // Input applies in the editor too, so a play session runs with the player's bindings.
        void ApplyInput();

        // Replaces any earlier rebind of the same slot and applies it at once, since a rebind menu shows the new key straight away.
        void SetBindingOverride(FName Action, int32 BindingIndex, const SKey& Key);

        // Drops the player's rebinds of Action, or of every action when it is none, and applies the authored keys again.
        void ResetBindingOverrides(FName Action);

        void ResetToDefaults();

        EQualityLevel GetQuality(EScalabilityGroup Group) const;
        void SetQuality(EScalabilityGroup Group, EQualityLevel Level);

        // Custom when the groups disagree.
        EQualityLevel GetOverallQuality() const;
        void SetOverallQuality(EQualityLevel Level);

    private:

        void ApplyDisplay();
        void ApplyAntiAliasing();
        void CaptureCurrentDisplay();

        EQualityLevel& QualitySlot(EScalabilityGroup Group);

        bool bLoaded = false;
    };
}
