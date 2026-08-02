#include "RuntimePCH.h"
#include "AnimationGraphVM.h"

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Console/ConsoleVariable.h"
#include "Memory/Memcpy.h"
#include "Renderer/MeshData.h"
#include "Log/Log.h"

namespace Lumina
{
    // Diagnostic companion to anim.DumpGraphTasks: logs each AdvanceClock execution with the speed
    // it actually read from the bytecode, discriminating a stale/incorrect compiled constant from a
    // clock-advance bug.
    static TConsoleVar<bool> CVarDumpGraphClocks(
        "anim.DumpGraphClocks",
        false,
        "Log every animation graph AdvanceClock execution (state slot, speed register and value, resulting clock) each frame while enabled.");

    namespace Detail
    {
        // Linear cursor over the flat bytecode array. Operand widths are fixed
        // per opcode (see EAnimOp); the compiler and VM must agree on layout.
        struct FByteReader
        {
            const uint8* Data = nullptr;
            SIZE_T Size = 0;
            SIZE_T Cursor = 0;

            FORCEINLINE bool AtEnd() const { return Cursor >= Size; }

            template <typename T>
            FORCEINLINE T Read()
            {
                T Value{};
                if (Cursor + sizeof(T) <= Size)
                {
                    Memory::Memcpy(&Value, Data + Cursor, sizeof(T));
                }
                Cursor += sizeof(T);
                return Value;
            }
        };

        static FORCEINLINE bool EvalTransitionCondition(const FAnimGraphTransition& Transition,
                                                        const CAnimationGraph* Graph,
                                                        const TVector<float>& Parameters)
        {
            // Resolved at load/compile; the fallback covers graphs built outside those paths.
            const int32 ParamIdx = Transition.CachedParamIndex != FAnimGraphTransition::ParamUnresolved
                ? Transition.CachedParamIndex
                : Graph->FindParameterIndex(Transition.ConditionParameter);
            const float Value = (ParamIdx >= 0 && ParamIdx < (int32)Parameters.size())
                ? Parameters[ParamIdx]
                : 0.0f;

            switch (Transition.Compare)
            {
            case EAnimTransitionCompare::Greater:      return Value >  Transition.CompareValue;
            case EAnimTransitionCompare::GreaterEqual: return Value >= Transition.CompareValue;
            case EAnimTransitionCompare::Less:         return Value <  Transition.CompareValue;
            case EAnimTransitionCompare::LessEqual:    return Value <= Transition.CompareValue;
            case EAnimTransitionCompare::Equal:        return Value == Transition.CompareValue;
            case EAnimTransitionCompare::NotEqual:     return Value != Transition.CompareValue;
            }
            return false;
        }

        template <typename TEnum>
        static FORCEINLINE TEnum ReadEnumReg(const float* Scalars, SIZE_T NumScalar, uint16 Reg, int32 MaxValue)
        {
            const float Value = Reg < NumScalar ? Scalars[Reg] : 0.0f;
            const int32 Index = Math::Clamp((int32)Math::Round(Value), 0, MaxValue);
            return (TEnum)Index;
        }

        static FORCEINLINE float ApplyScalarOp(EAnimScalarOp Op, float A, float B)
        {
            switch (Op)
            {
            case EAnimScalarOp::Add:      return A + B;
            case EAnimScalarOp::Sub:      return A - B;
            case EAnimScalarOp::Mul:      return A * B;
            case EAnimScalarOp::Div:      return B != 0.0f ? A / B : 0.0f;
            case EAnimScalarOp::Min:      return Math::Min(A, B);
            case EAnimScalarOp::Max:      return Math::Max(A, B);
            case EAnimScalarOp::Clamp01:  return Math::Clamp(A, 0.0f, 1.0f);
            case EAnimScalarOp::OneMinus: return 1.0f - A;
            case EAnimScalarOp::Abs:      return Math::Abs(A);
            case EAnimScalarOp::Sin:      return Math::Sin(A);
            case EAnimScalarOp::Cos:      return Math::Cos(A);
            case EAnimScalarOp::Mod:      return B != 0.0f ? fmodf(A, B) : 0.0f;
            case EAnimScalarOp::Pow:      return Math::Pow(A, B);
            case EAnimScalarOp::Atan2:    return Math::Atan2(A, B);
            case EAnimScalarOp::Less:     return A < B ? 1.0f : 0.0f;
            case EAnimScalarOp::Greater:  return A > B ? 1.0f : 0.0f;
            case EAnimScalarOp::Floor:    return Math::Floor(A);
            case EAnimScalarOp::Ceil:     return Math::Ceil(A);
            case EAnimScalarOp::Frac:     return Math::Fract(A);
            case EAnimScalarOp::Sqrt:     return Math::Sqrt(Math::Max(A, 0.0f));
            case EAnimScalarOp::Negate:   return -A;
            case EAnimScalarOp::Sign:     return Math::Sign(A);
            }
            return 0.0f;
        }
    }

    void FAnimationGraphVM::InitState(const CAnimationGraph* Graph, FAnimGraphVMState& State)
    {
        if (Graph == nullptr)
        {
            State = FAnimGraphVMState();
            return;
        }

        State.ScalarRegisters.assign(Graph->NumScalarRegisters, 0.0f);
        State.StateSlots.assign(Graph->NumStateSlots, 0.0f);

        State.Parameters.resize(Graph->Parameters.size());
        for (SIZE_T i = 0; i < Graph->Parameters.size(); ++i)
        {
            State.Parameters[i] = Graph->Parameters[i].DefaultValue;
        }

        // Seed non-zero slot values (entry state index, From = -1); zeroed slots are already correct.
        const SIZE_T NumSlots = State.StateSlots.size();
        for (const FAnimGraphStateMachine& SM : Graph->StateMachines)
        {
            if (SM.CurrentStateSlot < NumSlots)
            {
                State.StateSlots[SM.CurrentStateSlot] = (float)SM.EntryState;
            }
            if (SM.FromStateSlot < NumSlots)
            {
                State.StateSlots[SM.FromStateSlot] = -1.0f;
            }
        }

        // One inertialization record per state machine (transition smoothing; rebuilt each init).
        State.Inertializers.assign(Graph->StateMachines.size(), FAnimInertializer());

        State.SyncGroups.assign(Graph->NumSyncGroups, FAnimSyncGroup());

        State.SourceGraph  = Graph;
        State.bInitialized = true;
    }

    void FAnimationGraphVM::BuildTasks(const CAnimationGraph* Graph, FSkeletonResource* Skeleton, float DeltaTime, FAnimGraphVMState& State, FAnimTaskList& OutTasks, FAnimGraphRootMotion& RootMotionInOut, TVector<FAnimNotifyEvent>* OutEvents)
    {
        LUMINA_PROFILE_SCOPE();

        OutTasks.Reset();
        RootMotionInOut.Delta = FRootMotionDelta();

        if (Graph == nullptr || Skeleton == nullptr || Skeleton->GetNumBones() == 0)
        {
            return;
        }

        // A program compiled against an older opcode layout would misparse operands into garbage
        // poses. Refuse it: bind pose + one warning per instance, until the graph is recompiled
        // (opening it in the editor auto-compiles; save to persist).
        if (Graph->BytecodeVersion != kAnimBytecodeVersion)
        {
            if (State.SourceGraph != Graph)
            {
                State.SourceGraph  = Graph;
                State.bInitialized = false;
                LOG_WARN("AnimationGraph '{}': compiled bytecode version {} does not match runtime version {}; "
                         "showing bind pose. Open and re-save the graph in the editor to recompile.",
                         Graph->GetName().c_str(), Graph->BytecodeVersion, kAnimBytecodeVersion);
            }

            FAnimTask Ref;
            Ref.Type            = EAnimTaskType::ReferencePose;
            OutTasks.Skeleton   = Skeleton;
            OutTasks.OutputTask = OutTasks.Add(Ref);
            return;
        }

        // Re-initialize when the state is stale or sized for a different graph asset.
        if (!State.bInitialized ||
            State.SourceGraph != Graph ||
            (int32)State.ScalarRegisters.size() != Graph->NumScalarRegisters ||
            (int32)State.StateSlots.size() != Graph->NumStateSlots ||
            (int32)State.SyncGroups.size() != Graph->NumSyncGroups ||
            State.Parameters.size() != Graph->Parameters.size())
        {
            InitState(Graph, State);
        }

        // Latch last update's blended durations and re-arm the shared phases for this update.
        for (FAnimSyncGroup& Group : State.SyncGroups)
        {
            if (Group.bAdvanced)
            {
                Group.Duration  = Group.NextDuration;
                Group.bAdvanced = false;
            }
        }

        OutTasks.Skeleton = Skeleton;

        const SIZE_T NumScalar = State.ScalarRegisters.size();
        const SIZE_T NumState  = State.StateSlots.size();
        const SIZE_T NumClips  = Graph->Clips.size();
        const SIZE_T NumParams = State.Parameters.size();
        const SIZE_T NumPose   = Graph->NumPoseRegisters;

        float* RESTRICT Scalars = State.ScalarRegisters.data();

        // Pose registers now hold task indices: writing a register records which task produces that
        // pose, reading one wires a dependency. Transient per call.
        thread_local TVector<int16> PoseTasks;
        PoseTasks.assign(NumPose, FAnimTask::NoTask);

        // Root-motion deltas and notify-event ranges flow alongside the registers: AdvanceClock tags
        // its clock scalar, SampleAnim adopts the tag onto its pose register, blends combine, the
        // state machine keeps only the target branch, Output reports whatever survived.
        struct FEventRange
        {
            uint16 Start = 0;
            uint16 End = 0;
            bool IsEmpty() const { return End <= Start; }
        };

        thread_local TVector<FRootMotionDelta> ClockDeltas;
        thread_local TVector<FRootMotionDelta> PoseDeltas;
        thread_local TVector<FEventRange> ClockEvents;
        thread_local TVector<FEventRange> PoseEvents;
        thread_local TVector<FAnimNotifyEvent> EventScratch;

        const bool bExtractRootMotion = RootMotionInOut.Mode == ERootMotionLockMode::FromAsset &&
                                        RootMotionInOut.RootBoneIndex != INDEX_NONE;

        ClockDeltas.assign(NumScalar, FRootMotionDelta());
        PoseDeltas.assign(NumPose, FRootMotionDelta());
        ClockEvents.assign(NumScalar, FEventRange());
        PoseEvents.assign(NumPose, FEventRange());
        EventScratch.clear();

        const auto UnionEvents = [](const FEventRange& A, const FEventRange& B) -> FEventRange
        {
            if (A.IsEmpty())
            {
                return B;
            }
            if (B.IsEmpty())
            {
                return A;
            }
            return FEventRange{ Math::Min(A.Start, B.Start), Math::Max(A.End, B.End) };
        };

        const auto ScaleEventWeights = [](const FEventRange& Range, float Mul)
        {
            for (uint16 i = Range.Start; i < Range.End && i < (uint16)EventScratch.size(); ++i)
            {
                EventScratch[i].Weight *= Mul;
            }
        };

        const auto ReadScalar = [&](uint16 Reg, float Default) -> float
        {
            return Reg < NumScalar ? Scalars[Reg] : Default;
        };

        const auto DeltaOf = [&](uint16 Reg) -> FRootMotionDelta
        {
            return Reg < NumPose ? PoseDeltas[Reg] : FRootMotionDelta();
        };

        const auto EventsOf = [&](uint16 Reg) -> FEventRange
        {
            return Reg < NumPose ? PoseEvents[Reg] : FEventRange();
        };

        const auto SetPoseTags = [&](uint16 Reg, const FRootMotionDelta& Delta, const FEventRange& Events)
        {
            if (Reg < NumPose)
            {
                PoseDeltas[Reg] = Delta;
                PoseEvents[Reg] = Events;
            }
        };

        // Sync-group provenance: which group (and clip duration) produced a register, so blends can
        // refine the group's blended duration from their alpha (consumed next update).
        struct FSyncTag
        {
            int32 Group = -1;
            float ClipDuration = 0.0f;
        };

        thread_local TVector<FSyncTag> ClockSync;
        thread_local TVector<FSyncTag> PoseSync;
        ClockSync.assign(NumScalar, FSyncTag());
        PoseSync.assign(NumPose, FSyncTag());

        const auto SyncOf = [&](uint16 Reg) -> FSyncTag
        {
            return Reg < NumPose ? PoseSync[Reg] : FSyncTag();
        };

        const auto SetPoseSync = [&](uint16 Reg, const FSyncTag& Tag)
        {
            if (Reg < NumPose)
            {
                PoseSync[Reg] = Tag;
            }
        };

        // Reading a never-written pose register wires a bind-pose leaf, so malformed graphs degrade
        // to the reference pose instead of blending garbage.
        const auto PoseTaskFor = [&](uint16 Reg) -> int16
        {
            if (Reg < NumPose && PoseTasks[Reg] != FAnimTask::NoTask)
            {
                return PoseTasks[Reg];
            }
            FAnimTask Ref;
            Ref.Type = EAnimTaskType::ReferencePose;
            return OutTasks.Add(Ref);
        };

        const auto SetPoseTask = [&](uint16 Reg, int16 TaskIdx)
        {
            if (Reg < NumPose)
            {
                PoseTasks[Reg] = TaskIdx;
            }
        };

        Detail::FByteReader Reader;
        Reader.Data = Graph->Bytecode.data();
        Reader.Size = Graph->Bytecode.size();

        while (!Reader.AtEnd())
        {
            const EAnimOp Op = (EAnimOp)Reader.Read<uint8>();

            switch (Op)
            {
            case EAnimOp::Halt:
            {
                Reader.Cursor = Reader.Size;
                break;
            }

            case EAnimOp::LoadConst:
            {
                const float Imm  = Reader.Read<float>();
                const uint16 Dst = Reader.Read<uint16>();
                if (Dst < NumScalar)
                {
                    Scalars[Dst] = Imm;
                }
                break;
            }

            case EAnimOp::LoadParam:
            {
                const uint16 ParamIdx = Reader.Read<uint16>();
                const uint16 Dst      = Reader.Read<uint16>();
                if (Dst < NumScalar)
                {
                    Scalars[Dst] = ParamIdx < NumParams ? State.Parameters[ParamIdx] : 0.0f;
                }
                break;
            }

            case EAnimOp::ScalarOp:
            {
                const EAnimScalarOp SubOp = (EAnimScalarOp)Reader.Read<uint8>();
                const uint16 A   = Reader.Read<uint16>();
                const uint16 B   = Reader.Read<uint16>();
                const uint16 Dst = Reader.Read<uint16>();
                if (A < NumScalar && B < NumScalar && Dst < NumScalar)
                {
                    Scalars[Dst] = Detail::ApplyScalarOp(SubOp, Scalars[A], Scalars[B]);
                }
                break;
            }

            case EAnimOp::AdvanceClock:
            {
                const uint16 StateIdx     = Reader.Read<uint16>();
                const uint16 SpeedReg     = Reader.Read<uint16>();
                const uint16 ClipIdx      = Reader.Read<uint16>();
                const uint16 LoopModeReg  = Reader.Read<uint16>();
                const uint16 DstClock     = Reader.Read<uint16>();
                const uint16 DstFinished  = Reader.Read<uint16>();
                const uint16 SyncGroup    = Reader.Read<uint16>();

                const EClipLoopMode Mode = Detail::ReadEnumReg<EClipLoopMode>(Scalars, NumScalar, LoopModeReg, (int32)EClipLoopMode::PlayOnce);

                if (StateIdx < NumState)
                {
                    CAnimation* Clip = (ClipIdx < NumClips && Graph->Clips[ClipIdx].IsValid())
                        ? Graph->Clips[ClipIdx].Get()
                        : nullptr;

                    const float PrevClock = State.StateSlots[StateIdx];
                    const float Speed = ReadScalar(SpeedReg, 1.0f);
                    float Clock = PrevClock + DeltaTime * Speed;
                    float PrevSampleTime = PrevClock;
                    float Finished = 0.0f;
                    bool bSynced = false;

                    const float Duration = Clip ? Clip->GetDuration() : 0.0f;

                    // A synced clip samples at the group's shared phase instead of its own clock, so
                    // every member of the group sits at the same stride phase. The phase advances once
                    // per update at the group's blended-duration rate; synced clips always loop.
                    if (SyncGroup != kAnimNoSyncGroup && SyncGroup < State.SyncGroups.size() && Duration > 0.0f)
                    {
                        FAnimSyncGroup& Group = State.SyncGroups[SyncGroup];
                        if (!Group.bAdvanced)
                        {
                            Group.bAdvanced    = true;
                            Group.PrevPhase    = Group.Phase;
                            Group.NextDuration = Duration;

                            const float GroupDuration = Group.Duration > 1e-4f ? Group.Duration : Duration;
                            Group.Phase += (DeltaTime * Speed) / GroupDuration;
                            Group.Phase -= Math::Floor(Group.Phase); // wrap 0..1, negative-safe
                        }

                        PrevSampleTime = Group.PrevPhase * Duration;
                        Clock          = Group.Phase * Duration;
                        bSynced        = true;
                    }
                    else if (Duration > 0.0f)
                    {
                        if (Mode == EClipLoopMode::Loop)
                        {
                            Clock = fmodf(Clock, Duration);
                            if (Clock < 0.0f)
                            {
                                Clock += Duration;
                            }
                        }
                        else // PlayOnce -- clamp at the end and signal finished.
                        {
                            if (Clock >= Duration)
                            {
                                Clock    = Duration;
                                Finished = 1.0f;
                            }
                            else if (Clock < 0.0f)
                            {
                                Clock = 0.0f;
                            }
                        }
                    }

                    if (CVarDumpGraphClocks.GetValue())
                    {
                        LOG_INFO("[AnimClock] state={} speedReg={} speed={} clip={} prev={} clock={} sync={}",
                                 StateIdx, SpeedReg, Speed,
                                 Clip != nullptr ? Clip->GetName().c_str() : "<null>",
                                 PrevClock, Clock, SyncGroup == kAnimNoSyncGroup ? -1 : (int32)SyncGroup);
                    }

                    State.StateSlots[StateIdx] = Clock;
                    if (DstClock < NumScalar)
                    {
                        Scalars[DstClock] = Clock;

                        const bool bLooping = bSynced || Mode == EClipLoopMode::Loop;

                        if (bSynced)
                        {
                            ClockSync[DstClock] = FSyncTag{ (int32)SyncGroup, Duration };
                        }

                        // Tag the clock scalar with this advance's root delta; SampleAnim adopts it
                        // onto the pose register it feeds. bHasMotion stays set even on a paused
                        // frame so the branch keeps reading as root-motion driven (stable pinning).
                        if (bExtractRootMotion && Clip != nullptr && Clip->bEnableRootMotion && !Clip->bLockRootMotion)
                        {
                            FRootMotionDelta ClipDelta;
                            if (Clock != PrevSampleTime)
                            {
                                ClipDelta = RootMotion::ExtractRootDelta(Clip, Skeleton, RootMotionInOut.RootBoneIndex,
                                                                         PrevSampleTime, Clock, bLooping, Duration);
                            }
                            ClipDelta.bHasMotion   = true;
                            ClockDeltas[DstClock] = ClipDelta;
                        }

                        // Point notifies crossed by this advance, tagged the same way.
                        if (OutEvents != nullptr && Clip != nullptr && Clock != PrevSampleTime && Clip->HasNotifies())
                        {
                            const uint16 EventStart = (uint16)EventScratch.size();
                            AnimEvents::CollectTriggeredNotifies(Clip, PrevSampleTime, Clock, bLooping, 1.0f, EventScratch);
                            ClockEvents[DstClock] = { EventStart, (uint16)EventScratch.size() };
                        }
                    }
                    if (DstFinished < NumScalar)
                    {
                        Scalars[DstFinished] = Finished;
                    }
                }
                break;
            }

            case EAnimOp::SampleAnim:
            {
                const uint16 ClipIdx = Reader.Read<uint16>();
                const uint16 TimeReg = Reader.Read<uint16>();
                const uint16 Dst     = Reader.Read<uint16>();

                FAnimTask Task;
                if (ClipIdx < NumClips && Graph->Clips[ClipIdx].IsValid())
                {
                    Task.Type = EAnimTaskType::SampleClip;
                    Task.Clip = Graph->Clips[ClipIdx].Get();
                    Task.Time = ReadScalar(TimeReg, 0.0f);
                }
                else
                {
                    Task.Type = EAnimTaskType::ReferencePose;
                }
                SetPoseTask(Dst, OutTasks.Add(Task));

                // Adopt the clock's root-motion / event / sync tags onto the sampled pose.
                SetPoseTags(Dst,
                            TimeReg < NumScalar ? ClockDeltas[TimeReg] : FRootMotionDelta(),
                            TimeReg < NumScalar ? ClockEvents[TimeReg] : FEventRange());
                SetPoseSync(Dst, TimeReg < NumScalar ? ClockSync[TimeReg] : FSyncTag());
                break;
            }

            case EAnimOp::RefPose:
            {
                const uint16 Dst = Reader.Read<uint16>();
                FAnimTask Task;
                Task.Type = EAnimTaskType::ReferencePose;
                SetPoseTask(Dst, OutTasks.Add(Task));
                SetPoseTags(Dst, FRootMotionDelta(), FEventRange());
                SetPoseSync(Dst, FSyncTag());
                break;
            }

            case EAnimOp::Blend:
            {
                const uint16 A     = Reader.Read<uint16>();
                const uint16 B     = Reader.Read<uint16>();
                const uint16 Alpha = Reader.Read<uint16>();
                const uint16 Dst   = Reader.Read<uint16>();

                FAnimTask Task;
                Task.Type  = EAnimTaskType::Blend;
                Task.DepA  = PoseTaskFor(A);
                Task.DepB  = PoseTaskFor(B);
                Task.Alpha = ReadScalar(Alpha, 0.0f);
                SetPoseTask(Dst, OutTasks.Add(Task));

                const float BlendAlpha = Math::Clamp(Task.Alpha, 0.0f, 1.0f);
                ScaleEventWeights(EventsOf(A), 1.0f - BlendAlpha);
                ScaleEventWeights(EventsOf(B), BlendAlpha);
                SetPoseTags(Dst,
                            RootMotion::BlendRootMotion(DeltaOf(A), DeltaOf(B), BlendAlpha),
                            UnionEvents(EventsOf(A), EventsOf(B)));

                // Both inputs from the same sync group: this blend's alpha refines the group's
                // duration (consumed next update, so weights are one frame latent).
                const FSyncTag SyncA = SyncOf(A);
                const FSyncTag SyncB = SyncOf(B);
                if (SyncA.Group >= 0 && SyncA.Group == SyncB.Group && SyncA.Group < (int32)State.SyncGroups.size())
                {
                    const float Blended = SyncA.ClipDuration + (SyncB.ClipDuration - SyncA.ClipDuration) * BlendAlpha;
                    State.SyncGroups[SyncA.Group].NextDuration = Blended;
                    SetPoseSync(Dst, FSyncTag{ SyncA.Group, Blended });
                }
                else
                {
                    SetPoseSync(Dst, SyncA.Group >= 0 ? SyncA : SyncB);
                }
                break;
            }

            case EAnimOp::BlendMasked:
            {
                const uint16 A       = Reader.Read<uint16>();
                const uint16 B       = Reader.Read<uint16>();
                const uint16 Alpha   = Reader.Read<uint16>();
                const uint16 MaskIdx = Reader.Read<uint16>();
                const uint16 Dst     = Reader.Read<uint16>();

                FAnimTask Task;
                Task.Type  = EAnimTaskType::BlendMasked;
                Task.DepA  = PoseTaskFor(A);
                Task.DepB  = PoseTaskFor(B);
                Task.Alpha = ReadScalar(Alpha, 0.0f);
                // Out-of-range mask index falls back to a whole-skeleton blend (null weights).
                Task.MaskWeights = MaskIdx < Graph->BoneMasks.size() ? &Graph->BoneMasks[MaskIdx].Weights : nullptr;
                SetPoseTask(Dst, OutTasks.Add(Task));

                // A layered blend keeps the base's root motion (the layer shouldn't drive the entity);
                // events from both layers fire at full weight (an upper-body attack still lands).
                SetPoseTags(Dst, DeltaOf(A), UnionEvents(EventsOf(A), EventsOf(B)));
                SetPoseSync(Dst, SyncOf(A));
                break;
            }

            case EAnimOp::MakeAdditive:
            {
                const uint16 Src = Reader.Read<uint16>();
                const uint16 Dst = Reader.Read<uint16>();

                FAnimTask Task;
                Task.Type = EAnimTaskType::MakeAdditive;
                Task.DepA = PoseTaskFor(Src);
                SetPoseTask(Dst, OutTasks.Add(Task));

                // A delta pose carries no root motion of its own; its events ride along.
                SetPoseTags(Dst, FRootMotionDelta(), EventsOf(Src));
                SetPoseSync(Dst, SyncOf(Src));
                break;
            }

            case EAnimOp::ApplyAdditive:
            {
                const uint16 Base  = Reader.Read<uint16>();
                const uint16 Delta = Reader.Read<uint16>();
                const uint16 Alpha = Reader.Read<uint16>();
                const uint16 Dst   = Reader.Read<uint16>();

                FAnimTask Task;
                Task.Type  = EAnimTaskType::ApplyAdditive;
                Task.DepA  = PoseTaskFor(Base);
                Task.DepB  = PoseTaskFor(Delta);
                Task.Alpha = ReadScalar(Alpha, 0.0f);
                SetPoseTask(Dst, OutTasks.Add(Task));

                ScaleEventWeights(EventsOf(Delta), Math::Clamp(Task.Alpha, 0.0f, 1.0f));
                SetPoseTags(Dst, DeltaOf(Base), UnionEvents(EventsOf(Base), EventsOf(Delta)));
                SetPoseSync(Dst, SyncOf(Base));
                break;
            }

            case EAnimOp::EvalStateMachine:
            {
                const uint16 SmIdx = Reader.Read<uint16>();
                const uint16 Dst   = Reader.Read<uint16>();

                if (SmIdx >= Graph->StateMachines.size() || Dst >= NumPose)
                {
                    break;
                }

                const FAnimGraphStateMachine& SM = Graph->StateMachines[SmIdx];
                const int32 NumStates = (int32)SM.StatePoseRegisters.size();

                FAnimTask RefTask;
                RefTask.Type = EAnimTaskType::ReferencePose;

                if (NumStates == 0)
                {
                    SetPoseTask(Dst, OutTasks.Add(RefTask));
                    SetPoseTags(Dst, FRootMotionDelta(), FEventRange());
                    SetPoseSync(Dst, FSyncTag());
                    break;
                }

                // Out-of-range slot = corrupt/version-mismatched bytecode; fall back to bind pose.
                if (SM.CurrentStateSlot >= NumState || SM.FromStateSlot >= NumState ||
                    SmIdx >= State.Inertializers.size())
                {
                    SetPoseTask(Dst, OutTasks.Add(RefTask));
                    SetPoseTags(Dst, FRootMotionDelta(), FEventRange());
                    SetPoseSync(Dst, FSyncTag());
                    break;
                }

                FAnimInertializer& Inert = State.Inertializers[SmIdx];

                int32 Current = Math::Clamp((int32)State.StateSlots[SM.CurrentStateSlot], 0, NumStates - 1);
                int32 From    = (int32)State.StateSlots[SM.FromStateSlot];

                // Pick a transition: any matching edge when stable; only interruptible edges mid-transition.
                // The first passing edge (author order) wins.
                const bool bTransitioning = From >= 0;
                bool bStart = false;
                for (const FAnimGraphTransition& Transition : SM.Transitions)
                {
                    if (bTransitioning && !Transition.bCanInterrupt)
                    {
                        continue;
                    }
                    const bool bFromMatches = (Transition.FromState == Current) || (Transition.FromState < 0);
                    if (!bFromMatches ||
                        Transition.ToState == Current ||
                        Transition.ToState < 0 ||
                        Transition.ToState >= NumStates)
                    {
                        continue;
                    }
                    if (Detail::EvalTransitionCondition(Transition, Graph, State.Parameters))
                    {
                        From           = Current;
                        Current        = Transition.ToState;
                        Inert.Duration = Math::Max(Transition.BlendDuration, 0.0f);
                        bStart         = true;
                        break;
                    }
                }

                const uint16 CurReg = SM.StatePoseRegisters[Current];

                if (CurReg >= NumPose)
                {
                    SetPoseTask(Dst, OutTasks.Add(RefTask));
                    SetPoseTags(Dst, FRootMotionDelta(), FEventRange());
                    SetPoseSync(Dst, FSyncTag());
                    From = -1;
                }
                else
                {
                    // Inertialize the seam: the update pass owns the control state (start/expiry/elapsed);
                    // the recorded task captures the offset from the last shown pose and decays it onto the
                    // freshly-evaluated target at execute time. Only the target state's chain is wired, so
                    // inactive states never evaluate. An interrupt re-captures from the currently-shown
                    // (already-inertializing) pose, so velocity stays continuous.
                    if (bStart)
                    {
                        Inert.bActive = Inert.Duration > 1e-5f;
                        Inert.Elapsed = 0.0f;
                    }

                    FAnimTask Task;
                    Task.Type      = EAnimTaskType::StateMachineOutput;
                    Task.DepA      = PoseTaskFor(CurReg);
                    Task.Inert     = &Inert;
                    Task.bCapture  = bStart;
                    Task.bApply    = Inert.bActive;
                    Task.Time      = Inert.Elapsed; // pre-advance: the offset decays from this frame's time
                    Task.DeltaTime = DeltaTime;
                    SetPoseTask(Dst, OutTasks.Add(Task));

                    // Only the target state's branch survives: its root motion drives the entity and
                    // its events fire; inactive states' tags are simply never propagated.
                    SetPoseTags(Dst, DeltaOf(CurReg), EventsOf(CurReg));
                    SetPoseSync(Dst, SyncOf(CurReg));

                    if (Inert.bActive)
                    {
                        Inert.Elapsed += DeltaTime;
                        if (Inert.Elapsed >= Inert.Duration)
                        {
                            Inert.bActive = false;
                            From = -1;
                        }
                    }
                    else
                    {
                        From = -1;
                    }
                }

                State.StateSlots[SM.CurrentStateSlot] = (float)Current;
                State.StateSlots[SM.FromStateSlot]    = (float)From;
                break;
            }

            case EAnimOp::BoneTransform:
            {
                const uint16 Src      = Reader.Read<uint16>();
                const uint16 AlphaReg = Reader.Read<uint16>();
                const uint16 BoneIdx  = Reader.Read<uint16>();
                const uint16 SpaceReg = Reader.Read<uint16>();
                const uint16 ModeReg  = Reader.Read<uint16>();
                const FVector3 T      = Reader.Read<FVector3>();
                const FQuat R         = Reader.Read<FQuat>();
                const FVector3 S      = Reader.Read<FVector3>();
                const uint16 Dst      = Reader.Read<uint16>();

                const EBoneTransformSpace Space = Detail::ReadEnumReg<EBoneTransformSpace>(Scalars, NumScalar, SpaceReg, (int32)EBoneTransformSpace::ComponentSpace);
                const EBoneTransformMode  Mode  = Detail::ReadEnumReg<EBoneTransformMode>(Scalars, NumScalar, ModeReg, (int32)EBoneTransformMode::Replace);

                FAnimTask Task;
                Task.Type  = EAnimTaskType::BoneTransform;
                Task.DepA  = PoseTaskFor(Src);
                Task.Alpha = ReadScalar(AlphaReg, 1.0f);
                Task.T     = T;
                Task.R     = R;
                Task.S     = S;
                Task.BoneA = BoneIdx;
                Task.Space = (uint8)(Space == EBoneTransformSpace::LocalBone ? AnimPose::EBoneSpace::LocalBone : AnimPose::EBoneSpace::ComponentSpace);
                Task.Mode  = (uint8)(Mode == EBoneTransformMode::Add ? AnimPose::EBoneApplyMode::Add : AnimPose::EBoneApplyMode::Replace);
                SetPoseTask(Dst, OutTasks.Add(Task));
                SetPoseTags(Dst, DeltaOf(Src), EventsOf(Src));
                SetPoseSync(Dst, SyncOf(Src));
                break;
            }

            case EAnimOp::TwoBoneIK:
            {
                const uint16 Src      = Reader.Read<uint16>();
                const uint16 AlphaReg = Reader.Read<uint16>();
                const uint16 TX       = Reader.Read<uint16>();
                const uint16 TY       = Reader.Read<uint16>();
                const uint16 TZ       = Reader.Read<uint16>();
                const uint16 RootIdx  = Reader.Read<uint16>();
                const uint16 MidIdx   = Reader.Read<uint16>();
                const uint16 EndIdx   = Reader.Read<uint16>();
                const FVector3 Pole   = Reader.Read<FVector3>();
                const uint16 Dst      = Reader.Read<uint16>();

                FAnimTask Task;
                Task.Type  = EAnimTaskType::TwoBoneIK;
                Task.DepA  = PoseTaskFor(Src);
                Task.Alpha = ReadScalar(AlphaReg, 1.0f);
                Task.T     = FVector3(ReadScalar(TX, 0.0f), ReadScalar(TY, 0.0f), ReadScalar(TZ, 0.0f));
                Task.S     = Pole;
                Task.BoneA = RootIdx;
                Task.BoneB = MidIdx;
                Task.BoneC = EndIdx;
                SetPoseTask(Dst, OutTasks.Add(Task));
                SetPoseTags(Dst, DeltaOf(Src), EventsOf(Src));
                SetPoseSync(Dst, SyncOf(Src));
                break;
            }

            case EAnimOp::Output:
            {
                const uint16 Src = Reader.Read<uint16>();
                OutTasks.OutputTask = PoseTaskFor(Src);

                RootMotionInOut.Delta = DeltaOf(Src);

                // Pin the root when locked, or when the branch that reached the output is
                // root-motion driven (its motion moves the entity; the pose must stay centered).
                OutTasks.bLockRoot = RootMotionInOut.Mode == ERootMotionLockMode::ForceLock ||
                                     (bExtractRootMotion && RootMotionInOut.Delta.bHasMotion);
                OutTasks.RootBoneIndex = RootMotionInOut.RootBoneIndex;

                if (OutEvents != nullptr)
                {
                    const FEventRange Surviving = EventsOf(Src);
                    for (uint16 i = Surviving.Start; i < Surviving.End && i < (uint16)EventScratch.size(); ++i)
                    {
                        if (EventScratch[i].Weight > 0.01f)
                        {
                            OutEvents->push_back(EventScratch[i]);
                        }
                    }
                }

                Reader.Cursor = Reader.Size;
                break;
            }

            default:
            {
                // Unknown opcode, so the stream is corrupt or version-mismatched.
                Reader.Cursor = Reader.Size;
                break;
            }
            }
        }

        // A graph with no Output still yields a valid recipe (bind pose), so the executor always runs
        // and the mesh never keeps stale matrices.
        if (OutTasks.OutputTask == FAnimTask::NoTask)
        {
            FAnimTask Ref;
            Ref.Type = EAnimTaskType::ReferencePose;
            OutTasks.OutputTask = OutTasks.Add(Ref);
        }
    }
}
