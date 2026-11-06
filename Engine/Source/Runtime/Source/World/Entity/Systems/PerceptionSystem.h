#pragma once

#include "World/ECS/Registry.h"

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "AI/Perception/PerceptionTypes.h"
#include "AI/Perception/PerceptionWorldState.h"
#include "SignificanceSystem.h"
#include "SystemResources.h"
#include "World/ECS/EventQueue.h"
#include "World/Entity/Components/AIStimuliSourceComponent.h"
#include "World/Entity/Components/PerceptionComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Events/PerceptionEvent.h"
#include "PerceptionSystem.generated.h"

namespace Lumina
{
    // Runs every perceiver's senses and sends perceived/lost events, ahead of PathFollow so an AI reacts the same frame.
    REFLECT()
    class RUNTIME_API CPerceptionSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        // Sources alone would only drain stimuli no perceiver can hear, so the perceivers decide.
        bool HasWork(const SSystemContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SPerceptionComponent>, ECS::Read<STransformComponent>> Perceivers;
        ECS::TQuery<ECS::Read<SAIStimuliSourceComponent>, ECS::Read<STransformComponent>> Sources;
        ECS::TQuery<ECS::Read<SAIStimuliSourceComponent>> Instigators;
        ECS::TEventWriter<SPerceptionEvent> PerceptionEvents;
        TResMut<FPerceptionWorldState> PerceptionState;
        TRes<FSignificanceState> SignificanceState;

        TVector<FAIStimulusEvent> Stimuli;
    };
}
