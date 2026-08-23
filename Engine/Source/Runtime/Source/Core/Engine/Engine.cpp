#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include <string>
#include "Engine.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Audio/AudioContext.h"
#include "Audio/AudioSettings.h"
#include "Networking/INetworkRuntime.h"
#include "Config/Config.h"
#include "Config/EngineSettings.h"
#include "Core/Application/Application.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Module/ModuleManager.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/CPUProfiler.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Core/Profiler/Profile.h"
#include "Memory/Allocators/Allocator.h"
#if USING(WITH_EDITOR)
#include "TaskSystem/Scheduler/JobProfiler.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#endif
#include "Core/Windows/Window.h"
#include "encoder/basisu_enc.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/PakFileSystem.h"
#include "Config/InputSettings.h"
#include "Input/InputActionMap.h"
#include "Input/InputViewport.h"
#include "Pak/PakArchive.h"
#include "Platform/CrashHandler.h"
#include "Platform/CrashReporter.h"
#include "Platform/Process/PlatformProcess.h"
#include "TaskSystem/TaskSystem.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Scripting/ScriptableObject.h"
#include "Input/InputProcessor.h"
#include "nlohmann/json.hpp"
#include "Paths/Paths.h"
#include "Physics/Physics.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Renderer/RenderManager.h"
#include "Renderer/ShaderPaths.h"
#include "Renderer/TextureStreamingManager.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "Tools/FontManager/FontManager.h"
#include "Tools/UI/DevelopmentToolUI.h"
#include "World/WorldManager.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "UI/RmlUiBridge.h"
#include "GameInstance.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

#if USING(WITH_EDITOR)
#include "Tools/UI/ImGui/ImGuiX.h"   // editor toast for a refused project module (NotifyError)
#include "Log/Log.h"
#endif

namespace Lumina
{
    RUNTIME_API FEngine* GEngine;

    RUNTIME_API bool GIsHeadless = false;

    static FUIntVector2 EngineViewportSize = FUIntVector2(0, 0);

    static TConsoleVar CVarMaxFrameRate("Core.MaxFPS", 165, "Changes the maximum frame-rate of your engine");
    
    static FString ReadStartupProjectFromDisk()
    {
        const FString PrefsPath = Paths::GetEngineDirectory() + "/Editor/Config/EditorPreferences.json";

        // LoadFileIntoString logs a missing file as an error, which is what every clean clone looks like.
        if (!Filesystem::Exists(PrefsPath))
        {
            return {};
        }

        FString Json;
        if (!FileHelper::LoadFileIntoString(Json, PrefsPath))
        {
            return {};
        }

        try
        {
            const nlohmann::json Root = nlohmann::json::parse(Json.c_str(), Json.c_str() + Json.size());

            const auto SettingsIt = Root.find("EditorSettings");
            if (SettingsIt == Root.end() || !SettingsIt->is_object())
            {
                return {};
            }

            const auto StartupIt = SettingsIt->find("StartupProject");
            if (StartupIt == SettingsIt->end() || !StartupIt->is_string())
            {
                return {};
            }

            const std::string& Value = StartupIt->get_ref<const std::string&>();
            if (Value.empty() || Value == "NULL")
            {
                return {};
            }

            return FString(Value.c_str(), Value.size());
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    static void PreloadProjectPluginOverrides(FStringView LprojPath)
    {
        if (LprojPath.empty())
        {
            LOG_INFO("[PluginManager] No startup project; project plugin overrides not applied.");
            return;
        }
        if (!Filesystem::Exists(LprojPath))
        {
            LOG_WARN("[PluginManager] Startup project '{}' does not exist; plugin overrides not applied.", LprojPath);
            return;
        }

        FString Json;
        if (!FileHelper::LoadFileIntoString(Json, LprojPath))
        {
            LOG_WARN("[PluginManager] Could not read '{}'; plugin overrides not applied.", LprojPath);
            return;
        }

        nlohmann::json Root;
        try
        {
            Root = nlohmann::json::parse(Json.c_str(), Json.c_str() + Json.size());
        }
        catch (const std::exception&)
        {
            return; // Malformed JSON; LoadProject will surface a real error later.
        }
        if (!Root.is_object())
        {
            return;
        }

        auto It = Root.find("Plugins");
        if (It == Root.end() || !It->is_array() || It->empty())
        {
            return;
        }

        TVector<FProjectPluginOverride> Overrides;
        Overrides.reserve(It->size());
        for (const auto& Entry : *It)
        {
            if (!Entry.is_object())
            {
                continue;
            }
            FProjectPluginOverride Override;
            if (auto NameIt = Entry.find("Name"); NameIt != Entry.end() && NameIt->is_string())
            {
                const std::string& S = NameIt->get_ref<const std::string&>();
                Override.Name.assign(S.c_str(), S.size());
            }
            if (auto EnIt = Entry.find("Enabled"); EnIt != Entry.end() && EnIt->is_boolean())
            {
                Override.bEnabled = EnIt->get<bool>();
            }
            if (!Override.Name.empty())
            {
                Overrides.emplace_back(Move(Override));
            }
        }
        if (!Overrides.empty())
        {
            FPluginManager::Get().ApplyProjectOverrides(Overrides);
        }
    }
    
    bool FEngine::Init()
    {
        LUMINA_PROFILE_SCOPE();

        PlatformTime::EnableHighResolutionTiming();

        // Must run before renderer/Lua so Earliest/Core-phase plugins can wedge in ahead.
        FPluginManager::Get().DiscoverEnginePlugins();

        // Falls back to the stored startup project, so a bare launch respects plugin enable and disable.
        {
            FString PreloadLproj;
            if (TOptional<FFixedString> ProjectArg = GCommandLine->Get("Project"))
            {
                const FFixedString& V = ProjectArg.value();
                PreloadLproj.assign(V.c_str(), V.size());
            }
            else
            {
                PreloadLproj = ReadStartupProjectFromDisk();
            }
            PreloadProjectPluginOverrides(PreloadLproj);
        }

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::Earliest);

        const FString& EngineDir = Paths::GetEngineDirectory();
        if (!EngineDir.empty() && Filesystem::Exists(EngineDir))
        {
            VFS::Mount<VFS::FNativeFileSystem>("/Engine", EngineDir);
        }
        const FString& InstallDir = Paths::GetEngineInstallDirectory();
        if (!InstallDir.empty() && Filesystem::Exists(InstallDir))
        {
            const FString IntermediatesDir = InstallDir + "/Intermediates";
            Filesystem::MakeDirectoryTree(IntermediatesDir);
            VFS::Mount<VFS::FNativeFileSystem>("/Intermediates", IntermediatesDir);
        }
        
        FCoreDelegates::OnPreEngineInit.BroadcastAndClear();

        // An engine-lifetime subscription, so the handle is intentionally not retained.
        (void)FCoreDelegates::OnSettingsSaved.AddLambda([](CClass* Class)
        {
            if (Class == CInputSettings::StaticClass())
            {
                FInputActionMap::Get().RebuildFromSettings();
            }
            else if (Class == CAudioSettings::StaticClass())
            {
                Audio::ApplySettings();
            }
            else if (Class == CRendererSettings::StaticClass())
            {
                GetDefault<CRendererSettings>()->ApplyPresentMode();
            }
        });

        FConsoleRegistry::Get().LoadFromConfig();
        
        basisu::basisu_encoder_init();

        if (!GIsHeadless)
        {
            Audio::Initialize();
        }
        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            NetRuntime->Initialize();
        }
        Task::Initialize();
        Physics::Initialize();

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::Core);

        if (!GIsHeadless)
        {
            Internal::SetRenderManager(Memory::New<FRenderManager>());
            Render().Initialize();

            // After the renderer since residency goes through the RHI, and before any asset load.
            FTextureStreamingManager::Initialize();

            EngineViewportSize = Windowing::GetPrimaryWindowHandle()->GetExtent();
        }

        // C# host; non-fatal if the bundled runtime/bootstrap is absent.
        DotNet::Initialize();

        ProcessNewlyLoadedCObjects();

        // Post-reflection so module initializers do not null-deref, and pre WorldManager for what they spawn.
        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::PreEngineInit);
        ProcessNewlyLoadedCObjects();

        // Built-in primitive meshes must exist before any world deserializes.
        CPrimitiveManager::Get();
        
        if (!GIsHeadless)
        {
            CFontManager::Get();
        }

        GWorldManager = Memory::New<FWorldManager>();

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::EngineInit);
        ProcessNewlyLoadedCObjects();
        
        if (TOptional<FFixedString> ProjectArg = GCommandLine->Get("Project"))
        {
            LoadProject(ProjectArg.value());
        }

        #if USING(WITH_EDITOR)
        GConfig->DiscoverAndLoadSettings();

        // Applied as soon as the setting is readable, so a crash during startup still carries it.
        if (const CEditorSettings* EditorSettings = GetDefault<CEditorSettings>();
            !EditorSettings->CrashReportContactEmail.empty())
        {
            CrashReporting::SetUser("", EditorSettings->CrashReportContactEmail);
        }

        DeveloperToolUI = CreateDevelopmentTools();
        DeveloperToolUI->Initialize(UpdateContext);
        GApp->GetEventProcessor().RegisterEventHandler(DeveloperToolUI, (int32)EInputLayer::EditorChrome);

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::EditorInit);
        ProcessNewlyLoadedCObjects();
        #endif

        if (!GIsHeadless)
        {
            RmlUi::Initialize();
        }

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::PostEngineInit);
        ProcessNewlyLoadedCObjects();

        FCoreDelegates::OnPostEngineInit.BroadcastAndClear();

        return true;
    }

    bool FEngine::Shutdown()
    {
        LUMINA_PROFILE_SCOPE();

        FCoreDelegates::OnPreEngineShutdown.BroadcastAndClear();
        
        Jobs::WaitForAll();

        if (!GIsHeadless)
        {
            RHI::WaitDeviceIdle();
            RmlUi::Shutdown();
        }

        #if USING(WITH_EDITOR)
        DeveloperToolUI->Deinitialize(UpdateContext);
        delete DeveloperToolUI;
        #endif

        DestroyGameInstance();

        Memory::Delete(GWorldManager);
		GWorldManager = nullptr;
        
        Jobs::WaitForAll();

        // After WaitForAll since in-flight reads write into its records, and before the CObject shutdown.
        FTextureStreamingManager::Shutdown();

        ShutdownCObjectSystem();

        DotNet::Shutdown();

        if (FRenderManager* RenderManager = TryRender())
        {
            Internal::SetRenderManager(nullptr);
            Memory::Delete(RenderManager);
        }

        Physics::Shutdown();
        if (!GIsHeadless)
        {
            Audio::Shutdown();
        }
        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            NetRuntime->Shutdown();
        }
        Task::Shutdown();

        FPluginManager::Get().ShutdownAllPlugins();
        FModuleManager::Get().UnloadAllModules();

        PlatformTime::DisableHighResolutionTiming();

        return false;
    }

    bool FEngine::Update(bool bApplicationWantsExit)
    {
        LUMINA_PROFILE_SCOPE();

        bEngineReadyToClose = true;
        bCloseRequested = bApplicationWantsExit;

        UpdateContext.MarkFrameStart(PlatformTime::Seconds());

        // The hang watchdog dumps every thread's stack if this stops advancing.
        HangWatchdog::Heartbeat();

        // Quiescent here, since the previous frame's parallel gathers are already joined and consumed.
        ResetThreadFrameAllocators();

        FCPUProfiler::Get().BeginFrame();
        FGameplayProfiler::Get().BeginFrame();
        #if USING(WITH_EDITOR)
        FJobProfiler::Get().BeginFrame();
        #endif

        if (!GIsHeadless)
        {
            Audio::Update();
        }

        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            NetRuntime->Update();
        }

        if (GIsHeadless || !Windowing::GetPrimaryWindowHandle()->IsWindowMinimized())
        {
            {
                LUMINA_PROFILE_SECTION_COLORED("FrameStart", tracy::Color::Red);
                UpdateContext.UpdateStage = EUpdateStage::FrameStart;

                MainThread::ProcessQueue();
                
                ProcessPendingOpenLevel();
                ProcessPendingTravel();

                if (!GIsHeadless)
                {
                    Render().FrameStart(UpdateContext);

                    // Consumes last frame's reports, so draw commands see the new images rather than race them.
                    if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
                    {
                        Streaming->Update();
                    }
                }

                #if USING(WITH_EDITOR)
                DeveloperToolUI->StartFrame(UpdateContext);
                DeveloperToolUI->Update(UpdateContext);
                #endif

                if (!GIsHeadless)
                {
                    GWorldManager->ReclaimIdleRenderers(UpdateContext.GetFrameStartTime());
                }

                // After the editor UI sets the per-tool intervals and before the seven UpdateWorlds calls read them.
                GWorldManager->BeginFrame(UpdateContext.GetFrameStartTime());

                GWorldManager->UpdateWorlds(UpdateContext);

                OnUpdateStage(UpdateContext);
            }

            {
                LUMINA_PROFILE_SECTION_COLORED("Paused", tracy::Color::Purple);
                UpdateContext.UpdateStage = EUpdateStage::Paused;

                #if USING(WITH_EDITOR)
                DeveloperToolUI->Update(UpdateContext);
                #endif

                GWorldManager->UpdateWorlds(UpdateContext);

                OnUpdateStage(UpdateContext);
            }

            {
                LUMINA_PROFILE_SECTION_COLORED("Pre-Physics", tracy::Color::Green);
                UpdateContext.UpdateStage = EUpdateStage::PrePhysics;

                #if USING(WITH_EDITOR)
                DeveloperToolUI->Update(UpdateContext);
                #endif

                GWorldManager->UpdateWorlds(UpdateContext);

                OnUpdateStage(UpdateContext);
            }

            {
                LUMINA_PROFILE_SECTION_COLORED("During-Physics", tracy::Color::Blue);
                UpdateContext.UpdateStage = EUpdateStage::DuringPhysics;

                #if USING(WITH_EDITOR)
                DeveloperToolUI->Update(UpdateContext);
                #endif

                GWorldManager->UpdateWorlds(UpdateContext);

                OnUpdateStage(UpdateContext);
            }

            {
                // Synchronous, so PostPhysics reads THIS frame's results and contact events fire before it.
                LUMINA_PROFILE_SECTION_COLORED("Physics", tracy::Color::DarkOliveGreen);
                GWorldManager->TickPhysics();
            }

            {
                LUMINA_PROFILE_SECTION_COLORED("Post-Physics", tracy::Color::Yellow);
                UpdateContext.UpdateStage = EUpdateStage::PostPhysics;

                #if USING(WITH_EDITOR)
                DeveloperToolUI->Update(UpdateContext);
                #endif

                GWorldManager->UpdateWorlds(UpdateContext);

                OnUpdateStage(UpdateContext);
            }

            {
                LUMINA_PROFILE_SECTION_COLORED("Frame-End", tracy::Color::Coral);
                UpdateContext.UpdateStage = EUpdateStage::FrameEnd;

                #if USING(WITH_EDITOR)
                DeveloperToolUI->Update(UpdateContext);
                #endif

                // Final world update stage runs on the main thread.
                GWorldManager->UpdateWorlds(UpdateContext);

                #if USING(WITH_EDITOR)
                DeveloperToolUI->EndFrame(UpdateContext);
                #endif
                
                if (!GIsHeadless)
                {
                    RmlUi::TickEditorContexts();
                    GWorldManager->ExtractWorlds();
                    
                    GWorldManager->BeginImmediateLines();

                    Render().FrameEnd();
                }

                DotNet::Tick();

                OnUpdateStage(UpdateContext);
            }
        }
        
        FCPUProfiler::Get().EndFrame();
        FGameplayProfiler::Get().EndFrame();
#if USING(WITH_EDITOR)
        FJobProfiler::Get().EndFrame();
#endif

        UpdateContext.MarkFrameEnd(PlatformTime::Seconds());

        int32 MaxFrameRate = CVarMaxFrameRate.GetValue();
        
        if (!GIsHeadless && Windowing::GetPrimaryWindowHandle()->IsWindowMinimized())
        {
            const int32 BackgroundFrameRate = GetDefault<CEditorSettings>()->MaxBackgroundFPS;

            // An uncapped foreground takes the background rate, and a higher background value is ignored.
            if (BackgroundFrameRate > 0 && (MaxFrameRate <= 0 || BackgroundFrameRate < MaxFrameRate))
            {
                MaxFrameRate = BackgroundFrameRate;
            }
        }

        if (MaxFrameRate > 0)
        {
            LUMINA_PROFILE_SECTION_COLORED("Frame-Rate-Limiter", tracy::Color::Gray);
            const double TargetFrameTime = 1.0 / static_cast<double>(MaxFrameRate);
            const double FrameStartTime  = UpdateContext.GetFrameStartTime();
            const double TargetEndTime   = FrameStartTime + TargetFrameTime;

            // Sleep the bulk, leaving margin for OS scheduler overshoot, then spin for precision.
            constexpr double SpinMargin = 0.001;
            double Remaining = TargetEndTime - PlatformTime::Seconds();
            if (Remaining > SpinMargin)
            {
                PlatformTime::Sleep(Remaining - SpinMargin);
            }

            while (PlatformTime::Seconds() < TargetEndTime)
            {
                PlatformTime::YieldThread();
            }
        }
        
        if (bApplicationWantsExit)
        {
            return !bEngineReadyToClose;
        }
        
        return true;
    }

    void FEngine::OnUpdateStage(const FUpdateContext& Context)
    {
    }

    FUIntVector2 FEngine::GetEngineViewportSize()
    {
        return EngineViewportSize;
    }

    void FEngine::SetEngineViewportSize(const FUIntVector2& InSize)
    {
        EngineViewportSize = InSize;
    }

    TVector<FCookRoot> FEngine::GetCookRoots() const
    {
        TVector<FCookRoot> Result;

        // Each entry is an asset path with an implicit Main chunk, since advanced chunking is plugin-only.
        if (GConfig != nullptr)
        {
            const TVector<TSoftObjectPtr<CWorld>>& Roots = GetDefault<CProjectSettings>()->CookRoots;
            Result.reserve(Roots.size());
            for (const TSoftObjectPtr<CWorld>& SoftRoot : Roots)
            {
                const FStringView PathView = SoftRoot.GetPath();
                if (PathView.empty()) continue;
                FCookRoot Root;
                Root.Asset = FString(PathView.data(), PathView.size());
                Root.Chunk = FName("Main");
                Result.emplace_back(Move(Root));
            }
        }

        // Plugin descriptors can specify per-root chunk hints, and only enabled plugins contribute.
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())                  continue;
            for (const FCookRoot& Root : Plugin->GetDescriptor().CookRoots)
            {
                Result.push_back(Root);
            }
        }

        return Result;
    }

    void FEngine::LoadProject(FStringView Path)
    {
        using Json = nlohmann::json;
        
        FString JsonData;
        if (!FileHelper::LoadFileIntoString(JsonData, Path))
        {
            LOG_ERROR("Invalid project path");
            return;
        }
        
        Json Data;
        try
        {
            Data = Json::parse(JsonData.c_str(), JsonData.c_str() + JsonData.size());
        }
        catch (const std::exception& Ex)
        {
            LOG_ERROR("Failed to parse project file '{}': {}", Path, Ex.what());
            return;
        }

        if (!Data.is_object() || !Data.contains("ProjectID") || !Data.contains("Name"))
        {
            LOG_ERROR("Project file '{}' is missing required fields (ProjectID/Name).", Path);
            return;
        }

        ProjectPath                     .assign(VFS::Parent(Paths::Normalize(Path)));
        ProjectName                     = Data["Name"].get<std::string>().c_str();

        FFixedString ConfigDir          = Paths::Combine(ProjectPath, "Config");
        FFixedString GameRootDir        = Paths::Combine(ProjectPath, "Game");
        FFixedString GameContentDir     = Paths::Combine(GameRootDir, "Content");
        FFixedString GameScriptsDir     = Paths::Combine(GameRootDir, "Scripts");
        FFixedString GameShadersDir     = Paths::Combine(GameRootDir, "Shaders");
        FFixedString BinariesDirectory  = Paths::Combine(ProjectPath, "Binaries");

        Filesystem::MakeDirectoryTree(GameContentDir);
        Filesystem::MakeDirectoryTree(GameScriptsDir);
        // Assets, C# and Slang all live under the one /Game mount, in their own subdirectories.
        Filesystem::MakeDirectoryTree(GameShadersDir);
        
        const FFixedString LogsDir = Paths::Combine(ProjectPath, "Logs");
        Filesystem::MakeDirectoryTree(LogsDir);
        Logging::SetLogFileDirectory(LogsDir);

        const FFixedString CrashDumpsDir = Paths::Combine(ProjectPath, "CrashDumps");
        Filesystem::MakeDirectoryTree(CrashDumpsDir);
        CrashHandler::SetCrashDumpDirectory(CrashDumpsDir);
        
        CrashReporting::SetAttribute("Project", ProjectName);
        CrashReporting::ClearAttachments();
        CrashReporting::AddAttachment(Paths::Combine(LogsDir, "Lumina.log"));
        
        VFS::Unmount("/Game");
        VFS::Unmount("/Config");
        VFS::Mount<VFS::FNativeFileSystem>("/Game", GameRootDir);
        VFS::Mount<VFS::FNativeFileSystem>("/Config", ConfigDir);

        GConfig->LoadPath("/Config");
        
        GConfig->DiscoverAndLoadSettings();
        
        FPluginManager::Get().DiscoverProjectPlugins(ProjectPath);
        
        if (auto It = Data.find("Plugins"); It != Data.end() && It->is_array())
        {
            TVector<FProjectPluginOverride> Overrides;
            Overrides.reserve(It->size());
            for (const auto& Entry : *It)
            {
                if (!Entry.is_object()) continue;
                FProjectPluginOverride Override;
                if (auto NameIt = Entry.find("Name"); NameIt != Entry.end() && NameIt->is_string())
                {
                    const std::string& S = NameIt->get_ref<const std::string&>();
                    Override.Name.assign(S.c_str(), S.size());
                }
                if (auto EnIt = Entry.find("Enabled"); EnIt != Entry.end() && EnIt->is_boolean())
                {
                    Override.bEnabled = EnIt->get<bool>();
                }
                if (!Override.Name.empty())
                {
                    Overrides.emplace_back(Move(Override));
                }
            }
            FPluginManager::Get().ApplyProjectOverrides(Overrides);
        }
        
        if (auto It = Data.find("CookRoots"); It != Data.end() && It->is_array())
        {
            CProjectSettings* ProjectSettings = GetMutableDefault<CProjectSettings>();
            if (ProjectSettings->CookRoots.empty() && !It->empty())
            {
                TVector<TSoftObjectPtr<CWorld>> Migrated;
                Migrated.reserve(It->size());
                for (const auto& R : *It)
                {
                    if (R.is_string())
                    {
                        Migrated.emplace_back(FStringView(R.get<std::string>().c_str()));
                    }
                    else if (R.is_object())
                    {
                        if (auto AIt = R.find("Asset"); AIt != R.end() && AIt->is_string())
                        {
                            Migrated.emplace_back(FStringView(AIt->get<std::string>().c_str()));
                        }
                    }
                }
                if (!Migrated.empty())
                {
                    LOG_INFO("LoadProject: migrating {} cook root(s) from .lproject to Project settings", Migrated.size());
                    ProjectSettings->CookRoots = Move(Migrated);
                    GConfig->SaveSettings(CProjectSettings::StaticClass());
                }
            }
        }
        
        // The project's module DLL lives in its OWN Binaries, exactly like a template project.
        const FFixedString DLLPath = Paths::Combine(ProjectPath, "Binaries", LUMINA_PLATFORM_NAME,
                                                    Paths::MakeModuleFileName(ProjectName));

        if (Paths::Exists(DLLPath))
        {
            if (FModuleManager::Get().LoadModule(DLLPath) != nullptr)
            {
                ProcessNewlyLoadedCObjects();
            }
            else
            {
                const FString& LoadError = FModuleManager::Get().GetLastLoadError();
                if (!LoadError.empty())
                {
                    LOG_ERROR("Project module '{}' was not loaded: {}", ProjectName, LoadError);
#if WITH_EDITOR
                    ImGuiX::Notifications::NotifyError(
                        "Project '{}' code was not loaded: {}. Rebuild the project for this engine (matching configuration/platform).",
                        ProjectName, LoadError);
#endif
                }
                else
                {
                    LOG_INFO("No project module found");
                }
            }
        }

        // Project DLL is now in; plugin modules that wire up to project types load here.
        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::PostProjectLoad);
        ProcessNewlyLoadedCObjects();

        // The compiler's Initialize ran before any project, so these roots compile here rather than stalling.
        Shaders::PrecompileNewRoots();

        FAssetRegistry::Get().RunInitialDiscovery();

        // Discovery is async and LoadStartupMap resolves the map by path, which finds nothing in a
        // registry still filling in.
        GTaskSystem->WaitForAll();

        // Must run after GConfig->LoadPath but before any OnReady script body.
        FInputActionMap::Get().RebuildFromSettings();

        // Compile/load C# scripts before creating the GameInstance.
        DotNet::ReloadScripts();

        // A TSubclassOf naming a C# subclass could not resolve at first config load, since scripts were unminted.
        GConfig->ReloadSettings(CProjectSettings::StaticClass());

        CreateGameInstance();
        LoadStartupMap();

        OnProjectLoaded.Broadcast();
    }

    void FEngine::CreateGameInstance()
    {
        // Gotta tear down any prior instance first.
        DestroyGameInstance();
        
        CClass* InstanceClass = GetDefault<CProjectSettings>()->GameInstanceClass.Get();
        if (InstanceClass == nullptr)
        {
            InstanceClass = CGameInstance::StaticClass();
        }

        GameInstance = Cast<CGameInstance>(NewObject(InstanceClass, nullptr, NAME_None, FGuid::New(), OF_Transient));
        if (GameInstance == nullptr)
        {
            // The class resolved but is not a CGameInstance, and Init must never run through a null cast.
            LOG_WARN("GameInstance class '{}' is not a CGameInstance; using base CGameInstance.", InstanceClass->GetName().ToString().c_str());
            GameInstance = Cast<CGameInstance>(NewObject(CGameInstance::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient));
        }
        GameInstance->Init();

        // DISPLAY so it survives Shipping and proves which class actually backs the instance.
        LOG_DISPLAY("GameInstance created: class '{}'.", GameInstance->GetClass()->GetName().ToString().c_str());
    }

    void FEngine::LoadStartupMap()
    {
        FString RawMapName;
        if (TOptional<FFixedString> MapArg = GCommandLine->Get("map"))
        {
            RawMapName.assign(MapArg.value().c_str(), MapArg.value().size());
        }
        if (RawMapName.empty())
        {
            const FStringView GameMapView = GetDefault<CProjectSettings>()->GameStartupMap.GetPath();
            RawMapName.assign(GameMapView.data(), GameMapView.size());
        }
        if (RawMapName.empty())
        {
            const TVector<FCookRoot> Roots = GetCookRoots();
            if (!Roots.empty())
            {
                RawMapName = Roots[0].Asset;
                LOG_DISPLAY("No Project.GameStartupMap set; falling back to first cook root '{}'.", RawMapName.c_str());
            }
        }

        if (RawMapName.empty())
        {
            LOG_WARN("No startup map: set Project.GameStartupMap or add at least one Project.CookRoots entry. Runtime has no world to run.");
            return;
        }

        // Tolerate legacy absolute paths from before the path resolver.
        const FFixedString MapName = VFS::ResolveToVirtualPath(RawMapName);
        LOG_DISPLAY("LoadStartupMap: loading '{}' (resolved '{}').", RawMapName.c_str(), MapName.c_str());

        if (GIsHeadless)
        {
            uint16 Port = 7777;
            if (TOptional<int> PortArg = GCommandLine->GetInt("port"))
            {
                Port = static_cast<uint16>(PortArg.value());
            }
            LOG_DISPLAY("[Net] Starting dedicated server on port {} hosting '{}'.", Port, MapName.c_str());
            HostDedicatedLevel(FStringView(MapName.c_str(), MapName.size()), Port);
            return;
        }

        CWorld* SourceWorld = LoadObjectGraph<CWorld>(FStringView(MapName.c_str(), MapName.size()));
        if (SourceWorld == nullptr)
        {
            LOG_ERROR("Failed to load startup map '{}' (resolved to '{}').", RawMapName.c_str(), MapName.c_str());
            return;
        }

        // Duplicate so the cached asset isn't the live world; Travel would tear it down otherwise.
        CWorld* StartupWorld = CWorld::DuplicateWorld(SourceWorld);
        if (StartupWorld == nullptr)
        {
            LOG_ERROR("Failed to duplicate startup map '{}'.", MapName.c_str());
            return;
        }

        FWorldContext* Context = GWorldManager->CreateWorldContext(StartupWorld, EWorldType::Game, ENetMode::Standalone);
        if (Context == nullptr)
        {
            LOG_ERROR("LoadStartupMap: CreateWorldContext returned null; world won't tick.");
            return;
        }
        Context->SourceWorld  = SourceWorld;
        Context->GameInstance = GameInstance;

        if (FInputViewport* Primary = GApp ? GApp->GetPrimaryViewport() : nullptr)
        {
            Primary->SetWorld(StartupWorld);
        }
        else
        {
            LOG_WARN("LoadStartupMap: no primary viewport, world loaded but nothing will render.");
        }
    }

    void FEngine::Travel(FStringView WorldPath)
    {
        // Deferred, since tearing down a world inside its own tick is unsafe, and drained next FrameStart.
        PendingTravelPath.assign(WorldPath.data(), WorldPath.size());
        bHasPendingTravel = true;
    }

    void FEngine::RequestExitGame()
    {
        // With no subscriber this is a packaged game, where quitting the game means exiting the process.
        if (FCoreDelegates::OnGameQuitRequested.IsBound())
        {
            FCoreDelegates::OnGameQuitRequested.Broadcast();
            return;
        }
        FApplication::RequestExit();
    }

    void FEngine::ProcessPendingTravel()
    {
        if (!bHasPendingTravel)
        {
            return;
        }
        bHasPendingTravel = false;

        const FString RawPath = Move(PendingTravelPath);
        PendingTravelPath.clear();

        if (RawPath.empty())
        {
            LOG_ERROR("FEngine::Travel: empty world path.");
            return;
        }

        if (GWorldManager == nullptr)
        {
            LOG_ERROR("FEngine::Travel: WorldManager not initialized.");
            return;
        }

        const FFixedString MapName = VFS::ResolveToVirtualPath(RawPath);

        // Travel fans the destination world's whole dependency closure across workers.
        CWorld* WorldAsset = LoadObjectGraph<CWorld>(FStringView(MapName.c_str(), MapName.size()));
        if (WorldAsset == nullptr)
        {
            LOG_ERROR("FEngine::Travel: failed to load world '{}' (resolved to '{}').", RawPath.c_str(), MapName.c_str());
            return;
        }

        // Prefer a PIE Game context so Travel replaces the running world, not the editor proxy world.
        FWorldContext* OldContext = nullptr;
        for (const TUniquePtr<FWorldContext>& Ctx : GWorldManager->GetContexts())
        {
            if (Ctx->Type == EWorldType::Game)
            {
                OldContext = Ctx.get();
                if (Ctx->bPIE)
                {
                    break;
                }
            }
        }
        
        if (OldContext == nullptr)
        {
            CWorld* ColdWorld = CWorld::DuplicateWorld(WorldAsset);
            if (ColdWorld == nullptr)
            {
                LOG_ERROR("FEngine::Travel: DuplicateWorld failed for '{}' (cold-boot).", MapName.c_str());
                return;
            }

            ENetMode ColdNetMode = ENetMode::Standalone;
            if (bPendingHostOverride && bPendingHostListen)
            {
                ColdNetMode = bPendingHostDedicated ? ENetMode::DedicatedServer : ENetMode::ListenServer;
            }
            FWorldContext* NewContext = GWorldManager->CreateWorldContext(ColdWorld, EWorldType::Game, ColdNetMode);
            if (NewContext != nullptr)
            {
                NewContext->GameInstance = GameInstance;
                NewContext->SourceWorld  = WorldAsset;
                NewContext->MapPath      = FString(MapName.c_str());
                if (bPendingHostOverride)
                {
                    NewContext->NetPort = PendingHostPort;
                }
            }

            if (FInputViewport* Primary = GApp ? GApp->GetPrimaryViewport() : nullptr)
            {
                Primary->SetWorld(ColdWorld);
            }

            bPendingHostOverride  = false;
            bPendingHostDedicated = false;
            FCoreDelegates::OnWorldTraveled.Broadcast(nullptr, ColdWorld);
            return;
        }

        const EWorldType                Type           = OldContext->Type;
        ENetMode                        NetMode        = OldContext->NetMode;
        const bool                      bPIE           = OldContext->bPIE;
        const FString                   OldNetHost     = OldContext->NetHost;
        uint16                          NetPort        = OldContext->NetPort;
        CGameInstance* const            SavedInstance  = OldContext->GameInstance != nullptr ? OldContext->GameInstance : GameInstance.Get();
        CWorld* const                   OldWorld       = OldContext->World.Get();

        // A host-level OpenLevel overrides the role/port on the world it travels to.
        if (bPendingHostOverride)
        {
            if (bPendingHostListen)
            {
                NetMode = bPendingHostDedicated ? ENetMode::DedicatedServer : ENetMode::ListenServer;
            }
            else
            {
                NetMode = ENetMode::Standalone;
            }
            NetPort = PendingHostPort;
            bPendingHostOverride  = false;
            bPendingHostDedicated = false;
        }
        
        if (NetMode == ENetMode::Client && OldWorld != nullptr)
        {
            if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
            {
                bHasCarriedConnection = NetRuntime->TakeClientConnection(
                    OldWorld, CarriedTransport, CarriedServerConnection, CarriedLocalPeerId);
            }
        }

        // Always duplicated, since PIE never runs on the cached asset and same-map travel would self-destroy.
        CWorld* NewWorld = CWorld::DuplicateWorld(WorldAsset);
        if (NewWorld == nullptr)
        {
            LOG_ERROR("FEngine::Travel: DuplicateWorld failed for '{}'.", MapName.c_str());
            return;
        }

        GWorldManager->DestroyWorldContext(OldWorld);

        FWorldContext* NewContext = GWorldManager->CreateWorldContext(NewWorld, Type, NetMode);
        if (NewContext != nullptr)
        {
            NewContext->bPIE         = bPIE;
            NewContext->SourceWorld  = WorldAsset; // NewWorld was duplicated from WorldAsset -> that's its source
            NewContext->GameInstance = SavedInstance;
            NewContext->MapPath      = FString(MapName.c_str());
            NewContext->NetHost      = OldNetHost;
            NewContext->NetPort      = NetPort;
        }

        if (FInputViewport* Primary = GApp ? GApp->GetPrimaryViewport() : nullptr)
        {
            Primary->SetWorld(NewWorld);
        }

        // OldWorld memory still alive (only TeardownWorld has run); safe to compare identity, do not inspect state.
        FCoreDelegates::OnWorldTraveled.Broadcast(OldWorld, NewWorld);
    }

    void FEngine::OpenLevel(const FURL& URL)
    {
        // Deferred, drained at FrameStart alongside Travel.
        PendingOpenURL  = URL;
        bHasPendingOpen = true;
    }

    void FEngine::HostLevel(FStringView Map, uint16 Port)
    {
        FURL URL;
        URL.Map.assign(Map.data(), Map.size());
        URL.Port    = Port;
        URL.bListen = true;
        OpenLevel(URL);
    }

    void FEngine::HostDedicatedLevel(FStringView Map, uint16 Port)
    {
        FURL URL;
        URL.Map.assign(Map.data(), Map.size());
        URL.Port       = Port;
        URL.bListen    = true;
        URL.bDedicated = true;
        OpenLevel(URL);
    }

    void FEngine::ConnectToServer(FStringView Host, uint16 Port)
    {
        FURL URL;
        URL.Host.assign(Host.data(), Host.size());
        URL.Port = Port;
        OpenLevel(URL);
    }

    void FEngine::ProcessPendingOpenLevel()
    {
        if (!bHasPendingOpen)
        {
            return;
        }
        bHasPendingOpen = false;

        const FURL URL = Move(PendingOpenURL);
        PendingOpenURL = FURL{};

        if (GWorldManager == nullptr)
        {
            LOG_ERROR("FEngine::OpenLevel: WorldManager not initialized.");
            return;
        }

        if (URL.IsClient())
        {
            FWorldContext* Ctx = GWorldManager->GetPrimaryGameContext();
            if (Ctx == nullptr)
            {
                LOG_ERROR("FEngine::ConnectToServer: no game world to connect from; open a level first.");
                return;
            }
            Ctx->NetMode = ENetMode::Client;
            Ctx->NetHost = URL.Host;
            Ctx->NetPort = URL.Port;
            LOG_DISPLAY("[Net] Connecting to {}:{} ...", URL.Host.c_str(), URL.Port);
            return;
        }

        // Travels to the map, then stamps the role and port onto the new world's context.
        bPendingHostOverride  = true;
        bPendingHostListen    = URL.bListen;
        bPendingHostDedicated = URL.bDedicated;
        PendingHostPort       = URL.Port;
        Travel(URL.Map);
    }

    TUniquePtr<INetworkTransport> FEngine::TakeCarriedConnection(FConnectionHandle& OutConnection, uint32& OutLocalPeerId)
    {
        OutConnection           = CarriedServerConnection;
        OutLocalPeerId          = CarriedLocalPeerId;
        bHasCarriedConnection   = false;
        CarriedServerConnection = FConnectionHandle{};
        CarriedLocalPeerId      = 0;
        return Move(CarriedTransport);
    }

    bool FEngine::LoadCookedRuntime()
    {
        if (!MountCookedRuntime())
        {
            return false;
        }
        return StartCookedGame();
    }

    bool FEngine::MountCookedRuntime()
    {
        // Find the single .pak next to the exe. Platform::BaseDir returns wide on Windows; convert first.
        const FString ExeFullPath = FString(TCHAR_TO_UTF8(Platform::BaseDir()));
        const size_t LastSlash = ExeFullPath.find_last_of("/\\");
        const FString ExeDir = (LastSlash == FString::npos)
            ? ExeFullPath
            : ExeFullPath.substr(0, LastSlash);

        // Collect every .pak next to the exe (one per chunk); sorted for deterministic mount order.
        TVector<FFixedString> PakPaths;
        Filesystem::IterateDirectory(ExeDir, [&PakPaths](const Filesystem::FDirectoryEntry& Entry)
        {
            if (Entry.IsDirectory() || Entry.GetExtension() != FStringView(".pak"))
            {
                return;
            }

            PakPaths.emplace_back(Entry.FullPath.data(), Entry.FullPath.size());
        });
        Algo::Sort(PakPaths.begin(), PakPaths.end());

        if (PakPaths.empty())
        {
            LOG_ERROR("FEngine::LoadCookedRuntime: no .pak file found next to '{}'.", ExeDir.c_str());
            return false;
        }

        // One archive per file shared across mounts, and the first exposing /Config drives the probe.
        TSharedPtr<FPakArchive> ConfigArchive;
        for (const FFixedString& PakPath : PakPaths)
        {
            TSharedPtr<FPakArchive> Archive = FPakArchive::Open(PakPath);
            if (!Archive)
            {
                LOG_ERROR("FEngine::LoadCookedRuntime: failed to open '{}'.", PakPath.c_str());
                return false;
            }

            const TVector<FString> Aliases = Archive->GetTopLevelAliases();
            for (const FString& Alias : Aliases)
            {
                FFixedString AliasFixed(Alias.c_str(), Alias.size());
                VFS::Mount<VFS::FPakFileSystem>(AliasFixed, Archive);
                LOG_DISPLAY("FEngine::LoadCookedRuntime: mounted PAK '{}' at '{}'", PakPath.c_str(), Alias.c_str());
            }

            if (!ConfigArchive && Archive->HasEntry("/Config/GameSettings.json"))
            {
                ConfigArchive = Archive;
            }
        }
        
        const FString LooseGameDir = ExeDir + "/Game";
        if (Filesystem::Exists(LooseGameDir))
        {
            VFS::Mount<VFS::FNativeFileSystem>("/Game", LooseGameDir);
            LOG_INFO("FEngine::LoadCookedRuntime: mounted loose overlay at '/Game' -> {}", LooseGameDir.c_str());
        }

        if (ConfigArchive)
        {
            GConfig->LoadPath("/Config");
            // At this phase the Runtime CObjects are not registered, so a call here would load into nothing.
        }
        else
        {
            LOG_WARN("FEngine::LoadCookedRuntime: no /Config/GameSettings.json in any PAK; using defaults.");
        }

        return true;
    }

    bool FEngine::StartCookedGame()
    {
        // Resolve exe dir again, used for project DLL lookup.
        const FString ExeFullPath = FString(TCHAR_TO_UTF8(Platform::BaseDir()));
        const size_t LastSlash = ExeFullPath.find_last_of("/\\");
        const FString ExeDir = (LastSlash == FString::npos)
            ? ExeFullPath
            : ExeFullPath.substr(0, LastSlash);

        // Prefers the cooked blob to avoid walking every asset, falling back to discovery only if missing.
        bool bLoadedFromBlob = false;
        {
            TVector<uint8> RegistryBlob;
            if (VFS::ReadFile(RegistryBlob, "/Engine/AssetRegistry.bin"))
            {
                FMemoryReader Reader(RegistryBlob);
                if (FAssetRegistry::Get().LoadFromArchive(Reader))
                {
                    LOG_DISPLAY("FEngine::LoadCookedRuntime: loaded pre-baked registry ({} bytes).", RegistryBlob.size());
                    bLoadedFromBlob = true;
                }
                else
                {
                    LOG_WARN("FEngine::LoadCookedRuntime: /Engine/AssetRegistry.bin failed validation; falling back to discovery.");
                }
            }
        }

        if (!bLoadedFromBlob)
        {
            // Discovery is async; MUST wait before LoadStartupMap or GetAssetByPath silently fails on empty registry.
            FAssetRegistry::Get().RunInitialDiscovery();
            if (GTaskSystem != nullptr)
            {
                GTaskSystem->WaitForAll();
            }
            LOG_DISPLAY("FEngine::LoadCookedRuntime: asset discovery complete.");
        }

        // Without this a cooked game sees empty project settings and finds no startup map.
        GConfig->DiscoverAndLoadSettings();

        FInputActionMap::Get().RebuildFromSettings();

        // The cooker stashes the project name in config so the DLL beside the exe can be resolved.
        ProjectName = GConfig->Get<std::string>("Project.Name").c_str();
        if (!ProjectName.empty())
        {
            const FFixedString DLLPath = Paths::Combine(ExeDir, Paths::MakeModuleFileName(ProjectName));
            if (Paths::Exists(DLLPath))
            {
                if (FModuleManager::Get().LoadModule(DLLPath))
                {
                    ProcessNewlyLoadedCObjects();
                }
            }
            else
            {
                LOG_INFO("FEngine::LoadCookedRuntime: no project DLL at '{}' (ok if project has no C++ module)", DLLPath.c_str());
            }
        }

        // After the project DLL so its types exist, and before the startup map so scripts are registered.
        DotNet::LoadCookedScripts();

        // Cooked settings loaded before the scripts minted, so re-resolve now that a C# class exists.
        GConfig->ReloadSettings(CProjectSettings::StaticClass());

        CreateGameInstance();
        LoadStartupMap();
        return true;
    }

    // Mirrors the rule CreateWorldContext uses, so a restore lands on exactly the contexts that had one.
    void FEngine::RepointGameInstanceContexts(CGameInstance* Instance)
    {
        if (GWorldManager == nullptr)
        {
            return;
        }

        for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
        {
            if (Context->Type == EWorldType::Game || Context->Type == EWorldType::Simulation)
            {
                Context->GameInstance = Instance;
            }
        }
    }

    bool FEngine::EvacuateGameInstance(const THashSet<CClass*>& Classes, FName& OutClassName,
        TVector<uint8>& OutBytes)
    {
        CGameInstance* Instance = GameInstance.Get();
        if (Instance == nullptr || Instance->GetClass() == nullptr
            || Classes.find(Instance->GetClass()) == Classes.end())
        {
            return false;
        }

        OutClassName = Instance->GetClass()->GetName();
        {
            FMemoryWriter Writer(OutBytes);
            FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
            Instance->GetClass()->SerializeTaggedProperties(Ar, Instance);
        }

        // Shutdown is skipped for the same reason an evacuated entity script is not detached; it is coming back.
        RepointGameInstanceContexts(nullptr);
        GameInstance = nullptr;
        return true;
    }

    void FEngine::RestoreGameInstance(const FName& ClassName, const TVector<uint8>& Bytes)
    {
        // Resolved through the redirect registry, since an alias is what carries a renamed class across.
        CClass* InstanceClass = FScriptableRegistry::ResolveClass(ClassName);
        if (InstanceClass == nullptr || !InstanceClass->IsChildOf(CGameInstance::StaticClass()))
        {
            LOG_WARN("GameInstance class '{}' did not survive the script reload; falling back to the base class.",
                ClassName.c_str());
            InstanceClass = CGameInstance::StaticClass();
        }

        GameInstance = Cast<CGameInstance>(NewObject(InstanceClass, nullptr, NAME_None, FGuid::New(), OF_Transient));
        if (GameInstance == nullptr)
        {
            LOG_ERROR("GameInstance could not be rebuilt after the script reload.");
            return;
        }

        {
            FMemoryReader Reader(const_cast<TVector<uint8>&>(Bytes));
            FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
            GameInstance->GetClass()->SerializeTaggedProperties(Ar, GameInstance.Get());
        }

        RepointGameInstanceContexts(GameInstance.Get());
    }

    void FEngine::DestroyGameInstance()
    {
        if (GameInstance == nullptr)
        {
            return;
        }

        GameInstance->Shutdown();
        GameInstance = nullptr;
    }

    FFixedString FEngine::GetProjectContentDirectory() const
    {
        if (!HasLoadedProject())
        {
            return {};
        }

        return Paths::Combine(ProjectPath, "Game", "Content");

    }

    FFixedString FEngine::GetProjectScriptsDirectory() const
    {
        if (!HasLoadedProject())
        {
            return {};
        }

        return Paths::Combine(ProjectPath, "Game", "Scripts");
    }

    FFixedString FEngine::GetProjectShadersDirectory() const
    {
        if (!HasLoadedProject())
        {
            return {};
        }

        return Paths::Combine(ProjectPath, "Game", "Shaders");
    }
}
