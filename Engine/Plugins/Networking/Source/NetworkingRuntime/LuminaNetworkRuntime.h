#pragma once

#include "Networking/INetworkRuntime.h"

namespace Lumina
{
    // Lumina's own netcode behind the engine's networking interface. Nothing outside this plugin
    // names it; the engine only ever sees INetworkRuntime.
    class FLuminaNetworkRuntime final : public INetworkRuntime
    {
    public:

        static FLuminaNetworkRuntime& Get();

        void Initialize() override;
        void Shutdown() override;
        void Update() override;


        void OnWorldEntitiesLoaded(CWorld* World) override;
        void OnEntityAttachmentChanged(CWorld* World, entt::entity Entity) override;
    };
}
