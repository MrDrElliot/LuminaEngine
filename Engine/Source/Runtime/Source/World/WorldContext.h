#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/String.h"
#include "WorldTypes.h"

namespace Lumina
{
    class CWorld;
    class CGameInstance;

    enum class ENetMode : uint8
    {
        Standalone,
        Client,
        ListenServer,
        DedicatedServer,
    };

    // Per-world runtime state owned by FWorldManager (one world = one context). Holds the world's role
    // (type, net mode, PIE) so systems branch on it without CWorld carrying editor/network concerns.
    struct FWorldContext
    {
        TObjectPtr<CWorld>      World;
        EWorldType              Type            = EWorldType::None;
        ENetMode                NetMode         = ENetMode::Standalone;
        bool                    bPIE            = false;

        // Loadable map path this world represents (e.g. "/Game/Maps/NewWorld"). Used for the networked
        // Welcome handshake: the server sends it, the client compares + travels if it differs. For a PIE
        // world this is the editor source map; for a runtime world it's the path it was opened from.
        FString                 MapPath;

        // Networking target. Server: the port to listen on. Client: the host/port to connect to.
        // Default loopback:7777 keeps the existing editor PIE flow working without an explicit URL.
        FString                 NetHost         = "127.0.0.1";
        uint16                  NetPort         = 7777;

        // PIE-only: the editor-side source world this was duplicated from.
        TWeakObjectPtr<CWorld>  SourceWorld;

        // Set in Phase 2 once CGameInstance exists; survives world transitions.
        CGameInstance*          GameInstance    = nullptr;
    };
}
