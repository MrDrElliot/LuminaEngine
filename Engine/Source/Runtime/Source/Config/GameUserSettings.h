#pragma once

#include "Config/Scalability.h"
#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Windows/WindowMode.h"
#include "GameUserSettings.generated.h"

namespace Lumina
{
    // What the player picked in a settings menu, saved per user since a shipped game's own config is read-only.
    REFLECT()
    class RUNTIME_API CGameUserSettings : public CObject
    {
        GENERATED_BODY()

    public:

        PROPERTY()
        EWindowMode WindowMode = EWindowMode::Windowed;

        // Zero means the monitor's own resolution.
        PROPERTY()
        int32 ResolutionWidth = 0;

        PROPERTY()
        int32 ResolutionHeight = 0;

        // Read only in exclusive fullscreen, where zero picks the highest rate the resolution offers.
        PROPERTY()
        int32 RefreshRate = 0;

        PROPERTY()
        bool bVSync = false;

        // Zero is unlimited.
        PROPERTY()
        int32 FrameRateLimit = 0;

        PROPERTY()
        EQualityLevel ViewDistanceQuality = EQualityLevel::Default;

        PROPERTY()
        EQualityLevel ShadowQuality = EQualityLevel::Default;

        PROPERTY()
        EQualityLevel EffectsQuality = EQualityLevel::Default;

        PROPERTY()
        EQualityLevel PostProcessQuality = EQualityLevel::Default;

        PROPERTY()
        EQualityLevel TextureQuality = EQualityLevel::Default;

        PROPERTY()
        EQualityLevel AntiAliasingQuality = EQualityLevel::Default;

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

        void ResetToDefaults();

        EQualityLevel GetQuality(EScalabilityGroup Group) const;
        void SetQuality(EScalabilityGroup Group, EQualityLevel Level);

        // Custom when the groups disagree.
        EQualityLevel GetOverallQuality() const;
        void SetOverallQuality(EQualityLevel Level);

    private:

        void ApplyDisplay();
        void CaptureCurrentDisplay();

        EQualityLevel& QualitySlot(EScalabilityGroup Group);

        bool bLoaded = false;
    };
}
