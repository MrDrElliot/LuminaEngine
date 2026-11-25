#pragma once
#include "EntitySystem.h"
#include "Animation/AnimGraphPlan.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/TaskSystem/AnimTaskRecord.h"
#include "World/Entity/Systems/DynamicComponentReads.h"
#include "Core/Threading/Mutex.h"
#include "TaskSystem/TaskSystem.h"
#include "Core/Object/ObjectMacros.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/FollowerPoseComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/KinematicsSystem.h"
#include "World/Subsystems/WorldSettings.h"
#include "AnimationSystem.generated.h"

namespace Lumina
{

    // What the drawn followers ask of one leader, the latest time one was drawn and the finest detail any needs.
    struct FAnimLeaderDemand
    {
        double FollowerRenderedTime = 0.0;
        float  DistanceOverRadius   = 0.0f;
        float  BoneLODTolerance     = 0.0f;
    };

    // One graph's crowd topologies, with the crowd plan they were built from so a recompiled graph starts over.
    struct FAnimCrowdGraphCache
    {
        const FAnimPlanOp* PlanOps   = nullptr;
        int32              PlanCount = 0;
        FAnimTopologyCache Topologies;
    };

    // The crowd tier's caches by graph, and caches a recompile replaced, kept until no worker can still hold them.
    struct FAnimCrowdCaches
    {
        THashMap<const CAnimationGraph*, TUniquePtr<FAnimCrowdGraphCache>> ByGraph;
        TVector<TUniquePtr<FAnimCrowdGraphCache>> Retired;
    };

    // What one worker holds during a crowd update, only until the update's samples are out.
    struct FAnimCrowdScratch
    {
        FAnimTaskRecord Record;
        FAnimTaskList   Tasks;

        // This worker's lock-free view of the shared caches, so an update on a path the worker has seen takes no lock.
        const CAnimationGraph*                       SeenGraph      = nullptr;
        const FAnimPlanOp*                           SeenPlanOps    = nullptr;
        int32                                        SeenPlanCount  = 0;
        FAnimTopologyCache*                          SeenTopologies = nullptr;
        THashMap<uint64, const FAnimTopologyEntry*>  SeenEntries;
    };

    // A script reload that reshapes a C# component moves the fields graph inputs read, so every graph relinks after one.
    class FAnimGraphInputRelinker final : public Scripting::IScriptReloadListener
    {
    public:

        void OnScriptsReloaded(const Scripting::FScriptReload& Reload) override;

        static void RelinkAllGraphs();
    };

    REFLECT()
    class CAnimationSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

        // A reload while no world ran left nobody to relink, so a starting world relinks before its first update.
        void OnStartup(const SExclusiveContext& Context) override;
        void OnTeardown(const SExclusiveContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SSimpleAnimationComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>, ECS::Without<SFollowerPoseComponent>> SimpleAnimators;
        ECS::TQuery<ECS::Write<SAnimationGraphComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>, ECS::Without<SFollowerPoseComponent>> GraphAnimators;
        ECS::TQuery<ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>> Meshes;
        ECS::TQuery<ECS::Write<SFollowerPoseComponent>, ECS::Write<SSkeletalMeshComponent>, ECS::Without<SDisabledTag>> Followers;
        ECS::TQuery<ECS::Read<SFollowerPoseComponent>> PoseLinks;
        ECS::TQuery<ECS::Write<SSkeletalMeshComponent>> LeaderMeshes;
        TRes<FKinematicsState> KinematicsState;
        TRes<SWorldSettingsState> WorldSettings;
        ECS::TEventWriter<SAnimMontageStartedEvent> MontageStarted;
        ECS::TEventWriter<SAnimMontageBlendingOutEvent> MontageBlendingOut;
        ECS::TEventWriter<SAnimMontageEndedEvent> MontageEnded;
        ECS::TEventWriter<SAnimNotifyFiredEvent> NotifyFired;
        THashSet<ECS::FEntity> TakenFromFollowers;

        // Gathered per worker, so the pass over every follower runs in parallel.
        TVector<THashMap<ECS::FEntity, FAnimLeaderDemand>> LeaderDemandBySlot;

        // Notify handlers never run inside this system, so its declared access only has to cover the passes.
        FAnimNotifyQueue NotifyQueue;

        // The components graphs name as inputs, declared once a graph first reads them so no writer of them runs alongside.
        FDynamicComponentReads GraphInputs;

        // Created at startup rather than with the object, so a class default object never listens.
        TUniquePtr<FAnimGraphInputRelinker> InputRelinker;

        // Read by every worker's crowd update and grown by whichever first reaches a new path or a recompiled graph.
        TRwLock<FAnimCrowdCaches> CrowdCaches;
        TVector<FAnimCrowdScratch> CrowdScratchBySlot;

    public:

        FAnimTopologyCache& CrowdTopologiesFor(const CAnimationGraph& Graph);

        // The topology of the scratch's record, through the worker's own view first and the shared caches on a miss.
        const FAnimTopologyEntry& CrowdTopology(const CAnimationGraph& Graph, FAnimCrowdScratch& Scratch, const FAnimGraphVMState& State);
        FAnimCrowdScratch& CrowdScratch() { return CrowdScratchBySlot[Jobs::GetThreadSlot()]; }

        FDynamicComponentReads& GetGraphInputs() { return GraphInputs; }
    };
}
