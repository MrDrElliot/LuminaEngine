#include "RuntimePCH.h"
#include "AnimationSystem.h"
#include "KinematicsSystem.h"
#include "World/ECS/Registry.h"

#include "Core/Console/ConsoleVariable.h"

#include "Animation/AnimationGraphVM.h"
#include "Animation/BoneLOD.h"
#include "Platform/Time/PlatformTime.h"
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
#include "World/World.h"
#include "World/WorldTypes.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"
#include "Containers/StringFormat.h"
#include "Core/Math/Hash/Hash.h"

namespace Lumina
{
    // Slack so brief occlusion or culling flicker does not stutter the pose.
    static constexpr double kAnimVisibilityGrace = 0.25;

    namespace
    {
        // Names are the only thing two skeletons reliably share, so the map is built from them once.
        void BuildFollowerBoneMap(SFollowerPoseComponent& Follower, const FSkeletonResource& LeaderSkeleton,
                                  const FSkeletonResource& FollowerSkeleton)
        {
            const int32 NumFollowerBones = FollowerSkeleton.GetNumBones();

            Follower.BoneMap.assign(NumFollowerBones, Constants::kIndexNone);
            Follower.BindFixups.assign(NumFollowerBones, FMatrix4(1.0f));

            int32 NumMatched = 0;
            for (int32 i = 0; i < NumFollowerBones; ++i)
            {
                const int32 LeaderIndex = LeaderSkeleton.FindBoneIndex(FollowerSkeleton.GetBone(i).Name);
                if (LeaderIndex == Constants::kIndexNone)
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

        // A leader that is hidden or culled still animates while a follower is drawn, at the closest follower's detail.
        template<typename TFollowerView>
        void RaiseLeaderDemand(ECS::FRegistry& Registry, TFollowerView& FollowerView, double Now)
        {
            THashSet<ECS::FEntity> TakenFromFollowers;
            FollowerView.ForEachInRange(0, (uint32)FollowerView.NumDenseSlots(),
                [&](ECS::FEntity Entity, SFollowerPoseComponent&, SSkeletalMeshComponent& Mesh)
            {
                if (Now - Mesh.LastRenderedTime > kAnimVisibilityGrace)
                {
                    return;
                }

                const ECS::FEntity LeaderEntity = SkeletalUtils::ResolvePoseLeader(Registry, Entity);
                SSkeletalMeshComponent* Leader = LeaderEntity != ECS::NullEntity ? Registry.TryGet<SSkeletalMeshComponent>(LeaderEntity) : nullptr;
                if (Leader == nullptr)
                {
                    return;
                }

                Leader->FollowerRenderedTime = Math::Max(Leader->FollowerRenderedTime, Mesh.LastRenderedTime);

                // A drawn leader's own stamps are current, and the followers sit on it anyway.
                if (Now - Leader->LastRenderedTime <= kAnimVisibilityGrace)
                {
                    return;
                }

                // Zero means full rate and every bone, so the smaller value is always the more detailed one.
                if (TakenFromFollowers.insert(LeaderEntity).second)
                {
                    Leader->LastDistanceOverRadius = Mesh.LastDistanceOverRadius;
                    Leader->LastBoneLODTolerance   = Mesh.LastBoneLODTolerance;
                }
                else
                {
                    Leader->LastDistanceOverRadius = Math::Min(Leader->LastDistanceOverRadius, Mesh.LastDistanceOverRadius);
                    Leader->LastBoneLODTolerance   = Math::Min(Leader->LastBoneLODTolerance, Mesh.LastBoneLODTolerance);
                }
            });
        }
    }

    // Time advance, VM state and lazy init all mutate these, so they are declared Write, not Read.
    void SAnimationSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RequireUpdate(EUpdateStage::Paused);
        RunsAfter<SKinematicsSystem>();
        Writes<SSkeletalMeshComponent, STransformComponent, SSimpleAnimationComponent, SAnimationGraphComponent, SFollowerPoseComponent>();
        Reads<SCharacterMovementComponent, SystemResource::Hierarchy, SystemResource::PhysicsQuery, SystemResource::Kinematics>();
        SendsEvent<SAnimMontageStartedEvent, SAnimMontageBlendingOutEvent, SAnimMontageEndedEvent>();
    }

    // Diagnostic cross-check of the deferred executor against direct single-clip sampling.
    static TConsoleVar<bool> CVarValidateAnimTasks(
        "anim.ValidateTasks",
        false,
        "Re-evaluate single-clip animation recipes directly and compare against the task executor's skinning matrices; logs mismatches.");

    static TConsoleVar<bool> CVarFuseAnimExecute(
        "anim.FuseExecute",
        true,
        "Execute each mesh's pose in the pass that built its recipe, instead of in a separate pass after every recipe is built.");

    // The one switch that answers "is budgeting what I am looking at" without editing a component.
    static TConsoleVar<bool> CVarAnimBudget(
        "anim.Budget",
        true,
        "Throttle animation by distance and visibility: update-rate skipping, low-detail bone LOD, and the "
        "off-screen pose freeze. Off evaluates every skeleton at full rate with every bone, every frame.");

    static TConsoleVar<bool> CVarBoneLOD(
        "anim.BoneLOD",
        true,
        "Animate only the bones whose motion would show at the size the mesh renders, holding the rest at their bind pose.");

    static TConsoleVar<bool> CVarBoneLODLog(
        "anim.BoneLOD.Log",
        false,
        "Log about once a second how many bones the evaluated skeletons animated out of how many they have.");

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
            if (!Mesh.bUpdateRateOptimization || bForce || !CVarAnimBudget.GetValue())
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
                (Now - Math::Max(Mesh.LastRenderedTime, Mesh.FollowerRenderedTime)) > kAnimVisibilityGrace &&
                CVarAnimBudget.GetValue())
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

        // Zero means every bone.
        int32 ResolveActiveBoneCount(const SSkeletalMeshComponent& Mesh, const BoneLOD::FBoneLODLayout& Layout, int32 NumBones)
        {
            if (!CVarAnimBudget.GetValue() || Mesh.LastBoneLODTolerance <= 0.0f)
            {
                return 0;
            }
            const int32 Active = BoneLOD::CountActiveBones(Layout.Errors, NumBones, Mesh.LastBoneLODTolerance, 1);
            return Active < NumBones ? Active : 0;
        }

        struct FBoneLODTally
        {
            std::atomic<int64>  ExecuteCycles{ 0 };
            std::atomic<int64>  Executions{ 0 };
            std::atomic<int64>  Meshes{ 0 };
            std::atomic<int64>  Cut{ 0 };
            std::atomic<int64>  ActiveBones{ 0 };
            std::atomic<int64>  TotalBones{ 0 };
            std::atomic<double> LastReport{ 0.0 };
        };

        FBoneLODTally GBoneLODTally;

        void ReportBoneLOD(int32 Active, int32 NumBones)
        {
            GBoneLODTally.Meshes.fetch_add(1, std::memory_order_relaxed);
            GBoneLODTally.Cut.fetch_add(Active > 0 ? 1 : 0, std::memory_order_relaxed);
            GBoneLODTally.ActiveBones.fetch_add(Active > 0 ? Active : NumBones, std::memory_order_relaxed);
            GBoneLODTally.TotalBones.fetch_add(NumBones, std::memory_order_relaxed);

            constexpr double kReportSeconds = 1.0;
            const double Now = PlatformTime::Seconds();
            double Last = GBoneLODTally.LastReport.load(std::memory_order_relaxed);
            if (Now - Last < kReportSeconds || !GBoneLODTally.LastReport.compare_exchange_strong(Last, Now, std::memory_order_relaxed))
            {
                return;
            }

            const int64 Meshes = GBoneLODTally.Meshes.exchange(0, std::memory_order_relaxed);
            const int64 Cut    = GBoneLODTally.Cut.exchange(0, std::memory_order_relaxed);
            const int64 Bones  = GBoneLODTally.ActiveBones.exchange(0, std::memory_order_relaxed);
            const int64 Total  = GBoneLODTally.TotalBones.exchange(0, std::memory_order_relaxed);
            const int64 Cycles = GBoneLODTally.ExecuteCycles.exchange(0, std::memory_order_relaxed);
            const int64 Runs   = GBoneLODTally.Executions.exchange(0, std::memory_order_relaxed);
            const double ExecuteUs = Runs > 0 ? PlatformTime::ToSeconds((uint64)Cycles) * 1e6 / (double)Runs : 0.0;
            LOG_INFO("[BoneLOD] {} skeleton evaluations, {} cut, {} of {} bones animated ({:.1f}%), {:.2f} us to execute each",
                     Meshes, Cut, Bones, Total, Total > 0 ? 100.0 * (double)Bones / (double)Total : 100.0, ExecuteUs);
        }

        // Evaluates the recipe in the mesh's bone LOD order, kept whatever the distance so pose history never changes layout mid-blend.
        void ApplyBoneLOD(const SSkeletalMeshComponent& Mesh, FSkeletonResource* Skeleton, FAnimTaskList& Tasks)
        {
            const CSkeletalMesh* Asset = Mesh.SkeletalMesh.IsValid() ? Mesh.SkeletalMesh.Get() : nullptr;
            const BoneLOD::FBoneLODLayout* Layout = (CVarBoneLOD.GetValue() && Asset != nullptr) ? Asset->GetBoneLODLayout(Skeleton) : nullptr;
            const int32 NumBones = Skeleton->GetNumBones();
            if (Layout == nullptr)
            {
                if (CVarBoneLODLog.GetValue())
                {
                    ReportBoneLOD(0, NumBones);
                }
                return;
            }

            if (Layout->PoseSkeleton)
            {
                Tasks.Skeleton    = Layout->PoseSkeleton.get();
                Tasks.PoseToAsset = Layout->PoseToAsset.data();
                Tasks.AssetToPose = Layout->AssetToPose.data();
            }
            Tasks.ActiveBoneCount = ResolveActiveBoneCount(Mesh, *Layout, NumBones);

            if (CVarBoneLODLog.GetValue())
            {
                ReportBoneLOD(Tasks.ActiveBoneCount, NumBones);
            }
        }

        FSkeletonResource* ResolveSkeleton(SSkeletalMeshComponent& Mesh)
        {
            if (!Mesh.SkeletalMesh.IsValid())
            {
                return nullptr;
            }
            CSkeletalMesh* SkelMesh = Mesh.SkeletalMesh.Get();
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
                    Event.Source    = Asset;
                    Event.Alpha     = Span > 0.0f ? Math::Clamp((Anim.CurrentTime - Authored.StartTime) / Span, 0.0f, 1.0f) : 0.0f;
                };

                for (int32 Idx : NowActive)
                {
                    if (!Algo::Contains(Anim.ActiveNotifyStates, Idx))
                    {
                        EmitState(Idx, EAnimNotifyEventType::Begin);
                    }
                    EmitState(Idx, EAnimNotifyEventType::Tick);
                }
                for (int32 Idx : Anim.ActiveNotifyStates)
                {
                    if (Idx >= 0 && Idx < (int32)States.size() &&
                        !Algo::Contains(NowActive, Idx))
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
                : Constants::kIndexNone;

            Anim.PendingRootMotion.bHasMotion = false;

            FAnimTaskList& Tasks = Mesh.AnimTasks;
            Tasks.Reset();
            Tasks.Skeleton        = Skeleton;
            ApplyBoneLOD(Mesh, Skeleton, Tasks);

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

            if (RootIdx != Constants::kIndexNone)
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

        void SendMontageEvents(const SSystemContext& Context, ECS::FEntity Entity, FAnimMontagePlayer& Montages)
        {
            for (const FAnimMontageTransition& Transition : Montages.Transitions)
            {
                switch (Transition.Type)
                {
                case EAnimMontageTransition::Started:
                    Context.SendEvent(SAnimMontageStartedEvent{ Entity, Transition.Montage });
                    break;
                case EAnimMontageTransition::BlendingOut:
                    Context.SendEvent(SAnimMontageBlendingOutEvent{ Entity, Transition.Montage, Transition.bInterrupted });
                    break;
                case EAnimMontageTransition::Ended:
                    Context.SendEvent(SAnimMontageEndedEvent{ Entity, Transition.Montage, Transition.bInterrupted });
                    break;
                }
            }
            Montages.Transitions.clear();
        }

        // One entity's graph update, parallel-safe; pose math happens in the execute phase.
        void UpdateGraph(const SSystemContext& SystemContext, ECS::FEntity Entity,
                         SAnimationGraphComponent& AnimGraph, SSkeletalMeshComponent& Mesh,
                         float DeltaTime, double Now, const FKinematicsState* KinematicsState)
        {
            AnimGraph.NotifyEvents.clear();
            AnimGraph.PendingRootMotion = FRootMotionDelta();
            DeltaTime = AnimGraph.bPlaying ? DeltaTime * AnimGraph.PlaybackSpeed : 0.0f;

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
                // Computed rather than cached, since the capsule this mesh hangs from moved earlier this frame and a stale pose slips the feet.
                const VTransform World = Transform->GetWorldTransform();

                SceneContext.Scene          = SystemContext.GetPhysicsScene();
                SceneContext.BoneTransforms = &Mesh.BoneTransforms;
                SceneContext.WorldLocation  = World.GetLocation();
                SceneContext.WorldScale     = World.GetScale();
                SceneContext.WorldRotation  = World.GetRotation();
                SceneContext.SelfEntity     = Entity;

                // A mesh parented under its character has no body; the capsule it must not trace is the parent's.
                const ECS::FEntity Parent = SystemContext.GetHierarchy().GetParent(Entity);
                if (SceneContext.Scene != nullptr && SceneContext.Scene->GetBodyStatus(Entity) == Physics::EPhysicsBodyStatus::Missing && Parent != ECS::NullEntity)
                {
                    SceneContext.SelfEntity = Parent;
                }

                SceneContext.Velocity = Kinematics::GetVelocity(KinematicsState, Entity);
            }

            FAnimationGraphVM::BuildTasks(Graph, Skeleton, StepTime, AnimGraph.VMState, Mesh.AnimTasks,
                                          GraphRootMotion, &AnimGraph.NotifyEvents, &AnimGraph.Montages,
                                          &SceneContext);
            ApplyBoneLOD(Mesh, Skeleton, Mesh.AnimTasks);

            if (CVarDumpGraphTasks.GetValue())
            {
                static const char* TaskTypeNames[] =
                {
                    "RefPose", "SampleClip", "Blend", "BlendMasked", "MakeAdditive",
                    "ApplyAdditive", "Inertialize", "DeadBlend", "SaveSnapshot", "LoadSnapshot",
                    "BoneTransform", "TwoBoneIK", "FABRIK", "LookAt", "FootIK", "TranslateBone",
                };
                static_assert(std::size(TaskTypeNames) == (SIZE_T)EAnimTaskType::TranslateBone + 1);

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

        // Each mesh carries at most one recipe, so this runs once per mesh whichever pass produced the recipe.
        void ExecuteMeshRecipe(SSkeletalMeshComponent& Mesh)
        {
            if (!Mesh.AnimTasks.HasWork())
            {
                return;
            }

            // The same recipe rebuilds the same pose unless something else wrote it, so skipping keeps the serial and the gather still.
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
            int32  ValidateRoot = Constants::kIndexNone;
            // The executor hands back asset order, so the reference samples the asset skeleton.
            FSkeletonResource* ValidateSkeleton = ResolveSkeleton(Mesh);
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
            const uint64 ExecuteStart = CVarBoneLODLog.GetValue() ? PlatformTime::Cycles() : 0;
            const bool bProduced = Anim::ExecuteTaskList(Mesh.AnimTasks, PoseScratch, Snapshot);
            if (ExecuteStart != 0)
            {
                GBoneLODTally.ExecuteCycles.fetch_add((int64)(PlatformTime::Cycles() - ExecuteStart), std::memory_order_relaxed);
                GBoneLODTally.Executions.fetch_add(1, std::memory_order_relaxed);
            }

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
                if (bValidateLock && ValidateRoot != Constants::kIndexNone)
                {
                    RootMotion::PinRootToBindPose(RefPose, ValidateSkeleton, ValidateRoot);
                }
                AnimPose::ToSkinningMatrices(RefPose, ValidateSkeleton, RefMatrices);

                float MaxDiff = 0.0f;
                int32 WorstBone = Constants::kIndexNone;
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

            // Any other writer leaves the serial or the dirty flag moved, so only the animation system's own last pose is ever compared by hash.
            const uint64 PoseHash = bProduced ? Hash::GetHash64(PoseScratch.data(), PoseScratch.size() * sizeof(FMatrix4)) : 0ull;
            const bool bPoseChanged = bProduced
                && (PoseScratch.size() != Mesh.BoneTransforms.size()
                || Mesh.bRenderBonesDirty
                || Mesh.PackedPoseSerial != Mesh.PoseSerial
                || PoseHash != Mesh.PoseHash);

            if (bPoseChanged)
            {
                // Streamed rather than swapped, so the scratch stays cached and the component's cold lines are never read for ownership.
                Mesh.BoneTransforms.resize(PoseScratch.size());
                Memory::MemcpyToWriteCombined(Mesh.BoneTransforms.data(), PoseScratch.data(), PoseScratch.size() * sizeof(FMatrix4));

                // Packed here while the scratch is in cache, so the gather copies half the bytes and never reads the matrices.
                const uint32 NumBones = (uint32)PoseScratch.size();
                Mesh.PackedBones.resize((SIZE_T)NumBones * 2u);
                SkeletalUtils::PackRenderBones(PoseScratch.data(), NumBones, reinterpret_cast<FBoneTransform*>(Mesh.PackedBones.data()));

                // A stale external write is superseded by the pose that just replaced it.
                Mesh.bRenderBonesDirty = false;

                ++Mesh.PoseSerial;
                Mesh.PackedPoseSerial = Mesh.PoseSerial;
                Mesh.PoseHash         = PoseHash;
            }
        }
    }


    void SAnimationSystem::OnUpdate(const SSystemContext& Context)
    {

        LUMINA_PROFILE_SCOPE();

        auto SimpleView   = Context.CreateView<SSimpleAnimationComponent, SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag, SFollowerPoseComponent>{});
        auto GraphView    = Context.CreateView<SAnimationGraphComponent, SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag, SFollowerPoseComponent>{});
        auto MeshView     = Context.CreateView<SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag>{});
        auto FollowerView = Context.CreateView<SFollowerPoseComponent, SSkeletalMeshComponent>(ECS::TExclude<SDisabledTag>{});

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
        const FKinematicsState* KinematicsState = Kinematics::GetState(Context);

        // A paused game holds its poses, while editor worlds run this stage to preview animation unless the world turned that off.
        const bool bPreviewing = Context.GetWorldType() == EWorldType::Editor && Context.GetWorld()->AnimatesWhilePaused();
        const bool   bFrozen   = Context.GetUpdateStage() == EUpdateStage::Paused && !bPreviewing;
        const float  DeltaTime = bFrozen ? 0.0f : (float)Context.GetDeltaTime();
        const double Now       = Context.GetTime();

        // Relaxed throughout, since the TaskGraph::Wait() before the read is the ordering edge.
        std::atomic<bool> bAnyRootMotion{ false };

        if (bHasFollower)
        {
            RaiseLeaderDemand(Context.GetRegistry(), FollowerView, Now);
        }

        NotifyQueue.Prepare();

        const bool bFuseExecute = CVarFuseAnimExecute.GetValue();
        auto GraphStorage = Context.GetStorage<SAnimationGraphComponent>();

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
                    if (bFuseExecute && !(GraphStorage.IsValid() && GraphStorage.Contains(Entity)))
                    {
                        ExecuteMeshRecipe(Mesh);
                    }

                    if (!Anim.NotifyEvents.empty())
                    {
                        NotifyQueue.Record(Entity, EAnimNotifyPass::Simple, Anim.NotifyEvents);
                    }

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
                    UpdateGraph(Context, Entity, AnimGraph, Mesh, DeltaTime, Now, KinematicsState);
                    SendMontageEvents(Context, Entity, AnimGraph.Montages);

                    // Run while this mesh is hot, instead of after a barrier that waits for every graph.
                    if (bFuseExecute)
                    {
                        ExecuteMeshRecipe(Mesh);
                    }

                    if (!AnimGraph.NotifyEvents.empty())
                    {
                        NotifyQueue.Record(Entity, EAnimNotifyPass::Graph, AnimGraph.NotifyEvents);
                    }

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
                [&](ECS::FEntity, SSkeletalMeshComponent& Mesh)
            {
                ExecuteMeshRecipe(Mesh);
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
            ECS::FRegistry& Registry = Context.GetRegistry();

            const FTaskGraph::FNodeHandle CopyFollowers = TaskGraph.AddParallelFor((uint32)FollowerView.NumDenseSlots(), 16, [&](const Task::FParallelRange& Range)
            {
                FollowerView.ForEachInRange(Range.Start, Range.End,
                    [&](ECS::FEntity Entity, SFollowerPoseComponent& Follower, SSkeletalMeshComponent& Mesh)
                {


                    Follower.bSharesLeaderPose = false;

                    const ECS::FEntity LeaderEntity = SkeletalUtils::ResolvePoseLeader(Registry, Entity);
                    if (LeaderEntity == ECS::NullEntity)
                    {
                        return;
                    }

                    const SSkeletalMeshComponent* LeaderMesh = Registry.TryGet<SSkeletalMeshComponent>(LeaderEntity);
                    if (LeaderMesh == nullptr || LeaderMesh->BoneTransforms.empty())
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

                    // Same leader, same skeletons and an unmoved pose, so the copy already on this mesh still stands.
                    if (Follower.CopiedLeader == LeaderEntity && Follower.LastLeaderPoseSerial == LeaderMesh->PoseSerial &&
                        Follower.MappedLeaderSkeleton == LeaderSkeleton && Follower.MappedFollowerSkeleton == FollowerSkeleton &&
                        !Mesh.BoneTransforms.empty())
                    {
                        Follower.bSharesLeaderPose = LeaderSkeleton == FollowerSkeleton;
                        return;
                    }

                    // One skeleton behind both meshes is the common case, and it is a straight copy.
                    if (LeaderSkeleton == FollowerSkeleton)
                    {
                        Mesh.BoneTransforms = LeaderMesh->BoneTransforms;
                        Follower.MappedLeaderSkeleton   = LeaderSkeleton;
                        Follower.MappedFollowerSkeleton = FollowerSkeleton;
                        Follower.bSharesLeaderPose      = true;
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

                    Follower.CopiedLeader = LeaderEntity;
                    Mesh.bRenderBonesDirty = false;
                    Follower.LastLeaderPoseSerial = LeaderMesh->PoseSerial;
                    ++Mesh.PoseSerial;
                                });
            });

            TaskGraph.AddDependency(CopyFollowers, Execute);
        }

        TaskGraph.Dispatch();
        TaskGraph.Wait();

        // Handlers run user code that may touch any component, so they wait for the flush after this batch.
        NotifyQueue.Submit(Context.GetCommandBus());

        // Nothing moved, so everything below is dead work.
        if (!bAnyRootMotion.load(std::memory_order_relaxed))
        {
            return;
        }

        // Serial because applying root motion marks the transform dirty, which is not ParallelFor-safe.
        auto TransformStorage = Context.GetStorage<STransformComponent>();

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
            for (auto&& [Entity, Anim] : Context.GetStorage<SSimpleAnimationComponent>().Each())
            {
                if (Anim.PendingRootMotion.bHasMotion)
                {
                    ApplyRootMotion(Entity, Anim.PendingRootMotion);
                }
            }
        }

        if (bHasGraph)
        {
            for (auto&& [Entity, AnimGraph] : Context.GetStorage<SAnimationGraphComponent>().Each())
            {
                if (AnimGraph.PendingRootMotion.bHasMotion)
                {
                    ApplyRootMotion(Entity, AnimGraph.PendingRootMotion);
                }
            }
        }
    }
}
