#include "pch.h"
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
#include "Renderer/MeshData.h"
#include "TaskSystem/TaskGraph.h"
#include "world/entity/components/entitytags.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/BlackboardComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SystemResources.h"
#include "Log/Log.h"

namespace Lumina
{
    // Mutates the simple-anim / graph / blackboard state (time advance, VM state, lazy init), so they are
    // WRITES, not reads, the system is their sole writer but the declaration must be honest for scheduling.
    FSystemAccess SAnimationSystem::Access = FSystemAccess{}
        .Write<SSkeletalMeshComponent, STransformComponent, SSimpleAnimationComponent, SAnimationGraphComponent, SBlackboardComponent>();

    // Skeletons not rendered within this window are treated as off-screen (a few frames of slack so brief
    // occlusion / culling flicker doesn't stutter the pose).
    static constexpr double kAnimVisibilityGrace = 0.25;

    // Diagnostic: cross-check the deferred task executor against direct sampling for single-clip
    // recipes and log any divergence. Answers "is the animation data wrong, or the playback logic?"
    static TConsoleVar<bool> CVarValidateAnimTasks(
        "anim.ValidateTasks",
        false,
        "Re-evaluate single-clip animation recipes directly and compare against the task executor's skinning matrices; logs mismatches.");

    // Diagnostic: log the task recipe each graph-driven mesh records. Answers "what did the graph
    // actually build?" when a blend looks wrong -- shared clocks, refpose fallbacks on a wired pin,
    // and dead mask weights all show up directly in the dump.
    static TConsoleVar<bool> CVarDumpGraphTasks(
        "anim.DumpGraphTasks",
        false,
        "Log every graph-driven mesh's recorded animation task list (types, deps, clips, times, alphas, masks) each frame while enabled.");


    // The frame is split Esoterica-style: a cheap parallel UPDATE phase runs graph/playback logic and
    // records each entity's pose recipe (FAnimTaskList), then a parallel EXECUTE phase runs only the
    // recorded task chains into skinning matrices. Pose math for inactive state-machine branches never
    // runs, and pose buffers come from small per-worker pools instead of per-instance registers.
    namespace
    {
        // Update-rate optimization: distant meshes re-evaluate every Nth frame (pose frozen in
        // between) via a per-component countdown, phase-seeded by entity id so a crowd spreads
        // across frames. Returns false on skipped frames; skipped time stays in Mesh.PendingAnimTime
        // so the evaluating frame consumes one larger step and playback speed is unaffected.
        // (A shared frame counter breaks here: with several worlds ticking, one world only ever sees
        // every-Nth counter values and entities could permanently miss their evaluation slot.)
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
                // First reduced-rate frame: phase by entity id, then settle into the countdown.
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

        // Meshes farther than this (distance / bounding radius) animate the skeleton's low-detail
        // bone prefix; closer ones animate every bone.
        constexpr float kBoneLODDistanceOverRadius = 60.0f;

        // Skeleton LOD bone count for this frame. Bones are parents-first, so any prefix is a valid
        // partial hierarchy; dropped bones hold bind-pose locals (full FK keeps attachments valid).
        // Authored-only: bone order past the parents-first guarantee is importer-dependent, so an
        // automatic cut can freeze visually important bones -- a skeleton animates every bone unless
        // its LowDetailBoneCount says otherwise.
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

        // One entity's single-clip update (parallel-safe: touches only this entity's components).
        // Advances playback + decides root motion, then records a one-task recipe; no pose math here.
        void UpdateSimple(SSimpleAnimationComponent& Anim, SSkeletalMeshComponent& Mesh, entt::entity Entity,
                          float DeltaTime, double Now)
        {
            Anim.NotifyEvents.clear();

            if (Mesh.VisibilityBasedAnimTick == EAnimUpdateMode::TickWhenRendered &&
                (Now - Mesh.LastRenderedTime) > kAnimVisibilityGrace)
            {
                Anim.bAdvancedThisFrame = false;
                return;
            }

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

            // Update-rate gate; a fresh PlayAnimation (bDirty) always evaluates immediately.
            Mesh.PendingAnimTime += DeltaTime;
            if (!ShouldEvaluateThisFrame(Mesh, Entity, Anim.bDirty))
            {
                Anim.bAdvancedThisFrame = false;
                return;
            }
            const float StepTime = Mesh.PendingAnimTime;
            Mesh.PendingAnimTime = 0.0f;

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

            // Notify dispatch: point crossings while advancing (the accumulated step spans any
            // update-rate-skipped frames), notify-state windows diffed against last frame's set.
            // A stopped clip (bDirty, not advancing) ends every active state so effects never leak.
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
                    FAnimNotifyEvent& Event = Anim.NotifyEvents.emplace_back();
                    Event.Name  = States[StateIdx].NotifyName;
                    Event.Track = States[StateIdx].NotifyTrack;
                    Event.Type  = Type;
                };

                for (int32 Idx : NowActive)
                {
                    if (eastl::find(Anim.ActiveNotifyStates.begin(), Anim.ActiveNotifyStates.end(), Idx) ==
                        Anim.ActiveNotifyStates.end())
                    {
                        EmitState(Idx, EAnimNotifyEventType::Begin);
                    }
                    EmitState(Idx, EAnimNotifyEventType::Tick);
                }
                for (int32 Idx : Anim.ActiveNotifyStates)
                {
                    if (Idx >= 0 && Idx < (int32)States.size() &&
                        eastl::find(NowActive.begin(), NowActive.end(), Idx) == NowActive.end())
                    {
                        EmitState(Idx, EAnimNotifyEventType::End);
                    }
                }
                Anim.ActiveNotifyStates.assign(NowActive.begin(), NowActive.end());
            }

            const int32 RootIdx = RootMotion::ResolveRootBoneIndex(Skeleton, Asset->RootBoneName);

            const bool bLock = (Anim.RootMotionLock == ERootMotionLockMode::ForceLock) ||
                               (Anim.RootMotionLock == ERootMotionLockMode::FromAsset && Asset->bLockRootMotion);
            const bool bExtract = !bLock && Asset->bEnableRootMotion;

            Anim.PendingRootMotion.bHasMotion = false;

            FAnimTaskList& Tasks = Mesh.AnimTasks;
            Tasks.Reset();
            Tasks.Skeleton        = Skeleton;
            Tasks.ActiveBoneCount = ComputeActiveBoneCount(Mesh, Skeleton);

            FAnimTask Sample;
            Sample.Type = EAnimTaskType::SampleClip;
            Sample.Clip = Asset;
            Sample.Time = Anim.CurrentTime;
            Tasks.OutputTask = Tasks.Add(Sample);

            if (RootIdx != INDEX_NONE)
            {
                if (bLock)
                {
                    Tasks.bLockRoot     = true;
                    Tasks.RootBoneIndex = RootIdx;
                }
                else if (bExtract && Anim.bAdvancedThisFrame)
                {
                    // Skip on seek/stop frames (handled by bAdvancedThisFrame) so scrubbing doesn't teleport
                    // the entity. The root is stripped from the final pose so the mesh stays centered.
                    Anim.PendingRootMotion = RootMotion::ExtractRootDelta(
                        Asset, Skeleton, RootIdx, Anim.PreviousTime, Anim.CurrentTime, Anim.bLooping, Duration);
                    Tasks.bLockRoot     = true;
                    Tasks.RootBoneIndex = RootIdx;
                }
            }

            Anim.bDirty = false;
        }

        // One entity's animation-graph update (parallel-safe: touches only this entity's components).
        // Runs graph logic and records the pose recipe; pose math happens in the execute phase.
        void UpdateGraph(const FSystemContext& SystemContext, entt::entity Entity,
                         SAnimationGraphComponent& AnimGraph, SSkeletalMeshComponent& Mesh,
                         float DeltaTime, double Now)
        {
            AnimGraph.NotifyEvents.clear();
            AnimGraph.PendingRootMotion = FRootMotionDelta();

            if (Mesh.VisibilityBasedAnimTick == EAnimUpdateMode::TickWhenRendered &&
                (Now - Mesh.LastRenderedTime) > kAnimVisibilityGrace)
            {
                return;
            }

            if (!AnimGraph.Graph.IsValid())
            {
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

            // Update-rate gate: skipped frames keep the previous pose; the next evaluation consumes
            // the accumulated step so clocks and transitions stay on time.
            Mesh.PendingAnimTime += DeltaTime;
            if (!ShouldEvaluateThisFrame(Mesh, Entity, false))
            {
                return;
            }
            const float StepTime = Mesh.PendingAnimTime;
            Mesh.PendingAnimTime = 0.0f;

            // Init VM state first so BuildTasks won't re-init and wipe the written values.
            if (SBlackboardComponent* Blackboard = SystemContext.TryGet<SBlackboardComponent>(Entity))
            {
                Blackboard->EnsureInitialized();
                AnimGraph.EnsureStateInitialized();

                const int32 NumParams = (int32)Graph->Parameters.size();
                for (int32 i = 0; i < NumParams && i < (int32)AnimGraph.VMState.Parameters.size(); ++i)
                {
                    const FAnimGraphParameter& Param = Graph->Parameters[i];
                    AnimGraph.VMState.Parameters[i] = Blackboard->GetFloat(Param.Name, Param.DefaultValue);
                }
            }

            FAnimGraphRootMotion GraphRootMotion;
            GraphRootMotion.Mode          = AnimGraph.RootMotionLock;
            GraphRootMotion.RootBoneIndex = RootMotion::ResolveRootBoneIndex(Skeleton, FName());

            FAnimationGraphVM::BuildTasks(Graph, Skeleton, StepTime, AnimGraph.VMState, Mesh.AnimTasks,
                                          GraphRootMotion, &AnimGraph.NotifyEvents);
            Mesh.AnimTasks.ActiveBoneCount = ComputeActiveBoneCount(Mesh, Skeleton);

            if (CVarDumpGraphTasks.GetValue())
            {
                static const char* TaskTypeNames[] =
                {
                    "RefPose", "SampleClip", "Blend", "BlendMasked", "MakeAdditive",
                    "ApplyAdditive", "SMOutput", "BoneTransform", "TwoBoneIK",
                };

                FString Dump;
                Dump.append_sprintf("[AnimTasks] entity %u output=%d",
                                    (uint32)entt::to_integral(Entity), (int32)Mesh.AnimTasks.OutputTask);
                for (SIZE_T t = 0; t < Mesh.AnimTasks.Tasks.size(); ++t)
                {
                    const FAnimTask& Task = Mesh.AnimTasks.Tasks[t];
                    Dump.append_sprintf("\n  [%u] %-13s depA=%-3d depB=%-3d alpha=%.3f",
                                        (uint32)t, TaskTypeNames[(int32)Task.Type],
                                        (int32)Task.DepA, (int32)Task.DepB, Task.Alpha);
                    if (Task.Type == EAnimTaskType::SampleClip)
                    {
                        Dump.append_sprintf(" clip=%s time=%.4f",
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
                        Dump.append_sprintf(" mask=%d/%d bones", NumWeighted, (int32)Task.MaskWeights->size());
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

        auto SimpleView = SystemContext.CreateView<SSimpleAnimationComponent, SSkeletalMeshComponent>(entt::exclude<SDisabledTag>);
        auto GraphView  = SystemContext.CreateView<SAnimationGraphComponent, SSkeletalMeshComponent>(entt::exclude<SDisabledTag>);
        auto MeshView   = SystemContext.CreateView<SSkeletalMeshComponent>(entt::exclude<SDisabledTag>);

        auto SimpleHandle = SimpleView.handle();
        auto GraphHandle  = GraphView.handle();
        auto MeshHandle   = MeshView.handle();

        const bool bHasSimple = SimpleHandle != nullptr && !SimpleHandle->empty();
        const bool bHasGraph  = GraphHandle != nullptr && !GraphHandle->empty();

        if ((!bHasSimple && !bHasGraph) || MeshHandle == nullptr)
        {
            return;
        }

        const float  DeltaTime = (float)SystemContext.GetDeltaTime();
        const double Now       = SystemContext.GetTime();

        FTaskGraph TaskGraph;

        // Update phase: record each entity's pose recipe. The graph pass runs after the simple pass so
        // an entity carrying both components gets the graph's recipe deterministically (they share the
        // mesh's task list; previously this was a write race on BoneTransforms).
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
                    UpdateSimple(SimpleView.get<SSimpleAnimationComponent>(Entity),
                                 SimpleView.get<SSkeletalMeshComponent>(Entity), Entity, DeltaTime, Now);
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
                    UpdateGraph(SystemContext, Entity,
                                GraphView.get<SAnimationGraphComponent>(Entity),
                                GraphView.get<SSkeletalMeshComponent>(Entity), DeltaTime, Now);
                }
            });

            if (SimpleUpdate.IsValid())
            {
                TaskGraph.AddDependency(GraphUpdate, SimpleUpdate);
            }
        }

        // Execute phase: one pass over every skeletal mesh runs the recorded recipes into skinning
        // matrices (each mesh has at most one recipe, so no entity is touched twice). Only chains
        // reachable from the output task run; per-worker pose pools recycle buffers across entities.
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

                    // Debug capture (anim graph editor's task view): armed for at most one component,
                    // so this is a null atomic compare for every other mesh.
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

                    // Reference evaluation the pre-refactor way; any divergence is a playback-logic
                    // bug, agreement means the animation data itself is wrong.
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

                    // Pack for the GPU here (parallel, only for poses that actually evaluated) so the
                    // render gather bulk-copies instead of converting per bone on its critical path.
                    SkeletalUtils::PackRenderBones(Mesh.BoneTransforms, Mesh.RenderBones);
                    Mesh.bRenderBonesDirty = false;
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

        TaskGraph.Dispatch();
        TaskGraph.Wait();

        // Serial: applying root motion marks the transform dirty (a structural registry change, not
        // ParallelFor-safe).
        auto&& TransformStorage = SystemContext.GetStorage<STransformComponent>();

        const auto ApplyRootMotion = [&](entt::entity Entity, FRootMotionDelta& Delta)
        {
            if (!Delta.bHasMotion)
            {
                return;
            }
            Delta.bHasMotion = false;

            // Root-motion-driven branches stay tagged even on paused frames; an identity delta
            // shouldn't dirty the transform.
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
            for (size_t i = 0; i < SimpleHandle->size(); ++i)
            {
                entt::entity Entity = (*SimpleHandle)[i];
                if (SimpleView.contains(Entity))
                {
                    ApplyRootMotion(Entity, SimpleView.get<SSimpleAnimationComponent>(Entity).PendingRootMotion);
                }
            }
        }

        if (bHasGraph)
        {
            for (size_t i = 0; i < GraphHandle->size(); ++i)
            {
                entt::entity Entity = (*GraphHandle)[i];
                if (GraphView.contains(Entity))
                {
                    ApplyRootMotion(Entity, GraphView.get<SAnimationGraphComponent>(Entity).PendingRootMotion);
                }
            }
        }
    }
}
