#include "RuntimePCH.h"
#include "AnimationSystem.h"
#include "World/ECS/Registry.h"

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
#include "World/Entity/Systems/KinematicsSystem.h"
#include "World/Entity/Systems/SignificanceSystem.h"
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
        ECS::FEntity ResolveLeaderRoot(ECS::FRegistry& Registry, ECS::FEntity Start)
        {
            constexpr int32 MaxHops = 8;

            ECS::FEntity Cursor = Start;
            for (int32 Hop = 0; Hop < MaxHops; ++Hop)
            {
                if (Cursor == ECS::NullEntity || !Registry.IsValid(Cursor))
                {
                    return ECS::NullEntity;
                }

                const SFollowerPoseComponent* Follower = Registry.TryGet<SFollowerPoseComponent>(Cursor);
                if (Follower == nullptr || Follower->Leader == SFollowerPoseComponent::NoLeader)
                {
                    return Cursor;
                }

                const ECS::FEntity Next = (ECS::FEntity)Follower->Leader;
                if (Next == Cursor)
                {
                    return ECS::NullEntity;
                }
                Cursor = Next;
            }

            return ECS::NullEntity;
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
        .Read<SCharacterMovementComponent, SystemResource::PhysicsQuery, SystemResource::Kinematics>();

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
        bool ShouldEvaluateThisFrame(SSkeletalMeshComponent& Mesh, ECS::FEntity Entity, bool bForce)
        {
            if (!Mesh.bUpdateRateOptimization || bForce)
            {
                Mesh.AnimSkipCounter = -1;
                return true;
            }

            // The gather's metric rather than SSignificanceSystem's, since it sees the real culled bounds.
            const int16 Interval = (int16)Significance::IntervalForDistanceOverRadius(Mesh.LastDistanceOverRadius);

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
                const int16 Phase = (int16)((Entity).Value % (uint32)Interval);
                if (Phase > 0)
                {
                    Mesh.AnimSkipCounter = Phase - 1;
                    return false;
                }
            }

            Mesh.AnimSkipCounter = Interval - 1;
            return true;
        }

        // Field by field, since FAnimTask has padding the builder never writes and memcmp would read.
        bool SameTask(const FAnimTask& A, const FAnimTask& B)
        {
            const bool bCommon =
                   A.Type == B.Type
                && A.Stage == B.Stage
                && A.DepA == B.DepA
                && A.DepB == B.DepB
                && A.Clip == B.Clip
                && A.Alpha == B.Alpha
                && A.AdditiveSpace == B.AdditiveSpace
                && A.MaskWeights == B.MaskWeights
                && A.Inert == B.Inert
                && A.Dead == B.Dead
                && A.Snapshot == B.Snapshot
                && A.bCapture == B.bCapture
                && A.bApply == B.bApply
                && A.T == B.T
                && A.R == B.R
                && A.S == B.S
                && A.BoneA == B.BoneA
                && A.BoneB == B.BoneB
                && A.BoneC == B.BoneC
                && A.Space == B.Space
                && A.Mode == B.Mode;

            if (!bCommon)
            {
                return false;
            }

            // On a smoothing task these two carry the blend clock, which the executor reads only on the
            // frames it captures or applies. A frame doing neither ignores however far they have drifted.
            if (A.Type == EAnimTaskType::Inertialize || A.Type == EAnimTaskType::DeadBlend)
            {
                return (!A.bCapture || A.DeltaTime == B.DeltaTime)
                    && (!A.bApply || A.Time == B.Time);
            }

            return A.Time == B.Time && A.DeltaTime == B.DeltaTime;
        }

        // The executor is a pure function of this list plus the skeleton, so matching recipes match poses.
        bool SameRecipe(const FAnimTaskList& A, const FAnimTaskList& B)
        {
            if (A.OutputTask != B.OutputTask
                || A.Skeleton != B.Skeleton
                || A.bLockRoot != B.bLockRoot
                || A.RootBoneIndex != B.RootBoneIndex
                || A.ActiveBoneCount != B.ActiveBoneCount
                || A.Tasks.size() != B.Tasks.size())
            {
                return false;
            }

            for (SIZE_T i = 0; i < A.Tasks.size(); ++i)
            {
                if (!SameTask(A.Tasks[i], B.Tasks[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // Skipped frames leave the smoothing history untouched, so its stored velocity is stale by however
        // long the pose held. Reporting one frame of history makes the next capture start from rest.
        void HoldSmoothingHistory(const FAnimTaskList& List)
        {
            for (const FAnimTask& Task : List.Tasks)
            {
                if (Task.Inert != nullptr)
                {
                    Task.Inert->HistoryCount = Math::Min(Task.Inert->HistoryCount, 1);
                }
                if (Task.Dead != nullptr)
                {
                    Task.Dead->HistoryCount = Math::Min(Task.Dead->HistoryCount, 1);
                }
            }
        }

        // Frozen time deliberately does NOT accumulate, so an off-screen pose resumes where it left off.
        bool ShouldUpdatePose(SSkeletalMeshComponent& Mesh, ECS::FEntity Entity, float DeltaTime,
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
        void UpdateSimple(SSimpleAnimationComponent& Anim, SSkeletalMeshComponent& Mesh, ECS::FEntity Entity,
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
        void UpdateGraph(const FSystemContext& SystemContext, ECS::FEntity Entity,
                         SAnimationGraphComponent& AnimGraph, SSkeletalMeshComponent& Mesh,
                         float DeltaTime, double Now, const FKinematicsState* KinematicsState)
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

                SceneContext.Velocity = Kinematics::GetVelocity(KinematicsState, Entity);
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
                                    (uint32)(Entity).Value, (int32)Mesh.AnimTasks.OutputTask);
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

        auto SimpleView   = SystemContext.CreateView<SSimpleAnimationComponent, SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag, SFollowerPoseComponent>{});
        auto GraphView    = SystemContext.CreateView<SAnimationGraphComponent, SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag, SFollowerPoseComponent>{});
        auto MeshView     = SystemContext.CreateView<SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag>{});
        auto FollowerView = SystemContext.CreateView<SFollowerPoseComponent, SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag>{});

        auto SimpleHandle   = SimpleView.GetDriver();
        auto GraphHandle    = GraphView.GetDriver();
        auto MeshHandle     = MeshView.GetDriver();
        auto FollowerHandle = FollowerView.GetDriver();

        const bool bHasSimple   = SimpleHandle != nullptr && !SimpleHandle->IsEmpty();
        const bool bHasGraph    = GraphHandle != nullptr && !GraphHandle->IsEmpty();
        const bool bHasFollower = FollowerHandle != nullptr && !FollowerHandle->IsEmpty();

        if ((!bHasSimple && !bHasGraph) || MeshHandle == nullptr)
        {
            return;
        }

        // Hoisted, since the graph bodies run in parallel and the context lookup is a per-type hash probe.
        const FKinematicsState* KinematicsState = Kinematics::GetState(SystemContext);

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
            SimpleUpdate = TaskGraph.AddParallelFor((uint32)SimpleView.NumDenseSlots(), 16, [&](const Task::FParallelRange& Range)
            {
                SimpleView.ForEachInRange(Range.Start, Range.End,
                    [&](ECS::FEntity Entity, SSimpleAnimationComponent& Anim, SSkeletalMeshComponent& Mesh)
                {
                    UpdateSimple(Anim, Mesh, Entity, DeltaTime, Now);

                    // Recording that ANY entity produced root motion lets the serial apply skip its sweep.
                    if (Anim.PendingRootMotion.bHasMotion)
                    {
                        bAnyRootMotion.store(true, std::memory_order_relaxed);
                    }
                });
            });
        }

        if (bHasGraph)
        {
            GraphUpdate = TaskGraph.AddParallelFor((uint32)GraphView.NumDenseSlots(), 16, [&](const Task::FParallelRange& Range)
            {
                GraphView.ForEachInRange(Range.Start, Range.End,
                    [&](ECS::FEntity Entity, SAnimationGraphComponent& AnimGraph, SSkeletalMeshComponent& Mesh)
                {
                    UpdateGraph(SystemContext, Entity, AnimGraph, Mesh, DeltaTime, Now, KinematicsState);

                    if (AnimGraph.PendingRootMotion.bHasMotion)
                    {
                        bAnyRootMotion.store(true, std::memory_order_relaxed);
                    }
                });
            });

            if (SimpleUpdate.IsValid())
            {
                TaskGraph.AddDependency(GraphUpdate, SimpleUpdate);
            }
        }

        // Each mesh has at most one recipe, so no entity is touched twice by the execute pass.
        const FTaskGraph::FNodeHandle Execute = TaskGraph.AddParallelFor((uint32)MeshView.NumDenseSlots(), 16, [&](const Task::FParallelRange& Range)
        {
            MeshView.ForEachInRange(Range.Start, Range.End,
                [&](ECS::FEntity Entity, SSkeletalMeshComponent& Mesh)
            {
                if (Mesh.AnimTasks.HasWork())
                {
                    // Same recipe as the pose already on this mesh, so executing it would rebuild it byte
                    // for byte. Skipping keeps the serial still, which keeps the gather off it too.
                    // bRenderBonesDirty means something else wrote the pose, so it has to be rebuilt.
                    if (Mesh.bLastRecipeValid && !Mesh.bRenderBonesDirty
                        && SameRecipe(Mesh.AnimTasks, Mesh.LastRecipe))
                    {
                        HoldSmoothingHistory(Mesh.AnimTasks);
                        Mesh.AnimTasks.Reset();
                        return;
                    }

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

                    // Cached before execution, which consumes the list.
                    Mesh.LastRecipe        = Mesh.AnimTasks;
                    Mesh.bLastRecipeValid  = true;

                    // Into scratch, so a pose identical to the one already there can leave the serial alone.
                    thread_local TVector<FMatrix4> PoseScratch;
                    const bool bProduced = Anim::ExecuteTaskList(Mesh.AnimTasks, PoseScratch, Snapshot);

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
                        const SIZE_T Num = Math::Min(RefMatrices.size(), PoseScratch.size());
                        for (SIZE_T b = 0; b < Num; ++b)
                        {
                            for (int32 c = 0; c < 4; ++c)
                            {
                                for (int32 r = 0; r < 4; ++r)
                                {
                                    const float Diff = Math::Abs(RefMatrices[b][c][r] - PoseScratch[b][c][r]);
                                    if (Diff > MaxDiff)
                                    {
                                        MaxDiff   = Diff;
                                        WorstBone = (int32)b;
                                    }
                                }
                            }
                        }
                        if (MaxDiff > 1e-4f || RefMatrices.size() != PoseScratch.size())
                        {
                            LOG_ERROR("anim.ValidateTasks: executor diverges from direct sampling (max diff {} at bone {}, sizes {}/{})",
                                      MaxDiff, WorstBone, PoseScratch.size(), RefMatrices.size());
                        }
                    }

                    // Bit-exact because the same recipe on the same inputs runs the same instructions.
                    const bool bPoseChanged = bProduced
                        && (PoseScratch.size() != Mesh.BoneTransforms.size()
                        || (!PoseScratch.empty()
                            && Memory::Memcmp(PoseScratch.data(), Mesh.BoneTransforms.data(),
                                              PoseScratch.size() * sizeof(FMatrix4)) != 0));

                    if (bPoseChanged)
                    {
                        // Swapped rather than copied, and the scratch inherits the old buffer to refill.
                        Mesh.BoneTransforms.swap(PoseScratch);

                        // A stale external write is superseded by the pose that just replaced it.
                        Mesh.bRenderBonesDirty = false;

                        // No pack here; the gather packs into its arena slice, only for what survives culling.
                        ++Mesh.PoseSerial;
                    }
                }
                        });
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
            ECS::FRegistry& Registry = SystemContext.GetRegistry();

            const FTaskGraph::FNodeHandle CopyFollowers = TaskGraph.AddParallelFor((uint32)FollowerView.NumDenseSlots(), 16, [&](const Task::FParallelRange& Range)
            {
                FollowerView.ForEachInRange(Range.Start, Range.End,
                    [&](ECS::FEntity Entity, SFollowerPoseComponent& Follower, SSkeletalMeshComponent& Mesh)
                {


                    if (Follower.Leader == SFollowerPoseComponent::NoLeader)
                    {
                        return;
                    }

                    const ECS::FEntity LeaderEntity = ResolveLeaderRoot(Registry, (ECS::FEntity)Follower.Leader);
                    if (LeaderEntity == ECS::NullEntity || LeaderEntity == Entity)
                    {
                        return;
                    }

                    const SSkeletalMeshComponent* LeaderMesh = Registry.TryGet<SSkeletalMeshComponent>(LeaderEntity);
                    if (LeaderMesh == nullptr || LeaderMesh->BoneTransforms.empty())
                    {
                        return;
                    }

                    // The leader's pose has not moved, so the copy already on this mesh still stands.
                    if (Follower.LastLeaderPoseSerial == LeaderMesh->PoseSerial && !Mesh.BoneTransforms.empty())
                    {
                        return;
                    }

                    CSkeleton* LeaderSkeletonAsset   = SkeletalUtils::GetSkeletonAsset(*LeaderMesh);
                    CSkeleton* FollowerSkeletonAsset = SkeletalUtils::GetSkeletonAsset(Mesh);
                    if (LeaderSkeletonAsset == nullptr || FollowerSkeletonAsset == nullptr)
                    {
                        return;
                    }

                    const FSkeletonResource* LeaderSkeleton   = LeaderSkeletonAsset->GetSkeletonResource();
                    const FSkeletonResource* FollowerSkeleton = FollowerSkeletonAsset->GetSkeletonResource();
                    if (LeaderSkeleton == nullptr || FollowerSkeleton == nullptr)
                    {
                        return;
                    }

                    // One skeleton behind both meshes is the common case, and it is a straight copy.
                    if (LeaderSkeleton == FollowerSkeleton)
                    {
                        Mesh.BoneTransforms = LeaderMesh->BoneTransforms;
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
                    }

                    Mesh.bRenderBonesDirty = false;
                    Follower.LastLeaderPoseSerial = LeaderMesh->PoseSerial;
                    ++Mesh.PoseSerial;
                                });
            });

            TaskGraph.AddDependency(CopyFollowers, Execute);
        }

        TaskGraph.Dispatch();
        TaskGraph.Wait();

        // Serial because a typed notify runs user code, which the parallel passes must never do.
        {
            ECS::FRegistry& Registry = SystemContext.GetRegistry();
            if (bHasSimple)
            {
                for (auto&& [Entity, Anim] : SystemContext.GetStorage<SSimpleAnimationComponent>().Each())
                {
                    if (!Anim.NotifyEvents.empty())
                    {
                        AnimEvents::DispatchTypedNotifies(Anim.NotifyEvents, Registry, Entity);
                    }
                }
            }
            if (bHasGraph)
            {
                for (auto&& [Entity, AnimGraph] : SystemContext.GetStorage<SAnimationGraphComponent>().Each())
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
        auto TransformStorage = SystemContext.GetStorage<STransformComponent>();

        const auto ApplyRootMotion = [&](ECS::FEntity Entity, FRootMotionDelta& Delta)
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

            STransformComponent& Transform = TransformStorage.Get(Entity);
            FTransform DeltaTransform;
            DeltaTransform.SetLocation(Delta.Translation);
            DeltaTransform.SetRotation(Delta.Rotation);
            DeltaTransform.SetScale(FVector3(1.0f));
            Transform.SetLocalTransform(Transform.LocalTransform * DeltaTransform);
        };
        
        if (bHasSimple)
        {
            for (auto&& [Entity, Anim] : SystemContext.GetStorage<SSimpleAnimationComponent>().Each())
            {
                if (Anim.PendingRootMotion.bHasMotion)
                {
                    ApplyRootMotion(Entity, Anim.PendingRootMotion);
                }
            }
        }

        if (bHasGraph)
        {
            for (auto&& [Entity, AnimGraph] : SystemContext.GetStorage<SAnimationGraphComponent>().Each())
            {
                if (AnimGraph.PendingRootMotion.bHasMotion)
                {
                    ApplyRootMotion(Entity, AnimGraph.PendingRootMotion);
                }
            }
        }
    }
}
