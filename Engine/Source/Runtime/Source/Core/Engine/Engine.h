#pragma once

#include "Core/UpdateContext.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Engine/EngineURL.h"
#include "Memory/SmartPtr.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Networking/INetworkTransport.h"
#include "Assets/AssetRegistry/CookRoot.h"


namespace Lumina
{
    class FWorldManager;
    class FAssetRegistry;
    class FRenderManager;
    class IImGuiRenderer;
    class IDevelopmentToolUI;
    class FAssetManager;
    class FApplication;
    class FWindow;
    class CGameInstance;
}

namespace Lumina
{
    
    DECLARE_MULTICAST_DELEGATE(FProjectLoadedDelegate);

    
    class FEngine
    {
    public:
        
        FEngine() = default;
        virtual ~FEngine() = default;
        
        FEngine(const FEngine&) = delete;
        FEngine(FEngine&&) = delete;
        FEngine& operator = (const FEngine&) = delete;
        FEngine& operator = (FEngine&&) = delete;
        
        RUNTIME_API virtual bool Init();
        RUNTIME_API virtual bool Shutdown();
        RUNTIME_API bool Update(bool bApplicationWantsExit);
        RUNTIME_API virtual void OnUpdateStage(const FUpdateContext& Context);

        RUNTIME_API static FUIntVector2 GetEngineViewportSize();

        RUNTIME_API void SetEngineViewportSize(const FUIntVector2& InSize);
        
        /** Used to optionally load a project as a DLL from the command line */
        RUNTIME_API virtual void LoadProject(FStringView Path);

        /** Cooked-runtime entry: mounts .pak next to exe, loads config/scripts/DLL, spawns game instance, loads startup map. */
        RUNTIME_API bool LoadCookedRuntime();

        /** Cooked-runtime: mount the .pak + loose overlay so VFS reads work during engine init. */
        RUNTIME_API bool MountCookedRuntime();

        /** Cooked-runtime: post-init half, asset discovery, project DLL, game instance, startup map. */
        RUNTIME_API bool StartCookedGame();

        #if WITH_EDITOR
        RUNTIME_API virtual IDevelopmentToolUI* CreateDevelopmentTools() = 0;
        RUNTIME_API IDevelopmentToolUI* GetDevelopmentToolsUI() const { return DeveloperToolUI; }
        #endif

        // Meta-context accessors live in EngineMetaContext.h to avoid pulling <entt/entt.hpp> into Engine.h.

        RUNTIME_API void SetReadyToClose(bool bReadyToClose) { bEngineReadyToClose = bReadyToClose; }
        
        RUNTIME_API NODISCARD double GetDeltaTime() const { return UpdateContext.DeltaTime; }
        
        RUNTIME_API NODISCARD bool HasLoadedProject() const { return !ProjectName.empty(); }

        RUNTIME_API const FUpdateContext& GetUpdateContext() const { return UpdateContext; }

        RUNTIME_API void SetEngineReadyToClose(bool bReady) { bEngineReadyToClose = bReady; }
        RUNTIME_API NODISCARD bool IsCloseRequested() const { return bCloseRequested; }
        
        RUNTIME_API FProjectLoadedDelegate& GetProjectLoadedDelegate() { return OnProjectLoaded; }
        
        RUNTIME_API NODISCARD FStringView GetProjectName() const { return ProjectName; }
        RUNTIME_API NODISCARD FStringView GetProjectPath() const { return ProjectPath; }
        RUNTIME_API NODISCARD FFixedString GetProjectContentDirectory() const;
        RUNTIME_API NODISCARD FFixedString GetProjectScriptsDirectory() const;
        RUNTIME_API NODISCARD FFixedString GetProjectShadersDirectory() const;

        // Union of project + enabled-plugin cook roots; legacy GameStartupMap auto-converts
        // to a single root when no explicit roots exist. Cooker iterates this for BFS seeds.
        RUNTIME_API TVector<FCookRoot> GetCookRoots() const;

        RUNTIME_API CGameInstance* GetGameInstance() const { return GameInstance; }

        /** Queues world travel; swap runs at next FrameStart. Prefers PIE Game world; preserves editor proxy on PIE exit. */
        RUNTIME_API void Travel(FStringView WorldPath);

        /** Gameplay quit: ends the PIE session in the editor (via FCoreDelegates::OnGameQuitRequested),
         *  exits the process in a packaged game. Safe to call from inside a world tick. */
        RUNTIME_API void RequestExitGame();
        
        /** The proper entry point: host a level (Map [+ bListen]), open standalone, or connect to URL.Host. */
        RUNTIME_API void OpenLevel(const FURL& URL);

        /** Convenience: open Map as a listen server on Port. */
        RUNTIME_API void HostLevel(FStringView Map, uint16 Port = 7777);

        /** Convenience: open Map as a dedicated (clientless, non-rendered) server on Port. */
        RUNTIME_API void HostDedicatedLevel(FStringView Map, uint16 Port = 7777);

        /** Convenience: connect to Host:Port as a client. The server tells us which level to load (Welcome). */
        RUNTIME_API void ConnectToServer(FStringView Host, uint16 Port = 7777);

        //~ Client connection carried across a Welcome-driven travel (so we don't disconnect+reconnect). The
        //  travel moves the live transport out of the old world; the new world's net system adopts it.
        RUNTIME_API bool HasCarriedConnection() const { return bHasCarriedConnection; }
        RUNTIME_API TUniquePtr<INetworkTransport> TakeCarriedConnection(FConnectionHandle& OutConnection, uint32& OutLocalPeerId);

    protected:

        /** Constructs Project.GameInstanceClass (or base CGameInstance) and calls Init. */
        RUNTIME_API virtual void CreateGameInstance();

        /** Loads Project.GameStartupMap as a Game world. Editor overrides to no-op. */
        RUNTIME_API virtual void LoadStartupMap();

        RUNTIME_API virtual void DestroyGameInstance();

        /** Drains a queued Travel request; called at FrameStart. */
        RUNTIME_API void ProcessPendingTravel();

        /** Drains a queued OpenLevel/Host/Connect request; called at FrameStart (before ProcessPendingTravel). */
        RUNTIME_API void ProcessPendingOpenLevel();

    protected:

        FUpdateContext          UpdateContext;

        FString                 PendingTravelPath;
        bool                    bHasPendingTravel = false;

        // Deferred OpenLevel/Host/Connect, drained at FrameStart.
        FURL                    PendingOpenURL;
        bool                    bHasPendingOpen = false;

        // Host-level OpenLevel travels to a map, then applies this role/port to the new world's context.
        bool                    bPendingHostOverride  = false;
        bool                    bPendingHostListen    = false;
        bool                    bPendingHostDedicated = false;
        uint16                  PendingHostPort       = 7777;

        // Set while ProcessPendingTravel runs a Welcome-driven client travel: the new world's net system
        // adopts this live transport instead of opening a fresh connection.
        TUniquePtr<INetworkTransport> CarriedTransport;
        FConnectionHandle             CarriedServerConnection;
        uint32                        CarriedLocalPeerId = 0;
        bool                          bHasCarriedConnection = false;

        #if WITH_EDITOR
        IDevelopmentToolUI*     DeveloperToolUI =       nullptr;
        #endif
        
        FString                     ProjectName;
        FFixedString                ProjectPath;
        TObjectPtr<CGameInstance>   GameInstance;


        FProjectLoadedDelegate  OnProjectLoaded;
        
        bool                    bCloseRequested = false;
        bool                    bEngineReadyToClose = false;
    };
    
    RUNTIME_API extern FEngine* GEngine;

    // True for a packaged dedicated-server process (-server): no window, no RHI, no audio, no UI.
    // Set in LuminaMain before the window/engine exist. Always false in WITH_EDITOR builds; the
    // in-editor dedicated server is gated per-world by NetMode, not by this flag.
    RUNTIME_API extern bool GIsHeadless;
}
