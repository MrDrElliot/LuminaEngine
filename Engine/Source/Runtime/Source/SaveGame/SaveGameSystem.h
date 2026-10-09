#pragma once

#include "SaveGameEvents.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Threading/Thread.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "SaveGameSystem.generated.h"

namespace Lumina
{
    // Runs queued SSaveGameRequestEvents alone at the end of a frame, where restoring can destroy and spawn safely.
    REFLECT()
    class RUNTIME_API SSaveGameSystem : public CEntitySystem
    {
        GENERATED_BODY()

    public:

        void Configure() override;
        void OnStartup() override;
        bool HasWork(EUpdateStage Stage) override;
        void OnUpdate() override;
        void OnTeardown() override;

    private:

        void OnRequest(const SSaveGameRequestEvent& Request);
        void Run(const SSaveGameRequestEvent& Request);

        // A parallel system triggers through the command bus, but a worker thread may still trigger directly.
        FMutex                          PendingLock;
        TVector<SSaveGameRequestEvent>  Pending;
        bool                            bConnected = false;
    };
}
