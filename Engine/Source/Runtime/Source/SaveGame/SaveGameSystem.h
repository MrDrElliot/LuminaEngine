#pragma once

#include "SaveGameEvents.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/EventQueue.h"
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "SaveGameSystem.generated.h"

namespace Lumina
{
    // Runs the queued SSaveGameRequestEvents alone at the end of a frame, where restoring can destroy and spawn safely.
    REFLECT()
    class RUNTIME_API CSaveGameSystem : public CExclusiveSystem
    {
        GENERATED_BODY()

    public:

        void Configure() override;
        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        void Run(CWorld* World, const SSaveGameRequestEvent& Request) const;

        ECS::TEventReader<SSaveGameRequestEvent>  Requests;
        ECS::TEventWriter<SSaveGameCompletedEvent> Completions;
    };
}
