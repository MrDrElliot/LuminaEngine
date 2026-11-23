#include "RuntimePCH.h"
#include "GameUserSettingsLibrary.h"

#include "Core/Windows/Window.h"
#include "Renderer/RenderManager.h"
#include "World/Scene/RenderScene/SceneUpscaler.h"

namespace Lumina
{
    namespace
    {
        constexpr float kMinLookSensitivity = 0.01f;
        constexpr float kMaxLookSensitivity = 20.0f;
    }

    float CGameUserSettingsLibrary::GetMouseSensitivity()
    {
        return CGameUserSettings::Get().Input.MouseSensitivity;
    }

    void CGameUserSettingsLibrary::SetMouseSensitivity(float Sensitivity)
    {
        CGameUserSettings::Get().Input.MouseSensitivity = Math::Clamp(Sensitivity, kMinLookSensitivity, kMaxLookSensitivity);
    }

    float CGameUserSettingsLibrary::GetGamepadLookSensitivity()
    {
        return CGameUserSettings::Get().Input.GamepadLookSensitivity;
    }

    void CGameUserSettingsLibrary::SetGamepadLookSensitivity(float Sensitivity)
    {
        CGameUserSettings::Get().Input.GamepadLookSensitivity = Math::Clamp(Sensitivity, kMinLookSensitivity, kMaxLookSensitivity);
    }

    bool CGameUserSettingsLibrary::GetInvertMouseY()
    {
        return CGameUserSettings::Get().Input.bInvertMouseY;
    }

    void CGameUserSettingsLibrary::SetInvertMouseY(bool bInvert)
    {
        CGameUserSettings::Get().Input.bInvertMouseY = bInvert;
    }

    bool CGameUserSettingsLibrary::GetInvertGamepadLookY()
    {
        return CGameUserSettings::Get().Input.bInvertGamepadLookY;
    }

    void CGameUserSettingsLibrary::SetInvertGamepadLookY(bool bInvert)
    {
        CGameUserSettings::Get().Input.bInvertGamepadLookY = bInvert;
    }

    bool CGameUserSettingsLibrary::GetRawMouseInput()
    {
        return CGameUserSettings::Get().Input.bRawMouseInput;
    }

    void CGameUserSettingsLibrary::SetRawMouseInput(bool bRaw)
    {
        CGameUserSettings::Get().Input.bRawMouseInput = bRaw;
    }

    bool CGameUserSettingsLibrary::IsActionToggled(const FName& Action)
    {
        return Algo::Contains(CGameUserSettings::Get().Input.ToggleActions, Action);
    }

    void CGameUserSettingsLibrary::SetActionToggled(const FName& Action, bool bToggled)
    {
        TVector<FName>& Toggles = CGameUserSettings::Get().Input.ToggleActions;
        Toggles.Remove(Action);
        if (bToggled && !Action.IsNone())
        {
            Toggles.Add(Action);
        }
    }

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
        return Window != nullptr ? Window->GetDisplayModes().Num() : 0;
    }

    SDisplayMode CGameUserSettingsLibrary::GetDisplayMode(int32 Index)
    {
        const FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
        if (Window == nullptr)
        {
            return SDisplayMode{};
        }
        const TVector<SDisplayMode> Modes = Window->GetDisplayModes();
        return Index >= 0 && Index < Modes.Num() ? Modes[Index] : SDisplayMode{};
    }

    bool CGameUserSettingsLibrary::GetVSync()
    {
        return CGameUserSettings::Get().bVSync;
    }

    void CGameUserSettingsLibrary::SetVSync(bool bEnabled)
    {
        CGameUserSettings::Get().bVSync = bEnabled;
    }

    EHDROutput CGameUserSettingsLibrary::GetHDROutput()
    {
        return CGameUserSettings::Get().HDROutput;
    }

    void CGameUserSettingsLibrary::SetHDROutput(EHDROutput Mode)
    {
        CGameUserSettings::Get().HDROutput = Mode;
    }

    float CGameUserSettingsLibrary::GetHDRPaperWhiteNits()
    {
        return CGameUserSettings::Get().HDRPaperWhiteNits;
    }

    void CGameUserSettingsLibrary::SetHDRPaperWhiteNits(float Nits)
    {
        CGameUserSettings::Get().HDRPaperWhiteNits = Math::Max(Nits, 0.0f);
    }

    bool CGameUserSettingsLibrary::IsHDROutputActive()
    {
        const FRenderManager* Manager = TryRender();
        return Manager != nullptr && Manager->GetHDROutput().bActive;
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

    bool CGameUserSettingsLibrary::GetOverrideAntiAliasing()
    {
        return CGameUserSettings::Get().bOverrideAntiAliasing;
    }

    void CGameUserSettingsLibrary::SetOverrideAntiAliasing(bool bOverride)
    {
        CGameUserSettings::Get().bOverrideAntiAliasing = bOverride;
    }

    EAntiAliasingMethod CGameUserSettingsLibrary::GetAntiAliasing()
    {
        return CGameUserSettings::Get().AntiAliasing;
    }

    void CGameUserSettingsLibrary::SetAntiAliasing(EAntiAliasingMethod Method)
    {
        CGameUserSettings& Settings = CGameUserSettings::Get();
        Settings.AntiAliasing = Method;
        Settings.bOverrideAntiAliasing = true;
    }

    ERenderResolution CGameUserSettingsLibrary::GetRenderResolution()
    {
        return CGameUserSettings::Get().RenderResolution;
    }

    void CGameUserSettingsLibrary::SetRenderResolution(ERenderResolution Resolution)
    {
        CGameUserSettings& Settings = CGameUserSettings::Get();
        Settings.RenderResolution = Resolution;
        Settings.bOverrideAntiAliasing = true;
    }

    float CGameUserSettingsLibrary::GetScreenPercentage()
    {
        return CGameUserSettings::Get().ScreenPercentage;
    }

    void CGameUserSettingsLibrary::SetScreenPercentage(float Percent)
    {
        CGameUserSettings& Settings = CGameUserSettings::Get();
        Settings.ScreenPercentage = Math::Clamp(Percent, 25.0f, 100.0f);
        Settings.bOverrideAntiAliasing = true;
    }

    namespace
    {
        TVector<EAntiAliasingMethod> AvailableAntiAliasing()
        {
            TVector<IUpscaler*> Registered;
            FUpscalerRegistry::GetRegistered(Registered);

            // Built into every renderer rather than registered, so these are always on offer.
            TVector<EAntiAliasingMethod> Methods = { EAntiAliasingMethod::None, EAntiAliasingMethod::SMAA, EAntiAliasingMethod::SMAAT2x, EAntiAliasingMethod::TAA };
            for (IUpscaler* Upscaler : Registered)
            {
                if (Upscaler != nullptr && Upscaler->IsSupported())
                {
                    Methods.AddUnique(Upscaler->GetMethod());
                }
            }
            return Methods;
        }
    }

    int32 CGameUserSettingsLibrary::GetAvailableAntiAliasingCount()
    {
        return AvailableAntiAliasing().Num();
    }

    EAntiAliasingMethod CGameUserSettingsLibrary::GetAvailableAntiAliasing(int32 Index)
    {
        const TVector<EAntiAliasingMethod> Methods = AvailableAntiAliasing();
        return Index >= 0 && Index < Methods.Num() ? Methods[Index] : EAntiAliasingMethod::None;
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
