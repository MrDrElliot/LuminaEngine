#include "Platform/Time/PlatformTime.h"
#include "World/Entity/PersistentEntities.h"
#include "RuntimePCH.h"
#include <string>
#include "Engine.h"
#include "Core/Object/ObjectReferenceProvider.h"
#include "FrameStats.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Audio/AudioContext.h"
#include "Audio/AudioSettings.h"
#include "Networking/INetworkRuntime.h"
#include "Networking/INetworkTransport.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "Config/Config.h"
#include "Config/EngineSettings.h"
#include "Config/GameUserSettings.h"
#include "Core/Application/Application.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Threading/Thread.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Module/ModuleManager.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Core/Profiler/Profile.h"
#include "Memory/Allocators/Allocator.h"
#if USING(WITH_EDITOR)
#include "TaskSystem/Scheduler/JobProfiler.h"
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
#include "Core/Json/Json.h"
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
#include "Tools/UI/DevUI/GameDevUI.h"
#include "World/WorldManager.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "UI/RmlUiBridge.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

#if USING(WITH_EDITOR)
#include "Tools/UI/ImGui/ImGuiX.h"   // editor toast for a refused project module (NotifyError)
#include "Tools/Dialogs/Dialogs.h"
#include "Log/Log.h"
#endif

namespace Lumina
{
    RUNTIME_API FEngine* GEngine;

    RUNTIME_API bool GIsHeadless = false;

    static FUIntVector2 EngineViewportSize = FUIntVector2(0, 0);


    // Offline rendering steps the game a fixed amount per frame however long each frame takes to draw.
    static TConsoleVar CVarFixedDeltaTime("Core.FixedDeltaTime", 0.0f, "Seconds every frame advances the game by, whatever it took to draw; 0 uses the real frame time");

    static TConsoleVar CVarPaceToDisplay("Core.PaceToDisplay", true, "Under FIFO present, advance each frame by whole display refresh intervals so motion stays even on screen");

    static TConsoleVar CVarTrimAllocatorCaches("Core.TrimAllocatorCaches", true, "Hand the allocator's cached memory back to the OS once a level that replaced another has settled");

    FEngine::FEngine() = default;
    FEngine::~FEngine() = default;
    
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

        FJsonValue Root;
        if (!FJsonValue::Parse(Json.View(), Root))
        {
            return {};
        }

        const FStringView Value = Root["EditorSettings"]["StartupProject"].AsString();
        if (Value.IsEmpty() || Value == "NULL")
        {
            return {};
        }

        return FString(Value);
    }

    // Accepts either the .lproject itself or the directory holding it, since both spellings reach here.
    static FString ResolveProjectDescriptor(FStringView PathOrDirectory)
    {
        if (PathOrDirectory.IsEmpty())
        {
            return FString();
        }

        if (!Filesystem::IsDirectory(PathOrDirectory))
        {
            return FString(PathOrDirectory.GetData(), PathOrDirectory.Len());
        }

        FString Found;
        Filesystem::IterateDirectory(PathOrDirectory, [&Found](const Filesystem::FDirectoryEntry& Entry)
        {
            if (Entry.IsFile() && Entry.GetExtension() == ".lproject")
            {
                Found = FStringView(Entry.FullPath.GetData(), Entry.FullPath.Len());
                return Filesystem::EVisit::Stop;
            }

            return Filesystem::EVisit::Continue;
        });

        if (Found.IsEmpty())
        {
            LOG_WARN("No .lproject file was found in '{}'.", PathOrDirectory);
        }

        return Found;
    }

    // The .lproject's plugin list, read the way LuminaBuildTool reads it, so a plugin the project enables is one that gets built.
    static TVector<FProjectPluginOverride> ParseProjectPluginOverrides(const FJsonValue& Project)
    {
        TVector<FProjectPluginOverride> Overrides;
        const TSpan<const FJsonValue> Entries = Project["Plugins"].AsArray();
        Overrides.Reserve(Entries.Num());
        for (const FJsonValue& Entry : Entries)
        {
            if (!Entry.IsObject())
            {
                continue;
            }
            FProjectPluginOverride Override;
            Override.Name = Entry["Name"].AsString();
            Override.bEnabled = Entry["Enabled"].AsBool(Override.bEnabled);
            if (!Override.Name.IsEmpty())
            {
                Overrides.Emplace(Move(Override));
            }
        }
        return Overrides;
    }

    static void PreloadProjectPluginOverrides(FStringView PathOrDirectory)
    {
        const FString LprojPath = ResolveProjectDescriptor(PathOrDirectory);

        if (LprojPath.IsEmpty())
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

        FJsonValue Root;
        if (!FJsonValue::Parse(Json.View(), Root) || !Root.IsObject())
        {
            return; // Malformed JSON; LoadProject will surface a real error later.
        }

        const TVector<FProjectPluginOverride> Overrides = ParseProjectPluginOverrides(Root);
        if (!Overrides.IsEmpty())
        {
            FPluginManager::Get().ApplyProjectOverrides(Overrides);
        }
    }
    
    #if USING(WITH_DEV_UI)
    IDevelopmentToolUI* FEngine::CreateDevelopmentTools()
    {
        return Memory::New<FGameDevUI>();
    }
    #endif

    void FEngine::StartDevelopmentTools(EInputLayer Layer)
    {
        #if USING(WITH_DEV_UI)
        DeveloperToolUI.Reset(CreateDevelopmentTools());
        DeveloperToolUI->Initialize(UpdateContext);
        GApp->GetEventProcessor().RegisterEventHandler(DeveloperToolUI.Get(), (int32)Layer);
        #endif
    }

    IDevelopmentToolUI* FEngine::GetActiveDevelopmentTools() const
    {
        #if USING(WITH_DEV_UI)
        return DeveloperToolUI.Get();
        #else
        return nullptr;
        #endif
    }

    struct FEngine::FBootTimer
    {
        // Startup is a long serial chain, so -boottimings attributes it without attaching a profiler.
        const bool bEnabled = GCommandLine->Has("boottimings");
        const double Start = PlatformTime::Seconds();
        double Last = Start;

        void Mark(const char* Name)
        {
            const double Now = PlatformTime::Seconds();
            if (bEnabled)
            {
                LOG_DISPLAY("[boot] {} {} ms", Name, (Now - Last) * 1000.0);
            }
            Last = Now;
        }
    };

    struct FEngine::FStartupProject
    {
        FString Path;
        bool bFromCommandLine = false;
        bool bOpen = false;
        bool bMounted = false;
    };

    bool FEngine::Init()
    {
        LUMINA_PROFILE_SCOPE();

        PlatformTime::EnableHighResolutionTiming();

        FBootTimer Boot;

        // Before anything can build an object, so a reinstance always sees every holder.
        if (GConfig != nullptr)
        {
            FObjectReferenceProviders::Register(GConfig);
        }

        FStartupProject Startup = ResolveStartupProject(Boot);
        const bool bStartupSucceeded = MountStartupFilesystems(Startup, Boot);
        InitializeCoreSystems(Boot);
        InitializeRendererAndParallelSubsystems(Boot);
        InitializeReflectionAndWorld(Boot);

        if (Startup.bMounted)
        {
            StartProject();
        }
        Boot.Mark("StartProject");

        InitializeEditorTools(Boot);
        InitializeUserInterface(Boot);
        return FinishBoot(bStartupSucceeded, Boot);
    }

    FEngine::FStartupProject FEngine::ResolveStartupProject(FBootTimer& Boot)
    {
        // Must run before the renderer so Earliest/Core-phase plugins can wedge in ahead.
        FPluginManager::Get().DiscoverEnginePlugins();
        Boot.Mark("DiscoverEnginePlugins");

        // Falls back to the stored startup project, so a bare launch respects plugin enable and disable.
        FStartupProject Startup;
        if (TOptional<FFixedString> ProjectArg = GCommandLine->Get("Project"))
        {
            const FFixedString& V = ProjectArg.GetValue();
            Startup.Path = FStringView(*V, V.Len());
            Startup.bFromCommandLine = true;
        }
        else
        {
            Startup.Path = ReadStartupProjectFromDisk();
        }

        Startup.bOpen = Startup.bFromCommandLine;
        #if WITH_EDITOR
        // Only the editor reopens the stored project on a bare launch, which a packaged game must never do.
        Startup.bOpen |= !Startup.Path.IsEmpty() && Filesystem::Exists(Startup.Path);
        #endif

        // The project's own plugins have to be known before its overrides apply, or each one reads as missing.
        if (Startup.bOpen)
        {
            const FString Descriptor = ResolveProjectDescriptor(Startup.Path);
            if (!Descriptor.IsEmpty())
            {
                FPluginManager::Get().DiscoverProjectPlugins(VFS::Parent(Paths::Normalize(Descriptor)));
            }
        }
        PreloadProjectPluginOverrides(Startup.Path);

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::Earliest);
        Boot.Mark("Phase Earliest");

        return Startup;
    }

    bool FEngine::MountStartupFilesystems(FStartupProject& Startup, FBootTimer& Boot)
    {
        const FString& EngineDir = Paths::GetEngineDirectory();
        if (!EngineDir.IsEmpty() && Filesystem::Exists(EngineDir))
        {
            VFS::Mount<VFS::FNativeFileSystem>("/Engine", EngineDir);
        }
        const FString& InstallDir = Paths::GetEngineInstallDirectory();
        if (!InstallDir.IsEmpty() && Filesystem::Exists(InstallDir))
        {
            const FString IntermediatesDir = InstallDir + "/Intermediates";
            Filesystem::MakeDirectoryTree(IntermediatesDir);
            VFS::Mount<VFS::FNativeFileSystem>("/Intermediates", IntermediatesDir);
        }

        // Before any subsystem starts, so project plugins load in their own phases and boot sees /Game and /Config.
        Startup.bMounted = Startup.bOpen && MountProject(Startup.Path);
        Boot.Mark("MountProject");

        bool bStartupSucceeded = true;
        #if !WITH_EDITOR
        if (Startup.bFromCommandLine && !Startup.bMounted)
        {
            bStartupSucceeded = false;
        }
        #endif
        if (bStartCookedRuntime && !MountCookedRuntime())
        {
            bStartupSucceeded = false;
        }
        return bStartupSucceeded;
    }

    void FEngine::InitializeCoreSystems(FBootTimer& Boot)
    {
        FCoreDelegates::OnPreEngineInit.BroadcastAndClear();
        Boot.Mark("OnPreEngineInit");

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
        Boot.Mark("Console LoadFromConfig");

        Jobs::Initialize();
        Boot.Mark("Jobs::Initialize");

        if (INetworkRuntime* NetRuntime = GetNetworkRuntime())
        {
            NetRuntime->Initialize();
        }
        Boot.Mark("Net Initialize");
        Physics::Initialize();
        Boot.Mark("Physics::Initialize");

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::Core);
        Boot.Mark("Phase Core");
    }

    void FEngine::InitializeRendererAndParallelSubsystems(FBootTimer& Boot)
    {
        // Threads rather than jobs, since a CLR and a COM apartment would stay bound to whichever worker ran the job.
        FThread BasisuThread(FThreadOptions{ "Basisu Init" }, []
        {
            basisu::basisu_encoder_init();
        });

        // Started past this stretch's last module load, whose static initializers race the layout registry.
        FThread DotNetThread(FThreadOptions{ "DotNet Init" }, []
        {
            DotNet::Initialize();
        });

        FThread AudioThread;
        if (!GIsHeadless)
        {
            AudioThread = FThread(FThreadOptions{ "Audio Init" }, []
            {
                Audio::Initialize();
            });
        }

        if (!GIsHeadless)
        {
            Internal::SetRenderManager(Memory::New<FRenderManager>());
            Render().Initialize();
            Boot.Mark("  Render().Initialize");

            // After the renderer since residency goes through the RHI, and before any asset load.
            FTextureStreamingManager::Initialize();
            Boot.Mark("  TextureStreaming");

            EngineViewportSize = Windowing::GetPrimaryWindowHandle()->GetExtent();
        }
        Boot.Mark("Renderer total");

        if (AudioThread.Joinable())
        {
            AudioThread.Join();
        }
        BasisuThread.Join();
        // Script-backed CDOs run in the very next step, so the host has to be up by here.
        DotNetThread.Join();
        Boot.Mark("Join parallel init");
    }

    void FEngine::InitializeReflectionAndWorld(FBootTimer& Boot)
    {
        RegisterPendingTypes();
        Boot.Mark("RegisterPendingTypes");

        // Post-reflection so module initializers do not null-deref, and pre WorldManager for what they spawn.
        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::PreEngineInit);
        RegisterPendingTypes();
        Boot.Mark("Phase PreEngineInit");

        // Built-in primitive meshes must exist before any world deserializes.
        CPrimitiveManager::Get();
        Boot.Mark("CPrimitiveManager");

        if (!GIsHeadless)
        {
            CFontManager::Get();
        }
        Boot.Mark("CFontManager");

        GWorldManager = Memory::New<FWorldManager>();
        Boot.Mark("WorldManager");

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::EngineInit);
        RegisterPendingTypes();
        Boot.Mark("Phase EngineInit");
    }

    void FEngine::InitializeEditorTools([[maybe_unused]] FBootTimer& Boot)
    {
        #if USING(WITH_EDITOR)
        GConfig->DiscoverAndLoadSettings();

        // Applied as soon as the setting is readable, so a crash during startup still carries it.
        if (const CEditorSettings* EditorSettings = GetDefault<CEditorSettings>();
            !EditorSettings->CrashReportContactEmail.IsEmpty())
        {
            CrashReporting::SetUser("", EditorSettings->CrashReportContactEmail);
        }

        StartDevelopmentTools(EInputLayer::EditorChrome);
        Boot.Mark("DeveloperToolUI");

        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::EditorInit);
        RegisterPendingTypes();
        Boot.Mark("Phase EditorInit");
        #endif
    }

    void FEngine::InitializeUserInterface(FBootTimer& Boot)
    {
        if (!GIsHeadless)
        {
            RmlUi::Initialize();
            Boot.Mark("RmlUi::Initialize");
        }

        // The editor built its UI above; a game build has no window to draw an overlay into when headless.
        #if USING(WITH_DEV_UI) && !USING(WITH_EDITOR)
        if (!GIsHeadless)
        {
            StartDevelopmentTools(EInputLayer::DevOverlay);
            Boot.Mark("DeveloperToolUI");
        }
        #endif
    }

    bool FEngine::FinishBoot(bool bStartupSucceeded, FBootTimer& Boot)
    {
        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::PostEngineInit);
        RegisterPendingTypes();

        if (bStartCookedRuntime && bStartupSucceeded && !StartCookedGame())
        {
            bStartupSucceeded = false;
        }

        FCoreDelegates::OnPostEngineInit.BroadcastAndClear();
        Boot.Mark("Phase PostEngineInit");
        if (Boot.bEnabled)
        {
            LOG_DISPLAY("[boot] TOTAL FEngine::Init {} ms", (PlatformTime::Seconds() - Boot.Start) * 1000.0);
        }

        return bStartupSucceeded;
    }

    bool FEngine::Shutdown()
    {
        LUMINA_PROFILE_SCOPE();

        FCoreDelegates::OnEngineShutdownStarted.Broadcast();
        FCoreDelegates::Get().OnPreEngineShutdown.BroadcastAndClear();
        
        Jobs::WaitForSpawned();

        if (!GIsHeadless)
        {
            RHI::WaitDeviceIdle();
            RmlUi::Shutdown();
        }

        #if USING(WITH_DEV_UI)
        if (DeveloperToolUI)
        {
            DeveloperToolUI->Deinitialize(UpdateContext);
            DeveloperToolUI.Reset();
        }
        #endif

        Memory::Delete(GWorldManager);
		GWorldManager = nullptr;
        
        Jobs::WaitForSpawned();

        // After WaitForSpawned since in-flight reads write into its records, and before the CObject shutdown.
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
        Jobs::Shutdown();

        FPluginManager::Get().ShutdownAllPlugins();
        FModuleManager::Get().UnloadAllModules();

        PlatformTime::DisableHighResolutionTiming();

        return false;
    }

    int32 FEngine::GetMaxFrameRate() const
    {
        if (FrameRateCapOverride >= 0)
        {
            return FrameRateCapOverride;
        }
        if (GIsHeadless)
        {
            static const int32 TickRate = GCommandLine->GetInt("tickrate").Get(GetDefault<CProjectSettings>()->HeadlessTickRate);
            return Math::Max(TickRate, 1);
        }
        return UserFrameRateLimit >= 0 ? UserFrameRateLimit : GetDefault<CRendererSettings>()->MaxFPS;
    }

    void FEngine::MarkLoopStart()
    {
        const double Now = PlatformTime::Seconds();
        UpdateContext.MarkFrameStart(Now);
        FrameStats::MarkFrame(Now);
        PaceDeltaToDisplay(Now);

        const float FixedDeltaTime = CVarFixedDeltaTime.GetValue();
        if (FixedDeltaTime > 0.0f)
        {
            UpdateContext.DeltaTime = FixedDeltaTime;
        }
    }

    // FIFO shows each frame on a refresh boundary however unevenly the loop runs, so time advances by whole intervals.
    void FEngine::PaceDeltaToDisplay(double Now)
    {
        if (!CVarPaceToDisplay.GetValue() || GIsHeadless || GetDefault<CRendererSettings>()->PresentMode != EPresentMode::FIFO
            || GetMaxFrameRate() > 0)
        {
            PacedClock = 0.0;
            return;
        }

        if (RefreshCheckTime < 0.0 || Now - RefreshCheckTime > 1.0)
        {
            RefreshCheckTime = Now;
            const FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
            const float Hz = Window != nullptr ? Window->GetRefreshRate() : 0.0f;
            RefreshInterval = Hz > 1.0f ? 1.0 / (double)Hz : 0.0;
        }

        const double Raw = UpdateContext.DeltaTime;
        if (RefreshInterval <= 0.0 || Raw <= 0.0)
        {
            PacedClock = 0.0;
            return;
        }

        // A hitch or a stall resyncs to the wall clock rather than paying its debt out over later frames.
        const double Owed = Now - PacedClock;
        if (PacedClock <= 0.0 || Owed > RefreshInterval * 6.0 || Owed < -RefreshInterval * 2.0)
        {
            PacedClock = Now - Raw;
        }

        const double Intervals = Math::Max(1.0, std::round((Now - PacedClock) / RefreshInterval));
        UpdateContext.DeltaTime = Intervals * RefreshInterval;
        PacedClock += UpdateContext.DeltaTime;
    }

    bool FEngine::Update(bool bApplicationWantsExit)
    {
        // Named outright, since GCC would call it Update and the MCP profiler finds frames by this name.
        LUMINA_PROFILE_SECTION("Lumina::FEngine::Update");

        bEngineReadyToClose = true;
        bCloseRequested = bApplicationWantsExit;

        // The hang watchdog dumps every thread's stack if this stops advancing.
        HangWatchdog::Heartbeat();

        #if !USING(WITH_EDITOR) && !defined(LE_SHIPPING)
        {
            constexpr double TitleRefreshSeconds = 0.5;
            const double Now = PlatformTime::Seconds();
            ++TitleSampleFrames;
            if (TitleSampleStart == 0.0)
            {
                TitleSampleStart = Now;
                TitleSampleFrames = 0;
            }
            else if (Now - TitleSampleStart >= TitleRefreshSeconds)
            {
                RefreshWindowTitle((Now - TitleSampleStart) / TitleSampleFrames);
                TitleSampleStart = Now;
                TitleSampleFrames = 0;
            }
        }
        #endif

        // Quiescent here, since the previous frame's parallel gathers are already joined and consumed.
        ResetThreadFrameAllocators();

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

        // Drained even while minimized, since a tool request or an import queued here would otherwise wait for the window to come back.
        MainThread::ProcessQueue();

        if (GIsHeadless || !Windowing::GetPrimaryWindowHandle()->IsWindowMinimized())
        {
            {
                LUMINA_PROFILE_SECTION_COLORED("FrameStart", tracy::Color::Red);
                UpdateContext.UpdateStage = EUpdateStage::FrameStart;

                
                ProcessPendingOpenLevel();
                ProcessPendingTravel();

                if (CacheTrimCountdown > 0 && --CacheTrimCountdown == 0 && CVarTrimAllocatorCaches.GetValue())
                {
                    Memory::RequestCacheTrim();
                }

                // Unloads assemblies and destroys objects.
                DotNet::ProcessPendingScriptReload();

                if (!GIsHeadless)
                {
                    Render().FrameStart(UpdateContext);

                    // Consumes last frame's reports, so draw commands see the new images rather than race them.
                    if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
                    {
                        Streaming->Update();
                    }
                }

                if (IDevelopmentToolUI* DevTools = GetActiveDevelopmentTools())
                {
                    DevTools->StartFrame(UpdateContext);
                    DevTools->Update(UpdateContext);
                }

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

                if (IDevelopmentToolUI* DevTools = GetActiveDevelopmentTools())
                {
                    DevTools->Update(UpdateContext);
                }

                GWorldManager->UpdateWorlds(UpdateContext);

                OnUpdateStage(UpdateContext);
            }

            {
                LUMINA_PROFILE_SECTION_COLORED("Pre-Physics", tracy::Color::Green);
                UpdateContext.UpdateStage = EUpdateStage::PrePhysics;

                if (IDevelopmentToolUI* DevTools = GetActiveDevelopmentTools())
                {
                    DevTools->Update(UpdateContext);
                }

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

                if (IDevelopmentToolUI* DevTools = GetActiveDevelopmentTools())
                {
                    DevTools->Update(UpdateContext);
                }

                GWorldManager->UpdateWorlds(UpdateContext);

                OnUpdateStage(UpdateContext);
            }

            {
                LUMINA_PROFILE_SECTION_COLORED("Frame-End", tracy::Color::Coral);
                UpdateContext.UpdateStage = EUpdateStage::FrameEnd;

                if (IDevelopmentToolUI* DevTools = GetActiveDevelopmentTools())
                {
                    DevTools->Update(UpdateContext);
                }

                // Final world update stage runs on the main thread.
                GWorldManager->UpdateWorlds(UpdateContext);

                if (IDevelopmentToolUI* DevTools = GetActiveDevelopmentTools())
                {
                    DevTools->EndFrame(UpdateContext);
                }
                
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
        
        FGameplayProfiler::Get().EndFrame();
#if USING(WITH_EDITOR)
        FJobProfiler::Get().EndFrame();
#endif

        UpdateContext.MarkFrameEnd(PlatformTime::Seconds());

        int32 MaxFrameRate = GetMaxFrameRate();
        
        // The editor also slows down when another app has focus, while a game only slows down minimized, since two game windows side by side are a normal way to test.
        bool bInBackground = !GIsHeadless && Windowing::GetPrimaryWindowHandle()->IsWindowMinimized();
        #if USING(WITH_EDITOR)
        bInBackground |= !GIsHeadless && !Platform::IsProcessForeground();
        #endif
        // A capture or benchmark set its own rate on purpose, so it keeps it with the window in the background.
        if (bInBackground && FrameRateCapOverride < 0)
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

            // Paced against a running target, not the frame start, which would bake every scheduler overshoot into the period and stutter.
            FrameTargetTime += TargetFrameTime;

            // Sleep the bulk, leaving margin for OS scheduler overshoot, then spin for precision.
            constexpr double SpinMargin = 0.0005;
            const double Remaining = FrameTargetTime - PlatformTime::Seconds();
            if (Remaining > SpinMargin)
            {
                PlatformTime::SleepPrecise(Remaining - SpinMargin);
            }

            while (PlatformTime::Seconds() < FrameTargetTime)
            {
                PlatformTime::YieldThread();
            }

            // One frame of credit either way: a long frame is made up by the next, and a stall banks no burst of uncapped frames.
            const double Now = PlatformTime::Seconds();
            FrameTargetTime = (FrameTargetTime < Now - TargetFrameTime) ? Now - TargetFrameTime : FrameTargetTime;
            FrameTargetTime = (FrameTargetTime > Now + TargetFrameTime) ? Now + TargetFrameTime : FrameTargetTime;
        }
        else
        {
            // Nothing to make up while uncapped; re-arm from now so re-enabling a cap cannot bank credit.
            FrameTargetTime = PlatformTime::Seconds();
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
            const CProjectSettings* Settings = GetDefault<CProjectSettings>();
            const TVector<TSoftObjectPtr<CWorld>>& Roots = Settings->CookRoots;
            Result.Reserve(Roots.Num() + 1);

            // The game boots this map, so a project that only names its startup map still cooks it.
            const FStringView StartupMap = Settings->GameStartupMap.GetPath();
            if (!StartupMap.IsEmpty())
            {
                Result.Emplace(FString(StartupMap.GetData(), StartupMap.Len()), FName("Main"));
            }

            for (const TSoftObjectPtr<CWorld>& SoftRoot : Roots)
            {
                const FStringView PathView = SoftRoot.GetPath();
                if (PathView.IsEmpty() || PathView == StartupMap) continue;
                FCookRoot Root;
                Root.Asset = FString(PathView.GetData(), PathView.Len());
                Root.Chunk = FName("Main");
                Result.Emplace(Move(Root));
            }

            for (const FString& Folder : Settings->CookFolders)
            {
                if (!Folder.IsEmpty())
                {
                    Result.Emplace(Folder.EndsWith("/") ? Folder : Folder + "/", FName("Main"));
                }
            }
        }

        // Plugin descriptors can specify per-root chunk hints, and only enabled plugins contribute.
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())                  continue;
            for (const FCookRoot& Root : Plugin->GetDescriptor().CookRoots)
            {
                Result.Add(Root);
            }
        }

        return Result;
    }

    void FEngine::LoadProject(FStringView Path)
    {
        if (MountProject(Path))
        {
            StartProject();
        }
    }

    bool FEngine::MountProject(FStringView Path)
    {
        const FString Descriptor = ResolveProjectDescriptor(Path);

        FString JsonData;
        if (Descriptor.IsEmpty() || !FileHelper::LoadFileIntoString(JsonData, Descriptor))
        {
            LOG_ERROR("Could not read a project descriptor at '{}'.", Path);
            return false;
        }

        FJsonValue Data;
        FJsonParseError Error;
        if (!FJsonValue::Parse(JsonData.View(), Data, &Error))
        {
            LOG_ERROR("Failed to parse project file '{}': {}", Path, Error.Message);
            return false;
        }

        if (!Data.IsObject() || !Data.Contains("ProjectID") || !Data.Contains("Name"))
        {
            LOG_ERROR("Project file '{}' is missing required fields (ProjectID/Name).", Path);
            return false;
        }

        const FFixedString NormalizedDescriptor = Paths::Normalize(Descriptor);
        const FFixedString Directory(VFS::Parent(NormalizedDescriptor));
        if (!ClaimProject(Directory))
        {
            return false;
        }

        ProjectFile                     = NormalizedDescriptor;
        ProjectPath                     = Directory;
        ProjectName                     = Data["Name"].AsString();

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
        CrashReporting::AddAttachment(Logging::GetLogFilePath());
        
        VFS::Unmount("/Game");
        VFS::Unmount("/Config");
        VFS::Mount<VFS::FNativeFileSystem>("/Game", GameRootDir);
        VFS::Mount<VFS::FNativeFileSystem>("/Config", ConfigDir);

        GConfig->LoadPath("/Config");

        FPluginManager::Get().DiscoverProjectPlugins(ProjectPath);

        if (const TVector<FProjectPluginOverride> Overrides = ParseProjectPluginOverrides(Data); !Overrides.IsEmpty())
        {
            FPluginManager::Get().ApplyProjectOverrides(Overrides);
        }
        
        LegacyCookRoots.Reset();
        for (const FJsonValue& Root : Data["CookRoots"].AsArray())
        {
            const FJsonValue& Asset = Root.IsObject() ? Root["Asset"] : Root;
            if (Asset.IsString())
            {
                LegacyCookRoots.Emplace(Asset.AsString());
            }
        }

        return true;
    }

#if WITH_EDITOR
    // The editor cannot relink binaries it has loaded, so it closes and the build tool reopens the project once built.
    static bool OfferProjectRebuild(FStringView Name, FStringView ProjectFile, FStringView Reason)
    {
        const FString Script = Paths::GetBuildToolScript();
        if (Script.IsEmpty() || GIsHeadless || GCommandLine->Has("benchmark"))
        {
            return false;
        }

        if (!Dialogs::Confirmation("Rebuild Project", "The code for {} {}.\n\nRebuild it now? The editor closes, builds the project, then opens it again.", Name, Reason))
        {
            return false;
        }

        #if defined(LE_PLATFORM_WINDOWS)
        // The window stays open after a failed build so its errors can be read.
        const FString Executable = "cmd.exe";
        const FString Params = Format("/c \"\"{}\" Open \"{}\" -WaitForEditor || pause\"", Script, ProjectFile);
        #else
        const FString& Executable = Script;
        const FString Params = Format("Open \"{}\" -WaitForEditor", ProjectFile);
        #endif

        if (const int Error = Platform::LaunchProcess(UTF8_TO_TCHAR(*Executable), UTF8_TO_TCHAR(*Params)); Error != 0)
        {
            LOG_ERROR("Could not start the project rebuild (system error {}).", Error);
            return false;
        }

        FApplication::RequestExit();
        return true;
    }
#endif

    void FEngine::StartProject()
    {
        GConfig->DiscoverAndLoadSettings();

        CProjectSettings* ProjectSettings = GetMutableDefault<CProjectSettings>();
        if (ProjectSettings->CookRoots.IsEmpty() && !LegacyCookRoots.IsEmpty())
        {
            TVector<TSoftObjectPtr<CWorld>> Migrated;
            Migrated.Reserve(LegacyCookRoots.Num());
            for (const FString& Root : LegacyCookRoots)
            {
                Migrated.Emplace(FStringView(*Root, Root.Len()));
            }
            LOG_INFO("LoadProject: migrating {} cook root(s) from .lproject to Project settings", Migrated.Num());
            ProjectSettings->CookRoots = Move(Migrated);
            GConfig->SaveSettings(CProjectSettings::StaticClass());
        }
        LegacyCookRoots.Reset();

        if (!LoadProjectModule())
        {
            return;
        }

        // Project DLL is now in; plugin modules that wire up to project types load here.
        FPluginManager::Get().LoadModulesForPhase(EPluginLoadingPhase::PostProjectLoad);
        RegisterPendingTypes();

        // The compiler's Initialize ran before any project, so these roots compile here rather than stalling.
        Shaders::PrecompileNewRoots();

        FAssetRegistry::Get().RunInitialDiscovery();

        // Discovery is async and LoadStartupMap resolves by path, which finds nothing in a registry still filling in.
        Jobs::WaitForSpawned();

        // Must run after GConfig->LoadPath but before any OnReady script body.
        FInputActionMap::Get().RebuildFromSettings();

        // Before the startup map, so its scripts are minted when it loads.
        DotNet::ReloadScripts();

        // Before the startup map builds any bodies, since Box3D reads its tolerances as shapes are made.
        Physics::SetLengthUnitsPerMeter(GetDefault<CProjectSettings>()->PhysicsLengthUnitsPerMeter);

        RefreshWindowTitle();
        ApplyUserSettings();
        LoadStartupMap();

        OnProjectLoaded.Broadcast();
    }

    // False when a rebuild was offered and accepted, which closes the editor instead of finishing the load.
    bool FEngine::LoadProjectModule()
    {
        // The project's module DLL lives in its OWN Binaries, exactly like a template project.
        const FFixedString DLLPath = Paths::Combine(ProjectPath, "Binaries", LUMINA_PLATFORM_NAME,
                                                    Paths::MakeModuleFileName(ProjectName));

        if (Paths::Exists(DLLPath))
        {
            if (FModuleManager::Get().LoadModule(DLLPath) != nullptr)
            {
                RegisterPendingTypes();
            }
            else
            {
                const FString& LoadError = FModuleManager::Get().GetLastLoadError();
                if (!LoadError.IsEmpty())
                {
                    LOG_ERROR("Project module '{}' was not loaded: {}", ProjectName, LoadError);
#if WITH_EDITOR
                    if (OfferProjectRebuild(ProjectName, ProjectFile, Format("does not match this editor ({})", LoadError)))
                    {
                        return false;
                    }
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
#if WITH_EDITOR
        else if (Paths::Exists(Paths::Combine(ProjectPath, "Source", FFixedString(*ProjectName).Append(".Target.cs")))
            && OfferProjectRebuild(ProjectName, ProjectFile, "has not been built yet"))
        {
            return false;
        }
#endif
        return true;
    }

    void FEngine::ApplyUserSettings()
    {
        // The editor owns its window, and a play session applies quality on its own start instead.
        #if !USING(WITH_EDITOR)
        CGameUserSettings::Get().Apply();
        #endif
    }

    void FEngine::LoadBotWorlds(FStringView MapName)
    {
        const int32 Count = Math::Clamp(GCommandLine->GetInt("bots").Get(1), 1, 512);
        FURL Target;
        if (TOptional<FFixedString> ConnectArg = GCommandLine->Get("connect"))
        {
            Target = FURL::Parse(FStringView(*ConnectArg.GetValue(), ConnectArg.GetValue().Len()));
        }
        const FString Host = Target.Host.IsEmpty() ? FString("127.0.0.1") : Target.Host;
        const uint16 Port = Target.Port != 0 ? Target.Port : 7777;

        CWorld* SourceWorld = LoadObjectGraph<CWorld>(MapName);
        if (SourceWorld == nullptr)
        {
            LOG_ERROR("[Net] Bots could not load '{}'.", *FString(MapName.GetData(), MapName.Len()));
            return;
        }

        // Each bot is a whole client world of its own, which is what a separate player's machine would run.
        for (int32 Index = 0; Index < Count; ++Index)
        {
            CWorld* BotWorld = CWorld::DuplicateWorld(SourceWorld);
            if (BotWorld == nullptr)
            {
                continue;
            }
            FWorldContext* Context = GWorldManager->CreateWorldContext(BotWorld, EWorldType::Game, ENetMode::Client, true);
            if (Context == nullptr)
            {
                continue;
            }
            Context->SourceWorld  = SourceWorld;
            Context->MapPath      = FString(MapName.GetData(), MapName.Len());
            Context->NetHost      = Host;
            Context->NetPort      = Port;
            Context->bBot         = true;
        }
        LOG_DISPLAY("[Net] Started {} bot client(s) for {}:{}.", Count, *Host, Port);
    }

    void FEngine::LoadStartupMap()
    {
        FString RawMapName;
        if (TOptional<FFixedString> MapArg = GCommandLine->Get("map"))
        {
            RawMapName = FStringView(*MapArg.GetValue(), MapArg.GetValue().Len());
        }
        if (RawMapName.IsEmpty())
        {
            const FStringView GameMapView = GetDefault<CProjectSettings>()->GameStartupMap.GetPath();
            RawMapName = FStringView(GameMapView.GetData(), GameMapView.Len());
        }
        if (RawMapName.IsEmpty())
        {
            // A folder root names content to ship, not a world to boot.
            for (const FCookRoot& Root : GetCookRoots())
            {
                if (!Root.Asset.EndsWith("/"))
                {
                    RawMapName = Root.Asset;
                    LOG_DISPLAY("No Project.GameStartupMap set; falling back to first cook root '{}'.", *RawMapName);
                    break;
                }
            }
        }

        if (RawMapName.IsEmpty())
        {
            LOG_WARN("No startup map: set Project.GameStartupMap or add at least one Project.CookRoots entry. Runtime has no world to run.");
            return;
        }

        // Tolerate legacy absolute paths from before the path resolver.
        const FFixedString MapName = VFS::ResolveToVirtualPath(RawMapName);
        LOG_DISPLAY("LoadStartupMap: loading '{}' (resolved '{}').", *RawMapName, *MapName);

        if (GCommandLine->Has("bots"))
        {
            LoadBotWorlds(FStringView(*MapName, MapName.Len()));
            return;
        }

        // Wire-level load bots run inside the network runtime and never need a world of their own.
        if (GCommandLine->Has("netbots"))
        {
            LOG_DISPLAY("LoadStartupMap: skipped, this process only runs network load bots.");
            return;
        }

        if (GIsHeadless)
        {
            uint16 Port = 7777;
            if (TOptional<int> PortArg = GCommandLine->GetInt("port"))
            {
                Port = static_cast<uint16>(PortArg.GetValue());
            }
            LOG_DISPLAY("[Net] Starting dedicated server on port {} hosting '{}'.", Port, *MapName);
            HostDedicatedLevel(FStringView(*MapName, MapName.Len()), Port);
            return;
        }

        CWorld* SourceWorld = LoadObjectGraph<CWorld>(FStringView(*MapName, MapName.Len()));
        if (SourceWorld == nullptr)
        {
            LOG_ERROR("Failed to load startup map '{}' (resolved to '{}').", *RawMapName, *MapName);
            return;
        }

        // Duplicate so the cached asset isn't the live world; Travel would tear it down otherwise.
        CWorld* StartupWorld = CWorld::DuplicateWorld(SourceWorld);
        if (StartupWorld == nullptr)
        {
            LOG_ERROR("Failed to duplicate startup map '{}'.", *MapName);
            return;
        }

        FWorldContext* Context = GWorldManager->CreateWorldContext(StartupWorld, EWorldType::Game, ENetMode::Standalone);
        if (Context == nullptr)
        {
            LOG_ERROR("LoadStartupMap: CreateWorldContext returned null; world won't tick.");
            return;
        }
        Context->SourceWorld  = SourceWorld;

        if (FInputViewport* Primary = GApp ? GApp->GetPrimaryViewport() : nullptr)
        {
            Primary->SetWorld(StartupWorld);
        }
        else
        {
            LOG_WARN("LoadStartupMap: no primary viewport, world loaded but nothing will render.");
        }
    }

    void FEngine::Travel(FStringView WorldPath, CWorld* FromWorld)
    {
        // Deferred, since tearing down a world inside its own tick is unsafe, and drained next FrameStart.
        PendingTravelPath = FStringView(WorldPath.GetData(), WorldPath.Len());
        PendingTravelWorld = FromWorld;
        bHasPendingTravel = true;
    }

    void FEngine::RequestExitGame()
    {
        // With no subscriber this is a packaged game, where quitting the game means exiting the process.
        if (FCoreDelegates::Get().OnGameQuitRequested.IsBound())
        {
            FCoreDelegates::Get().OnGameQuitRequested.Broadcast();
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
        PendingTravelPath.Reset();
        CWorld* const FromWorld = PendingTravelWorld.Get();
        PendingTravelWorld.Reset();

        if (RawPath.IsEmpty())
        {
            LOG_ERROR("FEngine::Travel: empty world path.");
            return;
        }

        if (GWorldManager == nullptr)
        {
            LOG_ERROR("FEngine::Travel: WorldManager not initialized.");
            return;
        }

        const FrameStats::FSummary Level = FrameStats::Summarize(3, LevelStartSeconds);
        if (Level.Frames > 0)
        {
            LOG_INFO("Travel to {}: the level before ran {} frames over {:.1f} s, mean {:.2f} ms, p99 {:.2f} ms, max {:.1f} ms, {} over 50 ms, {} MB committed.",
                *RawPath, Level.Frames, Level.WindowSeconds, Level.MeanMs, Level.P99Ms, Level.MaxMs, Level.Over50Ms,
                Platform::GetProcessMemoryUsageMegaBytes());
        }
        LevelStartSeconds = PlatformTime::Seconds();

        const FFixedString MapName = VFS::ResolveToVirtualPath(RawPath);

        // Travel fans the destination world's whole dependency closure across workers.
        CWorld* WorldAsset = LoadObjectGraph<CWorld>(FStringView(*MapName, MapName.Len()));
        if (WorldAsset == nullptr)
        {
            LOG_ERROR("FEngine::Travel: failed to load world '{}' (resolved to '{}').", *RawPath, *MapName);
            return;
        }

        FWorldContext* OldContext = FromWorld != nullptr ? GWorldManager->FindContext(FromWorld) : nullptr;

        // Prefer a PIE Game context so Travel replaces the running world, not the editor proxy world.
        if (OldContext == nullptr)
        {
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
        }
        
        if (OldContext == nullptr)
        {
            CWorld* ColdWorld = CWorld::DuplicateWorld(WorldAsset);
            if (ColdWorld == nullptr)
            {
                LOG_ERROR("FEngine::Travel: DuplicateWorld failed for '{}' (cold-boot).", *MapName);
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
                NewContext->SourceWorld  = WorldAsset;
                NewContext->MapPath      = FString(*MapName);
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
            LOG_ERROR("FEngine::Travel: DuplicateWorld failed for '{}'.", *MapName);
            return;
        }

        FPersistentCarry Carry;
        Carry.Lift(OldWorld);

        GWorldManager->DestroyWorldContext(OldWorld);

        FWorldContext* NewContext = GWorldManager->CreateWorldContext(NewWorld, Type, NetMode);
        if (NewContext != nullptr)
        {
            NewContext->bPIE         = bPIE;
            NewContext->SourceWorld  = WorldAsset; // NewWorld was duplicated from WorldAsset -> that's its source
            NewContext->MapPath      = FString(*MapName);
            NewContext->NetHost      = OldNetHost;
            NewContext->NetPort      = NetPort;
        }

        // Every PIE preview has its own viewport, so only the ones showing the traveling world follow it.
        FInputViewportRegistry::Get().ReplaceWorld(OldWorld, NewWorld);
        if (FInputViewport* Primary = GApp ? GApp->GetPrimaryViewport() : nullptr; Primary != nullptr && OldWorld == nullptr)
        {
            Primary->SetWorld(NewWorld);
        }

        if (NewContext != nullptr)
        {
            Carry.Land(NewWorld);
        }

        // OldWorld memory still alive (only TeardownWorld has run); safe to compare identity, do not inspect state.
        FCoreDelegates::OnWorldTraveled.Broadcast(OldWorld, NewWorld);
    }

    void FEngine::OpenLevel(const FURL& URL, CWorld* FromWorld)
    {
        // Deferred, drained at FrameStart alongside Travel.
        PendingOpenURL   = URL;
        PendingOpenWorld = FromWorld;
        bHasPendingOpen  = true;
    }

    void FEngine::HostLevel(FStringView Map, uint16 Port)
    {
        FURL URL;
        URL.Map = FStringView(Map.GetData(), Map.Len());
        URL.Port    = Port;
        URL.bListen = true;
        OpenLevel(URL);
    }

    void FEngine::HostDedicatedLevel(FStringView Map, uint16 Port)
    {
        FURL URL;
        URL.Map = FStringView(Map.GetData(), Map.Len());
        URL.Port       = Port;
        URL.bListen    = true;
        URL.bDedicated = true;
        OpenLevel(URL);
    }

    void FEngine::ConnectToServer(FStringView Host, uint16 Port)
    {
        FURL URL;
        URL.Host = FStringView(Host.GetData(), Host.Len());
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
        CWorld* const FromWorld = PendingOpenWorld.Get();
        PendingOpenWorld.Reset();

        if (GWorldManager == nullptr)
        {
            LOG_ERROR("FEngine::OpenLevel: WorldManager not initialized.");
            return;
        }

        if (URL.IsClient())
        {
            FWorldContext* Ctx = FromWorld != nullptr ? GWorldManager->FindContext(FromWorld) : nullptr;
            if (Ctx == nullptr)
            {
                Ctx = GWorldManager->GetPrimaryGameContext();
            }
            if (Ctx == nullptr)
            {
                LOG_ERROR("FEngine::ConnectToServer: no game world to connect from; open a level first.");
                return;
            }
            Ctx->NetMode = ENetMode::Client;
            Ctx->NetHost = URL.Host;
            Ctx->NetPort = URL.Port;
            LOG_DISPLAY("[Net] Connecting to {}:{} ...", *URL.Host, URL.Port);
            return;
        }

        // Travels to the map, then stamps the role and port onto the new world's context.
        bPendingHostOverride  = true;
        bPendingHostListen    = URL.bListen;
        bPendingHostDedicated = URL.bDedicated;
        PendingHostPort       = URL.Port;
        Travel(URL.Map, FromWorld);
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
        // A package keeps its paks in the data folder beside the exe.
        const FString& DataDir = Paths::GetGameDataDirectory();

        // One .pak per chunk, sorted for a deterministic mount order.
        TVector<FFixedString> PakPaths;
        Filesystem::IterateDirectory(DataDir, [&PakPaths](const Filesystem::FDirectoryEntry& Entry)
        {
            if (Entry.IsDirectory() || Entry.GetExtension() != FStringView(".pak"))
            {
                return;
            }

            PakPaths.Emplace(Entry.FullPath.GetData(), Entry.FullPath.Len());
        });
        Algo::Sort(PakPaths);

        if (PakPaths.IsEmpty())
        {
            LOG_ERROR("FEngine::LoadCookedRuntime: no .pak file found in '{}'.", *DataDir);
            return false;
        }

        // One archive per file shared across mounts, and the first exposing /Config drives the probe.
        TSharedPtr<FPakArchive> ConfigArchive;
        for (const FFixedString& PakPath : PakPaths)
        {
            TSharedPtr<FPakArchive> Archive = FPakArchive::Open(PakPath);
            if (!Archive)
            {
                LOG_ERROR("FEngine::LoadCookedRuntime: failed to open '{}'.", *PakPath);
                return false;
            }

            const TVector<FString> Aliases = Archive->GetTopLevelAliases();
            for (const FString& Alias : Aliases)
            {
                FFixedString AliasFixed(*Alias, Alias.Len());
                VFS::Mount<VFS::FPakFileSystem>(AliasFixed, Archive);
                LOG_DISPLAY("FEngine::LoadCookedRuntime: mounted PAK '{}' at '{}'", *PakPath, *Alias);
            }

            if (!ConfigArchive && Archive->HasEntry("/Config/GameSettings.json"))
            {
                ConfigArchive = Archive;
            }
        }
        
        const FString LooseGameDir = DataDir + "/Game";
        if (Filesystem::Exists(LooseGameDir))
        {
            VFS::Mount<VFS::FNativeFileSystem>("/Game", LooseGameDir);
            LOG_INFO("FEngine::LoadCookedRuntime: mounted loose overlay at '/Game' -> {}", *LooseGameDir);
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
        const int32 LastSlash = ExeFullPath.FindLastOf("/\\");
        const FString ExeDir = (LastSlash == Constants::kIndexNone)
            ? ExeFullPath
            : ExeFullPath.Mid(0, LastSlash);

        // Prefers the cooked blob to avoid walking every asset, falling back to discovery only if missing.
        bool bLoadedFromBlob = false;
        {
            TVector<uint8> RegistryBlob;
            if (VFS::ReadFile(RegistryBlob, "/Engine/AssetRegistry.bin"))
            {
                FMemoryReader Reader(RegistryBlob);
                if (FAssetRegistry::Get().LoadFromArchive(Reader))
                {
                    LOG_DISPLAY("FEngine::LoadCookedRuntime: loaded pre-baked registry ({} bytes).", RegistryBlob.Num());
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
            if (Jobs::IsRuntimeRunning())
            {
                Jobs::WaitForSpawned();
            }
            LOG_DISPLAY("FEngine::LoadCookedRuntime: asset discovery complete.");
        }

        // Without this a cooked game sees empty project settings and finds no startup map.
        GConfig->DiscoverAndLoadSettings();

        FInputActionMap::Get().RebuildFromSettings();

        // The cooker stashes the project name in config so the DLL beside the exe can be resolved.
        ProjectName = GConfig->Get<FString>("Project.Name");
        if (!ProjectName.IsEmpty())
        {
            const FFixedString DLLPath = Paths::Combine(ExeDir, Paths::MakeModuleFileName(ProjectName));

            // A monolithic game links its module into the executable, and LoadModule finds it registered there by name.
            if (Paths::Exists(DLLPath) || FModuleManager::Get().HasStaticFactory(FName(*ProjectName)))
            {
                if (FModuleManager::Get().LoadModule(DLLPath))
                {
                    RegisterPendingTypes();
                }
            }
            else
            {
                LOG_INFO("FEngine::LoadCookedRuntime: no project DLL at '{}' (ok if project has no C++ module)", *DLLPath);
            }
        }

        // After the project DLL so its types exist, and before the startup map so scripts are registered.
        DotNet::LoadCookedScripts();

        // Before the startup map builds any bodies, since Box3D reads its tolerances as shapes are made.
        Physics::SetLengthUnitsPerMeter(GetDefault<CProjectSettings>()->PhysicsLengthUnitsPerMeter);

        RefreshWindowTitle();
        ApplyUserSettings();
        LoadStartupMap();
        return true;
    }

    void FEngine::RefreshWindowTitle(double AverageFrameSeconds)
    {
        // The editor draws its own title bar, so only a game window carries the game's name.
        #if !USING(WITH_EDITOR)
        FWindow* Window = GIsHeadless ? nullptr : Windowing::GetPrimaryWindowHandle();
        if (Window == nullptr)
        {
            return;
        }

        const CProjectSettings* Settings = GetDefault<CProjectSettings>();
        FString Title = Settings != nullptr && !Settings->GameDisplayName.IsEmpty() ? Settings->GameDisplayName : ProjectName;
        if (Title.IsEmpty())
        {
            Title = "Lumina";
        }

        #if !defined(LE_SHIPPING)
        const RHI::FGPUDeviceInfo Gpu = RHI::GetDeviceInfo();
        Title += Format(" | Lumina {} {} | {} | {}", LUMINA_VERSION, LUMINA_CONFIGURATION_NAME, *Gpu.APIName, *Gpu.Name);
        if (AverageFrameSeconds > 0.0)
        {
            Title += Format(" | {:.0f} fps, {:.2f} ms", 1.0 / AverageFrameSeconds, AverageFrameSeconds * 1000.0);
        }
        #endif

        Window->SetTitle(Title);
        #else
        (void)AverageFrameSeconds;
        #endif
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
