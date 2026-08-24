#include "RuntimePCH.h"
#include "AnimationSystem.h"

#include "Core/Console/ConsoleVariable.h"

#include "Animation/AnimationGraphVM.h"
#include "Animation/RootMotion.h"
#include "Animation/SkeletalMeshUtils.h"
#include "Animation/TaskSystem/AnimTaskExecutor.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Physics/PhysicsScene.h"
#include "Renderer/MeshData.h"
#include "TaskSystem/TaskGraph.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/FollowerPoseComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SystemResources.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // A follower of a follower reads the root of the chain, so the copy pass needs no ordering
        // between followers. A cycle stops at the hop limit and simply drives nothing.
        entt::entity ResolveLeaderRoot(FEntityRegistry& Registry, entt::entity Start)
        {
            constexpr int32 MaxHops = 8;

            entt::entity Cursor = Start;
            for (int32 Hop = 0; Hop < MaxHops; ++Hop)
            {
                if (Cursor == entt::null || !Registry.valid(Cursor))
                {
                    return entt::null;
                }

                const SFollowerPoseComponent* Follower = Registry.try_get<SFollowerPoseComponent>(Cursor);
                if (Follower == nullptr || Follower->Leader == SFollowerPoseComponent::NoLeader)
                {
                    return Cursor;
                }

                const entt::entity Next = (entt::entity)Follower->Leader;
                if (Next == Cursor)
                {
                    return entt::null;
                }
                Cursor = Next;
            }

            return entt::null;
        }

        // Names are the only thing two skeletons reliably share, so the map is built from them once.
        void BuildFollowerBoneMap(SFollowerPoseComponent& Follower, const FSkeletonResource& LeaderSkeleton,
                                  const FSkeletonResource& FollowerSkeleton)
        {
            const int32 NumFollowerBones = FollowerSkeleton.GetNumBones();

            Follower.BoneMap.assign(NumFollowerBones, INDEX_NONE);
            Follower.BindFixups.assign(NumFollowerBones, FMatrix4(1.0f));

            int32 NumMatched = 0;
            for (int32 i = 0; i < NumFollowerBones; ++i)
            {
                const int32 LeaderIndex = LeaderSkeleton.FindBoneIndex(FollowerSkeleton.GetBone(i).Name);
                if (LeaderIndex == INDEX_NONE)
                {
                    continue;
                }

                Follower.BoneMap[i] = LeaderIndex;

                // The leader hands over a skinning matrix, which carries its own bind pose folded in.
                const FMatrix4 LeaderBind = Math::Inverse(LeaderSkeleton.GetBone(LeaderIndex).InvBindMatrix);
                Follower.BindFixups[i] = LeaderBind * FollowerSkeleton.GetBone(i).InvBindMatrix;
                ++NumMatched;
            }

            Follower.MappedLeaderSkeleton   = &LeaderSkeleton;
            Follower.MappedFollowerSkeleton = &FollowerSkeleton;

            if (NumMatched == 0 && Follower.bWarnOnMissingBones && !Follower.bWarnedNoMatch)
            {
                Follower.bWarnedNoMatch = true;
                LOG_WARN("Follower pose: '{}' shares no bone names with leader skeleton '{}', so it will not follow it.",
                         FollowerSkeleton.Name.ToString(), LeaderSkeleton.Name.ToString());
            }
            else if (NumMatched > 0)
            {
                Follower.bWarnedNoMatch = false;
            }
        }
    }

    // Time advance, VM state and lazy init all mutate these, so they are declared Write, not Read.
    FSystemAccess SAnimationSystem::Access = FSystemAccess{}
        .Write<SSkeletalMeshComponent, STransformComponent, SSimpleAnimationComponent, SAnimationGraphComponent,
               SFollowerPoseComponent>()
        .Read<SCharacterMovementComponent>();

    // Slack so brief occlusion or culling flicker does not stutter the pose.
    static constexpr double kAnimVisibilityGrace = 0.25;

    // Diagnostic cross-check of the deferred executor against direct single-clip sampling.
    static TConsoleVar<bool> CVarValidateAnimTasks(
        "anim.ValidateTasks",
        false,
        "Re-evaluate single-clip animation recipes directly and compare against the task executor's skinning matrices; logs mismatches.");

    // Diagnostic dump of the task recipe each graph-driven mesh records.
    static TConsoleVar<bool> CVarDumpGraphTasks(
        "anim.DumpGraphTasks",
        false,
        "Log every graph-driven mesh's recorded animation task list (types, deps, clips, times, alphas, masks) each frame while enabled.");


    // A parallel update phase records each entity's recipe, then a parallel execute phase runs it.
    namespace
    {
        // Skipped time stays in PendingAnimTime, so playback speed survives the reduced update rate.
        bool ShouldEvaluateThisFrame(SSkeletalMeshComponent& Mesh, entt::entity Entity, bool bForce)
        {
            if (!Mesh.bUpdateRateOptimization || bForce)
            {
                Mesh.AnimSkipCounter = -1;
                return true;
            }

            const float DoR = Mesh.LastDistanceOverRadius;
            int16 Interval = 1;
            if (DoR > 120.0f)
            {
                Interval = 4;
            }
            else if (DoR > 60.0f)
            {
                Interval = 3;
            }
            else if (DoR > 30.0f)
            {
                Interval = 2;
            }

            if (Interval <= 1)
            {
                Mesh.AnimSkipCounter = -1;
                return true;
            }

            if (Mesh.AnimSkipCounter > 0)
            {
                --Mesh.AnimSkipCounter;
                return false;
            }

            if (Mesh.AnimSkipCounter < 0)
            {
                // First reduced-rate frame phases by entity id, then settles into the countdown.
                const int16 Phase = (int16)(entt::to_integral(Entity) % (uint32)Interval);
                if (Phase > 0)
                {
                    Mesh.AnimSkipCounter = Phase - 1;
                    return false;
                }
            }

            Mesh.AnimSkipCounter = Interval - 1;
            return true;
        }

        // Frozen time deliberately does NOT accumulate, so an off-screen pose resumes where it left off.
        bool ShouldUpdatePose(SSkeletalMeshComponent& Mesh, entt::entity Entity, float DeltaTime,
                              double Now, bool bForce, float& OutStepTime)
        {
            OutStepTime = 0.0f;

            if (Mesh.VisibilityBasedAnimTick == EAnimUpdateMode::TickWhenRendered &&
                (Now - Mesh.LastRenderedTime) > kAnimVisibilityGrace)
            {
                return false;
            }

            Mesh.PendingAnimTime += DeltaTime;
            if (!ShouldEvaluateThisFrame(Mesh, Entity, bForce))
            {
                return false;
            }

            OutStepTime = Mesh.PendingAnimTime;
            Mesh.PendingAnimTime = 0.0f;
            return true;
        }

        // Distance over bounding radius past which the skeleton's low-detail bone prefix is used.
        constexpr float kBoneLODDistanceOverRadius = 60.0f;

        // Authored-only, because bone order past parents-first is importer-dependent.
        int32 ComputeActiveBoneCount(const SSkeletalMeshComponent& Mesh, const FSkeletonResource* Skeleton)
        {
            const int32 NumBones = Skeleton->GetNumBones();
            if (!Mesh.bUpdateRateOptimization || Mesh.LastDistanceOverRadius <= kBoneLODDistanceOverRadius)
            {
                return 0; // all bones
            }

            const CSkeleton* Asset = Mesh.SkeletalMesh.IsValid() ? Mesh.SkeletalMesh->Skeleton.Get() : nullptr;
            const int32 Count = Asset ? Asset->LowDetailBoneCount : 0;
            return (Count > 0 && Count < NumBones) ? Count : 0;
        }

        FSkeletonResource* ResolveSkeleton(SSkeletalMeshComponent& Mesh)
        {
            if (!Mesh.SkeletalMesh.IsValid())
            {
                return nullptr;
            }
            CSkeletalMesh* SkelMesh = Mesh.SkeletalMesh;
            if (!SkelMesh->Skeleton.IsValid())
            {
                return nullptr;
            }
            FSkeletonResource* Skeleton = SkelMesh->Skeleton->GetSkeletonResource();
            return (Skeleton != nullptr && Skeleton->GetNumBones() > 0) ? Skeleton : nullptr;
        }

        // One entity's single-clip update, parallel-safe because it touches only this entity.
        void UpdateSimple(SSimpleAnimationComponent& Anim, SSkeletalMeshComponent& Mesh, entt::entity Entity,
                          float DeltaTime, double Now)
        {
            Anim.NotifyEvents.clear();

            if (!Anim.Animation.IsValid())
            {
                Anim.bAdvancedThisFrame = false;
                return;
            }

            FSkeletonResource* Skeleton = ResolveSkeleton(Mesh);
            if (Skeleton == nullptr)
            {
                Anim.bAdvancedThisFrame = false;
                return;
            }

            if (!Anim.bPlaying && !Anim.bDirty)
            {
                Anim.bAdvancedThisFrame = false;
                return;
            }

            // A fresh PlayAnimation (bDirty) always evaluates immediately.
            float StepTime = 0.0f;
            if (!ShouldUpdatePose(Mesh, Entity, DeltaTime, Now, Anim.bDirty, StepTime))
            {
                Anim.bAdvancedThisFrame = false;
                return;
            }

            const float Duration = Anim.Animation->GetDuration();

            if (Anim.bPlaying)
            {
                // Record the pre-advance time so the notify pass can find crossings.
                Anim.PreviousTime = Anim.CurrentTime;
                Anim.CurrentTime += StepTime * Anim.PlaybackSpeed;

                if (Duration > 0.0f && Anim.CurrentTime >= Duration)
                {
                    if (Anim.bLooping)
                    {
                        Anim.CurrentTime = fmodf(Anim.CurrentTime, Duration);
                    }
                    else
                    {
                        Anim.CurrentTime = Duration;
                        Anim.bPlaying    = false;
                        Anim.bFinished   = true;
                    }
                }

                Anim.bAdvancedThisFrame = true;
            }
            else
            {
                Anim.PreviousTime       = Anim.CurrentTime;
                Anim.bAdvancedThisFrame = false;
            }

            CAnimation* Asset = Anim.Animation.Get();

            // A stopped clip ends every active notify state so effects never leak.
            if (Asset->HasNotifies())
            {
                if (Anim.bAdvancedThisFrame)
                {
                    AnimEvents::CollectTriggeredNotifies(Asset, Anim.PreviousTime, Anim.CurrentTime,
                                                         Anim.bLooping, 1.0f, Anim.NotifyEvents);
                }

                const TVector<FAnimationNotifyState>& States = Asset->GetNotifyStates();

                thread_local TVector<int32> NowActive;
                NowActive.clear();
                if (Anim.bAdvancedThisFrame)
                {
                    for (int32 i = 0; i < (int32)States.size(); ++i)
                    {
                        if (Anim.CurrentTime >= States[i].StartTime && Anim.CurrentTime <= States[i].EndTime)
                        {
                            NowActive.push_back(i);
                        }
                    }
                }

                const auto EmitState = [&](int32 StateIdx, EAnimNotifyEventType Type)
                {
                    const FAnimationNotifyState& Authored = States[StateIdx];
                    const float Span = Authored.EndTime - Authored.StartTime;

                    FAnimNotifyEvent& Event = Anim.NotifyEvents.emplace_back();
                    Event.Name      = Authored.NotifyName;
                    Event.Track     = Authored.NotifyTrack;
                    Event.Type      = Type;
                    Event.Animation = Asset;
                    Event.State     = Authored.Notify.Get();
                    Event.Alpha     = Span > 0.0f ? Math::Clamp((Anim.CurrentTime - Authored.StartTime) / Span, 0.0f, 1.0f) : 0.0f;
                };

                for (int32 Idx : NowActive)
                {
                    if (Algo::Find(Anim.ActiveNotifyStates.begin(), Anim.ActiveNotifyStates.end(), Idx) ==
                        Anim.ActiveNotifyStates.end())
                    {
                        EmitState(Idx, EAnimNotifyEventType::Begin);
                    }
                    EmitState(Idx, EAnimNotifyEventType::Tick);
                }
                for (int32 Idx : Anim.ActiveNotifyStates)
                {
                    if (Idx >= 0 && Idx < (int32)States.size() &&
                        Algo::Find(NowActive.begin(), NowActive.end(), Idx) == NowActive.end())
                    {
                        EmitState(Idx, EAnimNotifyEventType::End);
                    }
                }
                Anim.ActiveNotifyStates.assign(NowActive.begin(), NowActive.end());
            }

            const bool bLock = (Anim.RootMotionLock == ERootMotionLockMode::ForceLock) ||
                               (Anim.RootMotionLock == ERootMotionLockMode::FromAsset && Asset->bLockRootMotion);

            // An additive clip's root track is a delta against its base, never entity motion.
            const bool bExtract = !bLock && Asset->bEnableRootMotion && !Asset->IsAdditive();

            // Resolved only when consumed, since a named RootBoneName costs a hash and a random probe.
            const int32 RootIdx = (bLock || bExtract)
                ? RootMotion::ResolveRootBoneIndex(Skeleton, Asset->RootBoneName)
                : INDEX_NONE;

            Anim.PendingRootMotion.bHasMotion = false;

            FAnimTaskList& Tasks = Mesh.AnimTasks;
            Tasks.Reset();
            Tasks.Skeleton        = Skeleton;
            Tasks.ActiveBoneCount = ComputeActiveBoneCount(Mesh, Skeleton);

            FAnimTask Sample;
            Sample.Type = EAnimTaskType::SampleClip;
            Sample.Clip = Asset;
            Sample.Time = Anim.CurrentTime;

            if (Asset->IsAdditive())
            {
                // Played on its own, an additive clip shows layered onto the base it was authored against.
                FAnimTask Base;
                if (CAnimation* BaseClip = Asset->GetAdditiveBaseAnimation())
                {
                    Base.Type = EAnimTaskType::SampleClip;
                    Base.Clip = BaseClip;
                    Base.Time = Asset->GetAdditiveBaseTime(Anim.CurrentTime);
                }
                else
                {
                    Base.Type = EAnimTaskType::ReferencePose;
                }

                FAnimTask Apply;
                Apply.Type  = EAnimTaskType::ApplyAdditive;
                Apply.DepA  = Tasks.Add(Base);
                Apply.DepB  = Tasks.Add(Sample);
                Apply.Alpha = 1.0f;
                Tasks.OutputTask = Tasks.Add(Apply);
            }
            else
            {
                Tasks.OutputTask = Tasks.Add(Sample);
            }

            if (RootIdx != INDEX_NONE)
            {
                if (bLock)
                {
                    Tasks.bLockRoot     = true;
                    Tasks.RootBoneIndex = RootIdx;
                }
                else if (bExtract && Anim.bAdvancedThisFrame)
                {
                    // Skipped on seek and stop frames so scrubbing does not teleport the entity.
                    Anim.PendingRootMotion = RootMotion::ExtractRootDelta(
                        Asset, Skeleton, RootIdx, Anim.PreviousTime, Anim.CurrentTime, Anim.bLooping, Duration);
                    Tasks.bLockRoot     = true;
                    Tasks.RootBoneIndex = RootIdx;
                }
            }

            Anim.bDirty = false;
        }

        // Reads the graph's parameters straight out of the component's own parameter block.
        void ApplyParameters(SAnimationGraphComponent& AnimGraph, const CAnimationGraph* Graph)
        {
            const uint8* Base = static_cast<const uint8*>(AnimGraph.GetParameterMemory());
            if (Base == nullptr)
            {
                return;
            }

            FAnimGraphVMState& VMState = AnimGraph.VMState;

            const SIZE_T NumParams = Math::Min(Graph->ParamBindings.size(), VMState.Parameters.size());
            for (SIZE_T i = 0; i < NumParams; ++i)
            {
                const FAnimGraphParamBinding& Binding = Graph->ParamBindings[i];
                if (Binding.IsResolved())
                {
                    VMState.Parameters[i] = ReadAnimParamScalar(Base, Binding);
                }
            }

            const SIZE_T NumObjects = Math::Min(Graph->ObjectParamBindings.size(), VMState.ObjectParameters.size());
            for (SIZE_T i = 0; i < NumObjects; ++i)
            {
                const FAnimGraphParamBinding& Binding = Graph->ObjectParamBindings[i];
                if (Binding.IsResolved())
                {
                    VMState.ObjectParameters[i] = ReadAnimParamObject(Base, Binding);
                }
            }
        }

        // One entity's graph update, parallel-safe; pose math happens in the execute phase.
        void UpdateGraph(const FSystemContext& SystemContext, entt::entity Entity,
                         SAnimationGraphComponent& AnimGraph, SSkeletalMeshComponent& Mesh,
                         float DeltaTime, double Now)
        {
            AnimGraph.NotifyEvents.clear();
            AnimGraph.PendingRootMotion = FRootMotionDelta();

            if (!AnimGraph.Graph.IsValid())
            {
                AnimGraph.Montages.Reset();
                return;
            }

            CAnimationGraph* Graph = AnimGraph.Graph.Get();
            if (!Graph->IsCompiled())
            {
                return;
            }

            FSkeletonResource* Skeleton = ResolveSkeleton(Mesh);
            if (Skeleton == nullptr)
            {
                return;
            }

            // Skipped frames keep the previous pose; the next evaluation consumes the accumulated step.
            float StepTime = 0.0f;
            if (!ShouldUpdatePose(Mesh, Entity, DeltaTime, Now, false, StepTime))
            {
                return;
            }

            // Init VM state first so BuildTasks won't re-init and wipe the written values.
            AnimGraph.EnsureStateInitialized();
            ApplyParameters(AnimGraph, Graph);

            FAnimGraphRootMotion GraphRootMotion;
            GraphRootMotion.Mode          = AnimGraph.RootMotionLock;
            GraphRootMotion.RootBoneIndex = RootMotion::ResolveRootBoneIndex(Skeleton, FName());

            AnimGraph.Montages.Update(StepTime, &AnimGraph.NotifyEvents);

            // Ground traces are read-only and PrePhysics never overlaps the step, so querying here is safe.
            FAnimGraphSceneContext SceneContext;
            if (const STransformComponent* Transform = SystemContext.TryGet<STransformComponent>(Entity))
            {
                const VTransform World = Transform->GetWorldTransformCached();

                SceneContext.Scene          = SystemContext.GetPhysicsScene();
                SceneContext.BoneTransforms = &Mesh.BoneTransforms;
                SceneContext.WorldLocation  = World.GetLocation();
                SceneContext.WorldScale     = World.GetScale();
                SceneContext.WorldRotation  = World.GetRotation();
                SceneContext.SelfBodyID     = SystemContext.GetEntityBodyID(Entity);

                if (const SCharacterMovementComponent* Movement = SystemContext.TryGet<SCharacterMovementComponent>(Entity))
                {
                    SceneContext.Velocity = Movement->Velocity;
                }
                else if (Physics::IPhysicsScene* Scene = SceneContext.Scene)
                {
                    SceneContext.Velocity = Scene->GetLinearVelocity(Entity);
                }
            }

            FAnimationGraphVM::BuildTasks(Graph, Skeleton, StepTime, AnimGraph.VMState, Mesh.AnimTasks,
                                          GraphRootMotion, &AnimGraph.NotifyEvents, &AnimGraph.Montages,
                                          &SceneContext);
            Mesh.AnimTasks.ActiveBoneCount = ComputeActiveBoneCount(Mesh, Skeleton);

            if (CVarDumpGraphTasks.GetValue())
            {
                static const char* TaskTypeNames[] =
                {
                    "RefPose", "SampleClip", "Blend", "BlendMasked", "MakeAdditive",
                    "ApplyAdditive", "SMOutput", "BoneTransform", "TwoBoneIK",
                };

                FString Dump;
                AppendFormat(Dump, "[AnimTasks] entity {} output={}",
                                    (uint32)entt::to_integral(Entity), (int32)Mesh.AnimTasks.OutputTask);
                for (SIZE_T t = 0; t < Mesh.AnimTasks.Tasks.size(); ++t)
                {
                    const FAnimTask& Task = Mesh.AnimTasks.Tasks[t];
                    AppendFormat(Dump, "\n  [{}] {:<13} depA={:<3} depB={:<3} alpha={:.3f}",
                                        (uint32)t, TaskTypeNames[(int32)Task.Type],
                                        (int32)Task.DepA, (int32)Task.DepB, Task.Alpha);
                    if (Task.Type == EAnimTaskType::SampleClip)
                    {
                        AppendFormat(Dump, " clip={} time={:.4f}",
                                            Task.Clip != nullptr ? Task.Clip->GetName().c_str() : "<null>",
                                            Task.Time);
                    }
                    if (Task.MaskWeights != nullptr)
                    {
                        int32 NumWeighted = 0;
                        for (float Weight : *Task.MaskWeights)
                        {
                            NumWeighted += Weight > 0.5f ? 1 : 0;
                        }
                        AppendFormat(Dump, " mask={}/{} bones", NumWeighted, (int32)Task.MaskWeights->size());
                    }
                }
                LOG_INFO("{}", Dump.c_str());
            }

            AnimGraph.PendingRootMotion = GraphRootMotion.Delta;
        }
    }

    void SAnimationSystem::Update(const FSystemContext& SystemContext) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        auto SimpleView   = SystemContext.CreateView<SSimpleAnimationComponent, SSkeletalMeshComponent>(entt::exclude<SDisabledTag, SFollowerPoseComponent>);
        auto GraphView    = SystemContext.CreateView<SAnimationGraphComponent, SSkeletalMeshComponent>(entt::exclude<SDisabledTag, SFollowerPoseComponent>);
        auto MeshView     = SystemContext.CreateView<SSkeletalMeshComponent>(entt::exclude<SDisabledTag>);
        auto FollowerView = SystemContext.CreateView<SFollowerPoseComponent, SSkeletalMeshComponent>(entt::exclude<SDisabledTag>);

        auto SimpleHandle   = SimpleView.handle();
        auto GraphHandle    = GraphView.handle();
        auto MeshHandle     = MeshView.handle();
        auto FollowerHandle = FollowerView.handle();

        const bool bHasSimple   = SimpleHandle != nullptr && !SimpleHandle->empty();
        const bool bHasGraph    = GraphHandle != nullptr && !GraphHandle->empty();
        const bool bHasFollower = FollowerHandle != nullptr && !FollowerHandle->empty();

        if ((!bHasSimple && !bHasGraph) || MeshHandle == nullptr)
        {
            return;
        }

        const float  DeltaTime = (float)SystemContext.GetDeltaTime();
        const double Now       = SystemContext.GetTime();

        // Relaxed throughout, since the TaskGraph::Wait() before the read is the ordering edge.
        std::atomic<bool> bAnyRootMotion{ false };

        FTaskGraph TaskGraph;

        // The graph pass runs after the simple pass so a dual-component entity resolves the same way.
        FTaskGraph::FNodeHandle SimpleUpdate;
        FTaskGraph::FNodeHandle GraphUpdate;

        if (bHasSimple)
        {
            SimpleUpdate = TaskGraph.AddParallelFor((uint32)SimpleHandle->size(), 16, [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    entt::entity Entity = (*SimpleHandle)[i];
                    if (!SimpleView.contains(Entity))
                    {
                        continue;
                    }
                    SSimpleAnimationComponent& Anim = SimpleView.get<SSimpleAnimationComponent>(Entity);
                    UpdateSimple(Anim, SimpleView.get<SSkeletalMeshComponent>(Entity), Entity, DeltaTime, Now);

                    // Recording that ANY entity produced root motion lets the serial apply skip its sweep.
                    if (Anim.PendingRootMotion.bHasMotion)
                    {
                        bAnyRootMotion.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }

        if (bHasGraph)
        {
            GraphUpdate = TaskGraph.AddParallelFor((uint32)GraphHandle->size(), 16, [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    entt::entity Entity = (*GraphHandle)[i];
                    if (!GraphView.contains(Entity))
                    {
                        continue;
                    }
                    SAnimationGraphComponent& AnimGraph = GraphView.get<SAnimationGraphComponent>(Entity);
                    UpdateGraph(SystemContext, Entity, AnimGraph,
                                GraphView.get<SSkeletalMeshComponent>(Entity), DeltaTime, Now);

                    if (AnimGraph.PendingRootMotion.bHasMotion)
                    {
                        bAnyRootMotion.store(true, std::memory_order_relaxed);
                    }
                }
            });

            if (SimpleUpdate.IsValid())
            {
                TaskGraph.AddDependency(GraphUpdate, SimpleUpdate);
            }
        }

        // Each mesh has at most one recipe, so no entity is touched twice by the execute pass.
        const FTaskGraph::FNodeHandle Execute = TaskGraph.AddParallelFor((uint32)MeshHandle->size(), 16, [&](const Task::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                entt::entity Entity = (*MeshHandle)[i];
                if (!MeshView.contains(Entity))
                {
                    continue;
                }
                SSkeletalMeshComponent& Mesh = MeshView.get<SSkeletalMeshComponent>(Entity);
                if (Mesh.AnimTasks.HasWork())
                {
                    // Snapshot single-clip recipes before execution consumes the list (diagnostic).
                    CAnimation* ValidateClip = nullptr;
                    float  ValidateTime = 0.0f;
                    bool   bValidateLock = false;
                    int32  ValidateRoot = INDEX_NONE;
                    FSkeletonResource* ValidateSkeleton = Mesh.AnimTasks.Skeleton;
                    if (CVarValidateAnimTasks.GetValue() &&
                        Mesh.AnimTasks.Tasks.size() == 1 &&
                        Mesh.AnimTasks.Tasks[0].Type == EAnimTaskType::SampleClip &&
                        Mesh.AnimTasks.ActiveBoneCount == 0)
                    {
                        ValidateClip  = Mesh.AnimTasks.Tasks[0].Clip;
                        ValidateTime  = Mesh.AnimTasks.Tasks[0].Time;
                        bValidateLock = Mesh.AnimTasks.bLockRoot;
                        ValidateRoot  = Mesh.AnimTasks.RootBoneIndex;
                    }

                    // Armed for at most one component, so this is a null atomic compare for every other mesh.
                    FAnimTaskSnapshot* Snapshot = nullptr;
                    thread_local FAnimTaskSnapshot CaptureScratch;
                    if (Anim::IsTaskCaptureArmed(&Mesh))
                    {
                        CaptureScratch.Reset();
                        Snapshot = &CaptureScratch;
                    }

                    Anim::ExecuteTaskList(Mesh.AnimTasks, Mesh.BoneTransforms, Snapshot);

                    if (Snapshot != nullptr)
                    {
                        Anim::StoreTaskCapture(*Snapshot);
                    }

                    // Divergence means a playback-logic bug; agreement means the animation data is wrong.
                    if (ValidateClip != nullptr && ValidateSkeleton != nullptr)
                    {
                        thread_local FPose RefPose;
                        thread_local TVector<FMatrix4> RefMatrices;
                        ValidateClip->SampleLocalPose(ValidateTime, ValidateSkeleton, RefPose);
                        if (bValidateLock && ValidateRoot != INDEX_NONE)
                        {
                            RootMotion::PinRootToBindPose(RefPose, ValidateSkeleton, ValidateRoot);
                        }
                        AnimPose::ToSkinningMatrices(RefPose, ValidateSkeleton, RefMatrices);

                        float MaxDiff = 0.0f;
                        int32 WorstBone = INDEX_NONE;
                        const SIZE_T Num = Math::Min(RefMatrices.size(), Mesh.BoneTransforms.size());
                        for (SIZE_T b = 0; b < Num; ++b)
                        {
                            for (int32 c = 0; c < 4; ++c)
                            {
                                for (int32 r = 0; r < 4; ++r)
                                {
                                    const float Diff = Math::Abs(RefMatrices[b][c][r] - Mesh.BoneTransforms[b][c][r]);
                                    if (Diff > MaxDiff)
                                    {
                                        MaxDiff   = Diff;
                                        WorstBone = (int32)b;
                                    }
                                }
                            }
                        }
                        if (MaxDiff > 1e-4f || RefMatrices.size() != Mesh.BoneTransforms.size())
                        {
                            LOG_ERROR("anim.ValidateTasks: executor diverges from direct sampling (max diff {} at bone {}, sizes {}/{})",
                                      MaxDiff, WorstBone, Mesh.BoneTransforms.size(), RefMatrices.size());
                        }
                    }

                    // Packed here so the render gather bulk-copies instead of converting per bone.
                    SkeletalUtils::PackRenderBones(Mesh.BoneTransforms, Mesh.RenderBones);
                    Mesh.bRenderBonesDirty = false;
                    ++Mesh.PoseSerial;
                }
            }
        });

        if (SimpleUpdate.IsValid())
        {
            TaskGraph.AddDependency(Execute, SimpleUpdate);
        }
        if (GraphUpdate.IsValid())
        {
            TaskGraph.AddDependency(Execute, GraphUpdate);
        }

        // Copying is its own pass so every leader has finished before any follower reads one. Followers
        // resolve to the root of their chain, so nothing here depends on the order within the pass.
        if (bHasFollower)
        {
            FEntityRegistry& Registry = SystemContext.GetRegistry();

            const FTaskGraph::FNodeHandle CopyFollowers = TaskGraph.AddParallelFor((uint32)FollowerHandle->size(), 16, [&](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    const entt::entity Entity = (*FollowerHandle)[i];
                    if (!FollowerView.contains(Entity))
                    {
                        continue;
                    }

                    SFollowerPoseComponent& Follower = FollowerView.get<SFollowerPoseComponent>(Entity);
                    SSkeletalMeshComponent& Mesh     = FollowerView.get<SSkeletalMeshComponent>(Entity);

                    if (Follower.Leader == SFollowerPoseComponent::NoLeader)
                    {
                        continue;
                    }

                    const entt::entity LeaderEntity = ResolveLeaderRoot(Registry, (entt::entity)Follower.Leader);
                    if (LeaderEntity == entt::null || LeaderEntity == Entity)
                    {
                        continue;
                    }

                    const SSkeletalMeshComponent* LeaderMesh = Registry.try_get<SSkeletalMeshComponent>(LeaderEntity);
                    if (LeaderMesh == nullptr || LeaderMesh->RenderBones.empty())
                    {
                        continue;
                    }

                    // The leader's pose has not moved, so the copy already on this mesh still stands.
                    if (Follower.LastLeaderPoseSerial == LeaderMesh->PoseSerial && !Mesh.RenderBones.empty())
                    {
                        continue;
                    }

                    CSkeleton* LeaderSkeletonAsset   = SkeletalUtils::GetSkeletonAsset(*LeaderMesh);
                    CSkeleton* FollowerSkeletonAsset = SkeletalUtils::GetSkeletonAsset(Mesh);
                    if (LeaderSkeletonAsset == nullptr || FollowerSkeletonAsset == nullptr)
                    {
                        continue;
                    }

                    const FSkeletonResource* LeaderSkeleton   = LeaderSkeletonAsset->GetSkeletonResource();
                    const FSkeletonResource* FollowerSkeleton = FollowerSkeletonAsset->GetSkeletonResource();
                    if (LeaderSkeleton == nullptr || FollowerSkeleton == nullptr)
                    {
                        continue;
                    }

                    // One skeleton behind both meshes is the common case, and it is a straight copy.
                    if (LeaderSkeleton == FollowerSkeleton)
                    {
                        Mesh.BoneTransforms = LeaderMesh->BoneTransforms;
                        Mesh.RenderBones    = LeaderMesh->RenderBones;
                    }
                    else
                    {
                        if (Follower.MappedLeaderSkeleton != LeaderSkeleton ||
                            Follower.MappedFollowerSkeleton != FollowerSkeleton ||
                            Follower.BoneMap.size() != (SIZE_T)FollowerSkeleton->GetNumBones())
                        {
                            BuildFollowerBoneMap(Follower, *LeaderSkeleton, *FollowerSkeleton);
                        }

                        const int32 NumFollowerBones = FollowerSkeleton->GetNumBones();
                        const int32 NumLeaderMatrices = (int32)LeaderMesh->BoneTransforms.size();
                        Mesh.BoneTransforms.resize(NumFollowerBones);

                        for (int32 Bone = 0; Bone < NumFollowerBones; ++Bone)
                        {
                            const int32 LeaderBone = Follower.BoneMap[Bone];

                            // An unmatched bone resolves to its own bind pose, which skins as the identity.
                            Mesh.BoneTransforms[Bone] = (LeaderBone >= 0 && LeaderBone < NumLeaderMatrices)
                                ? LeaderMesh->BoneTransforms[LeaderBone] * Follower.BindFixups[Bone]
                                : FMatrix4(1.0f);
                        }

                        SkeletalUtils::PackRenderBones(Mesh.BoneTransforms, Mesh.RenderBones);
                    }

                    Mesh.bRenderBonesDirty = false;
                    Follower.LastLeaderPoseSerial = LeaderMesh->PoseSerial;
                    ++Mesh.PoseSerial;
                }
            });

            TaskGraph.AddDependency(CopyFollowers, Execute);
        }

        TaskGraph.Dispatch();
        TaskGraph.Wait();

        // Serial because a typed notify runs user code, which the parallel passes must never do.
        {
            FEntityRegistry& Registry = SystemContext.GetRegistry();
            if (bHasSimple)
            {
                for (auto&& [Entity, Anim] : SystemContext.GetStorage<SSimpleAnimationComponent>().each())
                {
                    if (!Anim.NotifyEvents.empty())
                    {
                        AnimEvents::DispatchTypedNotifies(Anim.NotifyEvents, Registry, Entity);
                    }
                }
            }
            if (bHasGraph)
            {
                for (auto&& [Entity, AnimGraph] : SystemContext.GetStorage<SAnimationGraphComponent>().each())
                {
                    if (!AnimGraph.NotifyEvents.empty())
                    {
                        AnimEvents::DispatchTypedNotifies(AnimGraph.NotifyEvents, Registry, Entity);
                    }
                }
            }
        }

        // Nothing moved, so everything below is dead work.
        if (!bAnyRootMotion.load(std::memory_order_relaxed))
        {
            return;
        }

        // Serial because applying root motion marks the transform dirty, which is not ParallelFor-safe.
        auto&& TransformStorage = SystemContext.GetStorage<STransformComponent>();

        const auto ApplyRootMotion = [&](entt::entity Entity, FRootMotionDelta& Delta)
        {
            if (!Delta.bHasMotion)
            {
                return;
            }
            Delta.bHasMotion = false;

            // Root-motion branches stay tagged on paused frames, and an identity delta must not dirty.
            if (Math::LengthSquared(Delta.Translation) < 1e-12f && Math::Abs(Delta.Rotation.w) > 0.9999995f)
            {
                return;
            }

            STransformComponent& Transform = TransformStorage.get(Entity);
            FTransform DeltaTransform;
            DeltaTransform.SetLocation(Delta.Translation);
            DeltaTransform.SetRotation(Delta.Rotation);
            DeltaTransform.SetScale(FVector3(1.0f));
            Transform.SetLocalTransform(Transform.LocalTransform * DeltaTransform);
        };
        
        if (bHasSimple)
        {
            for (auto&& [Entity, Anim] : SystemContext.GetStorage<SSimpleAnimationComponent>().each())
            {
                if (Anim.PendingRootMotion.bHasMotion)
                {
                    ApplyRootMotion(Entity, Anim.PendingRootMotion);
                }
            }
        }

        if (bHasGraph)
        {
            for (auto&& [Entity, AnimGraph] : SystemContext.GetStorage<SAnimationGraphComponent>().each())
            {
                if (AnimGraph.PendingRootMotion.bHasMotion)
                {
                    ApplyRootMotion(Entity, AnimGraph.PendingRootMotion);
                }
            }
        }
    }
}
