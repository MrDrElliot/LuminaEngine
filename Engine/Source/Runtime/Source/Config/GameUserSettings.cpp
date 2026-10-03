#include "RuntimePCH.h"
#include "GameUserSettings.h"

#include "EngineSettings.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"
#include "Core/Windows/Window.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHI.h"

using Json = nlohmann::json;

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

        FString UserDataDirectory()
        {
            #if defined(_WIN32)
            return Platform::GetEnvVariable("LOCALAPPDATA");
            #else
            const FString Xdg = Platform::GetEnvVariable("XDG_CONFIG_HOME");
            return !Xdg.empty() ? Xdg : Platform::GetEnvVariable("HOME") + "/.config";
            #endif
        }
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
        if (GEngine != nullptr && !GEngine->GetProjectPath().empty())
        {
            const FFixedString Path = Paths::Combine(GEngine->GetProjectPath(), "Saved", "UserSettings.json");
            return FString(Path.c_str(), Path.size());
        }

        const FStringView ProjectName = GEngine != nullptr ? GEngine->GetProjectName() : FStringView();
        FString Directory = UserDataDirectory();
        Directory += "/";
        Directory += ProjectName.empty() ? FString("Lumina") : FString(ProjectName.data(), ProjectName.size());
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

        try
        {
            Json Saved = Json::parse(Text.c_str());
            FJsonStructuredArchive::LoadStruct(Saved, StaticClass(), this);
        }
        catch (const std::exception& Ex)
        {
            LOG_WARN("User settings at {} could not be read ({}); starting from the defaults.", Path.c_str(), Ex.what());
            CaptureCurrentDisplay();
        }
    }

    void CGameUserSettings::Save() const
    {
        Json Saved = Json::object();
        FJsonStructuredArchive::SaveStruct(Saved, StaticClass(), const_cast<CGameUserSettings*>(this));

        const FString Path = GetSettingsFilePath();
        const std::string Text = Saved.dump(4);
        if (!Filesystem::MakeParentDirectoryTree(Path)
            || !Filesystem::WriteFile(Path, TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.data()), Text.size())))
        {
            LOG_ERROR("User settings could not be written to {}.", Path.c_str());
        }
    }

    void CGameUserSettings::Apply()
    {
        ApplyDisplay();
        ApplyQuality();
    }

    void CGameUserSettings::ApplyQuality()
    {
        for (int32 Index = 0; Index < Scalability::NumGroups; ++Index)
        {
            const EScalabilityGroup Group = static_cast<EScalabilityGroup>(Index);
            Scalability::ApplyGroup(Group, GetQuality(Group));
        }
    }

    void CGameUserSettings::ApplyDisplay()
    {
        #if USING(WITH_EDITOR)
        return;
        #else
        if (GIsHeadless)
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

        if (GEngine != nullptr)
        {
            GEngine->SetUserFrameRateLimit(Math::Max(FrameRateLimit, 0));
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
