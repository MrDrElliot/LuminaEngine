#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "WorldTypes.h"
#include "WorldContext.generated.h"

namespace Lumina
{
    class CWorld;

    REFLECT()
    enum class ENetMode : uint8
    {
        Standalone,
        Client,
        ListenServer,
        DedicatedServer,
    };

    // Per-world role (type, net mode, PIE) owned by FWorldManager, so CWorld carries no editor or network concerns.
    struct FWorldContext
    {
        TStrongObjectPtr<CWorld> World;
        EWorldType              Type            = EWorldType::None;
        ENetMode                NetMode         = ENetMode::Standalone;
        bool                    bPIE            = false;

        // Loadable map path this world represents, compared by the client in the Welcome handshake.
        FString                 MapPath;

        // Server port to listen on or client host and port to connect to, defaulting to loopback 7777 for editor PIE.
        FString                 NetHost         = "127.0.0.1";
        uint16                  NetPort         = 7777;

        // PIE-only: the editor-side source world this was duplicated from.
        TWeakObjectPtr<CWorld>  SourceWorld;

        // Edge state for the render-skip diagnostic, so a legitimately throttled world logs once.
        bool                    bWarnedRenderSkip = false;

        // Driven by script instead of a person, for load testing a server with many clients from one process.
        bool                    bBot = false;
    };
}
