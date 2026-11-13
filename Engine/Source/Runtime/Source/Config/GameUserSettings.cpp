#include "RuntimePCH.h"
#include "GameUserSettings.h"

#include "EngineSettings.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"
#include "Core/Windows/Window.h"
#include "Input/InputActionMap.h"
#include "Input/InputViewport.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHI.h"

namespace Lumina
{
    namespace
    {
        // Vsync off keeps whichever uncapped mode the project chose, and falls back to Immediate under a FIFO project.
        EPresentMode PresentModeFor(bool bVSync)
        {
            if (bVSync)
            {
                return EPresentMode::FIFO;
            }
            const EPresentMode Project = GetDefault<CRendererSettings>()->PresentMode;
            return Project != EPresentMode::FIFO ? Project : EPresentMode::Immediate;
        }

        const char* UpscalerName(EUpscaler Upscaler)
        {
            switch (Upscaler)
            {
            case EUpscaler::DLSS: return "DLSS";
            case EUpscaler::FSR:  return "FSR";
            case EUpscaler::None: break;
            }
            return "None";
        }

        const char* UpscalerModeName(EUpscalerMode Mode)
        {
            switch (Mode)
            {
            case EUpscalerMode::NativeAA:         return "NativeAA";
            case EUpscalerMode::Quality:          return "Quality";
            case EUpscalerMode::Balanced:         return "Balanced";
            case EUpscalerMode::Performance:      return "Performance";
            case EUpscalerMode::UltraPerformance: return "UltraPerformance";
            case EUpscalerMode::Custom:           break;
            }
            return "Custom";
        }

        constexpr const char* kUpscalerKey          = "RendererSettings.Upscaler";
        constexpr const char* kUpscalerModeKey      = "RendererSettings.UpscalerMode";
        constexpr const char* kScreenPercentageKey  = "RendererSettings.ScreenPercentage";
    }

    CGameUserSettings& CGameUserSettings::Get()
    {
        CGameUserSettings* Settings = GetMutableDefault<CGameUserSettings>();
        if (!Settings->bLoaded)
        {
            Settings->Load();
        }
        return *Settings;
    }

    FString CGameUserSettings::GetSettingsFilePath()
    {
        if (Global::GEngine != nullptr && !Global::GEngine->GetProjectPath().IsEmpty())
        {
            const FFixedString Path = Paths::Combine(Global::GEngine->GetProjectPath(), "Saved", "UserSettings.json");
            return FString(*Path, Path.Len());
        }

        const FStringView ProjectName = Global::GEngine != nullptr ? Global::GEngine->GetProjectName() : FStringView();
        FString Directory = Paths::GetUserDataDirectory();
        Directory += "/";
        Directory += ProjectName.IsEmpty() ? FString("Lumina") : FString(ProjectName.GetData(), ProjectName.Len());
        return Directory + "/UserSettings.json";
    }

    void CGameUserSettings::Load()
    {
        bLoaded = true;
        ResetToDefaults();

        const FString Path = GetSettingsFilePath();
        FString Text;
        if (!Filesystem::Exists(Path) || !Filesystem::ReadFile(Text, Path))
        {
            CaptureCurrentDisplay();
            return;
        }

        FJsonValue Saved;
        FJsonParseError Error;
        if (!FJsonValue::Parse(Text.View(), Saved, &Error))
        {
            LOG_WARN("User settings at {} could not be read ({}); starting from the defaults.", *Path, Error.Message);
            CaptureCurrentDisplay();
            return;
        }
        FJsonStructuredArchive::LoadStruct(Saved, StaticClass(), this);
    }

    void CGameUserSettings::Save() const
    {
        FJsonValue Saved = FJsonValue::MakeObject();
        FJsonStructuredArchive::SaveStruct(Saved, StaticClass(), const_cast<CGameUserSettings*>(this));

        const FString Path = GetSettingsFilePath();
        const FString Text = Saved.ToString(4);
        if (!Filesystem::MakeParentDirectoryTree(Path)
            || !Filesystem::WriteFile(Path, TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len())))
        {
            LOG_ERROR("User settings could not be written to {}.", *Path);
        }
    }

    void CGameUserSettings::Apply()
    {
        ApplyDisplay();
        ApplyQuality();
        ApplyInput();
    }

    void CGameUserSettings::ApplyInput()
    {
        FInputActionMap::Get().SetPlayerSettings(Input);

        // Raw motion is a window setting, so a captured cursor picks up the change only when the mode is pushed again.
        FInputViewportRegistry::Get().ApplyActiveCursorMode();
    }

    void CGameUserSettings::SetBindingOverride(FName Action, int32 BindingIndex, const SKey& Key)
    {
        TVector<SInputBindingOverride>& Overrides = Input.BindingOverrides;
        Overrides.RemoveAll([&](const SInputBindingOverride& Override)
        {
            return Override.Action == Action && Override.BindingIndex == BindingIndex;
        });
        Input.BindingOverrides.Emplace(Action, BindingIndex, Key);
        ApplyInput();
    }

    void CGameUserSettings::ResetBindingOverrides(FName Action)
    {
        TVector<SInputBindingOverride>& Overrides = Input.BindingOverrides;
        Overrides.RemoveAll([&](const SInputBindingOverride& Override)
        {
            return Action.IsNone() || Override.Action == Action;
        });
        ApplyInput();
    }

    void CGameUserSettings::ApplyQuality()
    {
        for (int32 Index = 0; Index < Scalability::NumGroups; ++Index)
        {
            const EScalabilityGroup Group = static_cast<EScalabilityGroup>(Index);
            Scalability::ApplyGroup(Group, GetQuality(Group));
        }
        ApplyUpscaling();
    }

    void CGameUserSettings::ApplyUpscaling()
    {
        if (!bOverrideUpscaling)
        {
            Scalability::RestoreSetting(kUpscalerKey);
            Scalability::RestoreSetting(kUpscalerModeKey);
            Scalability::RestoreSetting(kScreenPercentageKey);
            return;
        }

        Scalability::OverrideSetting(kUpscalerKey, Lumina::Format("\"{}\"", UpscalerName(Upscaler)));
        Scalability::OverrideSetting(kUpscalerModeKey, Lumina::Format("\"{}\"", UpscalerModeName(UpscalerMode)));
        Scalability::OverrideSetting(kScreenPercentageKey, Lumina::Format("{}", Math::Clamp(ScreenPercentage, 25.0f, 100.0f)));
    }

    void CGameUserSettings::ApplyDisplay()
    {
        #if USING(WITH_EDITOR)
        return;
        #else
        if (Global::GIsHeadless)
        {
            return;
        }

        if (FWindow* Window = Windowing::TryGetPrimaryWindowHandle())
        {
            const FUIntVector2 Resolution((uint32)Math::Max(ResolutionWidth, 0), (uint32)Math::Max(ResolutionHeight, 0));
            Window->SetWindowMode(WindowMode, Resolution, RefreshRate);
        }

        const EPresentMode Wanted = PresentModeFor(bVSync);
        if (RHI::GetPresentMode() != Wanted)
        {
            RHI::SetPresentMode(Wanted);
            if (FRenderManager* Manager = TryRender())
            {
                Manager->RecreatePrimarySwapchain();
            }
        }

        if (Global::GEngine != nullptr)
        {
            Global::GEngine->SetUserFrameRateLimit(Math::Max(FrameRateLimit, 0));
        }
        #endif
    }

    void CGameUserSettings::CaptureCurrentDisplay()
    {
        if (const FWindow* Window = Windowing::TryGetPrimaryWindowHandle())
        {
            WindowMode = Window->GetWindowMode();
        }
        bVSync = RHI::GetPresentMode() == EPresentMode::FIFO;
        FrameRateLimit = GetDefault<CRendererSettings>()->MaxFPS;
    }

    void CGameUserSettings::ResetToDefaults()
    {
        WindowMode = EWindowMode::Windowed;
        ResolutionWidth = 0;
        ResolutionHeight = 0;
        RefreshRate = 0;
        bVSync = GetDefault<CRendererSettings>()->PresentMode == EPresentMode::FIFO;
        FrameRateLimit = GetDefault<CRendererSettings>()->MaxFPS;
        SetOverallQuality(EQualityLevel::Default);
        bOverrideUpscaling = false;
        Upscaler = EUpscaler::None;
        UpscalerMode = EUpscalerMode::Custom;
        ScreenPercentage = 100.0f;
        Input = SPlayerInputSettings();
    }

    EQualityLevel& CGameUserSettings::QualitySlot(EScalabilityGroup Group)
    {
        switch (Group)
        {
        case EScalabilityGroup::ViewDistance: return ViewDistanceQuality;
        case EScalabilityGroup::Shadows:      return ShadowQuality;
        case EScalabilityGroup::Effects:      return EffectsQuality;
        case EScalabilityGroup::PostProcess:  return PostProcessQuality;
        case EScalabilityGroup::Textures:     return TextureQuality;
        case EScalabilityGroup::AntiAliasing: return AntiAliasingQuality;
        }
        return ViewDistanceQuality;
    }

    EQualityLevel CGameUserSettings::GetQuality(EScalabilityGroup Group) const
    {
        return const_cast<CGameUserSettings*>(this)->QualitySlot(Group);
    }

    void CGameUserSettings::SetQuality(EScalabilityGroup Group, EQualityLevel Level)
    {
        if (Level != EQualityLevel::Custom)
        {
            QualitySlot(Group) = Level;
        }
    }

    EQualityLevel CGameUserSettings::GetOverallQuality() const
    {
        const EQualityLevel First = GetQuality(EScalabilityGroup::ViewDistance);
        for (int32 Index = 1; Index < Scalability::NumGroups; ++Index)
        {
            if (GetQuality(static_cast<EScalabilityGroup>(Index)) != First)
            {
                return EQualityLevel::Custom;
            }
        }
        return First;
    }

    void CGameUserSettings::SetOverallQuality(EQualityLevel Level)
    {
        for (int32 Index = 0; Index < Scalability::NumGroups; ++Index)
        {
            SetQuality(static_cast<EScalabilityGroup>(Index), Level);
        }
    }
}
