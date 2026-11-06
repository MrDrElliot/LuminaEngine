#pragma once

#include "EntitySystem.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/AudioVolumeComponent.h"
#include "World/Entity/Components/ProceduralAudioComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "KinematicsSystem.h"
#include "SignificanceSystem.h"
#include "SystemResources.h"
#include "SystemSingletons.h"
#include "AudioSystem.generated.h"

namespace Lumina
{

    // The audio device is a process singleton, but this is the only system that touches it within a world.
    REFLECT()
    class RUNTIME_API CAudioSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        // Transforms is a lookup, so only the audio queries decide.
        bool HasWork(const SSystemContext& Context) override;

        void OnUpdate(const SSystemContext& Context) override;
        void OnTeardown(const SExclusiveContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SAudioListenerComponent>> Listeners;
        ECS::TQuery<ECS::Write<SAudioVolumeComponent>> Volumes;
        ECS::TQuery<ECS::Write<SAudioSourceComponent>, ECS::Read<STransformComponent>> Sources;
        ECS::TQuery<ECS::Write<SAudioSourceComponent>> AllSources;
        ECS::TQuery<ECS::Write<SProceduralAudioComponent>> ProceduralSources;
        ECS::TQuery<ECS::Read<STransformComponent>> Transforms;
        TRes<FKinematicsState> KinematicsState;
        TRes<FSignificanceState> SignificanceState;
        TRes<FCameraGlobalState> CameraState;
    };
}
