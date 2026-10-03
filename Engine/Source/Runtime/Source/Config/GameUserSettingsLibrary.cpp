#include "RuntimePCH.h"
#include "GameUserSettingsLibrary.h"

#include "Core/Windows/Window.h"

namespace Lumina
{
    EWindowMode CGameUserSettingsLibrary::GetWindowMode()
    {
        return CGameUserSettings::Get().WindowMode;
    }

    void CGameUserSettingsLibrary::SetWindowMode(EWindowMode Mode)
    {
        CGameUserSettings::Get().WindowMode = Mode;
    }

    FIntVector2 CGameUserSettingsLibrary::GetResolution()
    {
        const CGameUserSettings& Settings = CGameUserSettings::Get();
        return FIntVector2(Settings.ResolutionWidth, Settings.ResolutionHeight);
    }

    void CGameUserSettingsLibrary::SetResolution(FIntVector2 Resolution)
    {
        CGameUserSettings& Settings = CGameUserSettings::Get();
        Settings.ResolutionWidth = Math::Max(Resolution.x, 0);
        Settings.ResolutionHeight = Math::Max(Resolution.y, 0);
    }

    int32 CGameUserSettingsLibrary::GetRefreshRate()
    {
        return CGameUserSettings::Get().RefreshRate;
    }

    void CGameUserSettingsLibrary::SetRefreshRate(int32 RefreshRate)
    {
        CGameUserSettings::Get().RefreshRate = Math::Max(RefreshRate, 0);
    }

    int32 CGameUserSettingsLibrary::GetDisplayModeCount()
    {
        const FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
        return Window != nullptr ? (int32)Window->GetDisplayModes().size() : 0;
    }

    SDisplayMode CGameUserSettingsLibrary::GetDisplayMode(int32 Index)
    {
        const FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
        if (Window == nullptr)
        {
            return SDisplayMode{};
        }
        const TVector<SDisplayMode> Modes = Window->GetDisplayModes();
        return Index >= 0 && Index < (int32)Modes.size() ? Modes[Index] : SDisplayMode{};
    }

    bool CGameUserSettingsLibrary::GetVSync()
    {
        return CGameUserSettings::Get().bVSync;
    }

    void CGameUserSettingsLibrary::SetVSync(bool bEnabled)
    {
        CGameUserSettings::Get().bVSync = bEnabled;
    }

    int32 CGameUserSettingsLibrary::GetFrameRateLimit()
    {
        return CGameUserSettings::Get().FrameRateLimit;
    }

    void CGameUserSettingsLibrary::SetFrameRateLimit(int32 FramesPerSecond)
    {
        CGameUserSettings::Get().FrameRateLimit = Math::Max(FramesPerSecond, 0);
    }

    EQualityLevel CGameUserSettingsLibrary::GetQuality(EScalabilityGroup Group)
    {
        return CGameUserSettings::Get().GetQuality(Group);
    }

    void CGameUserSettingsLibrary::SetQuality(EScalabilityGroup Group, EQualityLevel Level)
    {
        CGameUserSettings::Get().SetQuality(Group, Level);
    }

    EQualityLevel CGameUserSettingsLibrary::GetOverallQuality()
    {
        return CGameUserSettings::Get().GetOverallQuality();
    }

    void CGameUserSettingsLibrary::SetOverallQuality(EQualityLevel Level)
    {
        CGameUserSettings::Get().SetOverallQuality(Level);
    }

    void CGameUserSettingsLibrary::ApplySettings()
    {
        CGameUserSettings::Get().Apply();
    }

    void CGameUserSettingsLibrary::SaveSettings()
    {
        CGameUserSettings::Get().Save();
    }

    void CGameUserSettingsLibrary::RevertSettings()
    {
        CGameUserSettings& Settings = CGameUserSettings::Get();
        Settings.Load();
        Settings.Apply();
    }

    void CGameUserSettingsLibrary::ResetToDefaults()
    {
        CGameUserSettings::Get().ResetToDefaults();
    }
}
