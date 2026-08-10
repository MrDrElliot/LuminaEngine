#include "RuntimePCH.h"
#include "AnimationGraphVM.h"

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Console/ConsoleVariable.h"
#include "Memory/Memcpy.h"
#include "Renderer/MeshData.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"

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
        State.CurveValues.assign(Graph->CurveNames.size(), 0.0f);

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
            State.CurveValues.size() != Graph->CurveNames.size() ||
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

        // Curves ride the pose registers as a dense per-register slot array, so every op that blends
        // poses blends the curves identically and a value tracks whatever its branch's weight does.
        const SIZE_T NumCurves = Graph->CurveNames.size();

        thread_local TVector<float> PoseCurves;
        thread_local TVector<float> CurveScratch;
        PoseCurves.assign(NumPose * NumCurves, 0.0f);
        CurveScratch.assign(NumCurves, 0.0f);

        if (!State.CurveValues.empty())
        {
            Memory::Memset(State.CurveValues.data(), 0, State.CurveValues.size() * sizeof(float));
        }

        const auto CurvesOf = [&](uint16 Reg) -> float*
        {
            return (NumCurves > 0 && Reg < NumPose) ? PoseCurves.data() + (SIZE_T)Reg * NumCurves : nullptr;
        };

        const auto SampleClipCurvesInto = [&](float* RESTRICT Dst, const CAnimation* Clip, const FAnimGraphClipCurveMap* Map, float Time)
        {
            if (Dst == nullptr)
            {
                return;
            }

            Memory::Memset(Dst, 0, NumCurves * sizeof(float));
            if (Clip == nullptr || Map == nullptr)
            {
                return;
            }

            const TVector<FAnimationCurve>& Curves = Clip->GetCurves();
            const SIZE_T Count = Math::Min(Curves.size(), Map->Slots.size());
            for (SIZE_T i = 0; i < Count; ++i)
            {
                const int32 Slot = Map->Slots[i];
                if (Slot >= 0 && Slot < (int32)NumCurves)
                {
                    Dst[Slot] = Curves[i].Curve.Evaluate(Time);
                }
            }
        };

        const auto ClipCurveMapFor = [&](uint16 ClipIdx) -> const FAnimGraphClipCurveMap*
        {
            return ClipIdx < Graph->ClipCurveMaps.size() ? &Graph->ClipCurveMaps[ClipIdx] : nullptr;
        };

        const auto ZeroCurves = [&](uint16 Dst)
        {
            if (float* D = CurvesOf(Dst))
            {
                Memory::Memset(D, 0, NumCurves * sizeof(float));
            }
        };

        const auto CopyCurves = [&](uint16 Dst, uint16 Src)
        {
            float* D = CurvesOf(Dst);
            if (D == nullptr)
            {
                return;
            }
            const float* S = CurvesOf(Src);
            if (S == nullptr)
            {
                Memory::Memset(D, 0, NumCurves * sizeof(float));
            }
            else if (S != D)
            {
                Memory::Memcpy(D, S, NumCurves * sizeof(float));
            }
        };

        // A curve missing from one side reads as 0 there, so it fades in/out with the blend.
        const auto LerpCurves = [&](uint16 Dst, uint16 A, uint16 B, float Alpha)
        {
            float* D = CurvesOf(Dst);
            if (D == nullptr)
            {
                return;
            }
            const float* SA = CurvesOf(A);
            const float* SB = CurvesOf(B);
            for (SIZE_T i = 0; i < NumCurves; ++i)
            {
                const float VA = SA != nullptr ? SA[i] : 0.0f;
                const float VB = SB != nullptr ? SB[i] : 0.0f;
                D[i] = VA + (VB - VA) * Alpha;
            }
        };

        const auto AddCurves = [&](uint16 Dst, uint16 Base, uint16 Delta, float Alpha)
        {
            float* D = CurvesOf(Dst);
            if (D == nullptr)
            {
                return;
            }
            const float* SBase  = CurvesOf(Base);
            const float* SDelta = CurvesOf(Delta);
            for (SIZE_T i = 0; i < NumCurves; ++i)
            {
                D[i] = (SBase != nullptr ? SBase[i] : 0.0f) + (SDelta != nullptr ? SDelta[i] : 0.0f) * Alpha;
            }
        };

        // State-machine seam: the pose is inertialized execute-side, so the curves have to follow the
        // same schedule here or they step at the transition. Same quintic decay with zero initial
        // velocity, evaluated against the transition's own elapsed time.
        const auto InertializeCurves = [&](FAnimInertializer& Inert, uint16 Dst, bool bStart)
        {
            float* D = CurvesOf(Dst);
            if (D == nullptr)
            {
                return;
            }

            if (Inert.CurveOffsets.size() != NumCurves)
            {
                Inert.CurveOffsets.assign(NumCurves, 0.0f);
                Inert.PrevCurves.assign(NumCurves, 0.0f);
                Inert.bHasCurveHistory = false;
            }

            if (bStart)
            {
                for (SIZE_T i = 0; i < NumCurves; ++i)
                {
                    Inert.CurveOffsets[i] = Inert.bHasCurveHistory ? (Inert.PrevCurves[i] - D[i]) : 0.0f;
                }
            }

            if (Inert.bActive && Inert.Duration > 1e-5f)
            {
                const float U = Math::Clamp(Inert.Elapsed / Inert.Duration, 0.0f, 1.0f);
                const float Decay = 1.0f - U * U * U * (U * (U * 6.0f - 15.0f) + 10.0f);
                for (SIZE_T i = 0; i < NumCurves; ++i)
                {
                    D[i] += Inert.CurveOffsets[i] * Decay;
                }
            }

            Memory::Memcpy(Inert.PrevCurves.data(), D, NumCurves * sizeof(float));
            Inert.bHasCurveHistory = true;
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

                SampleClipCurvesInto(CurvesOf(Dst), Task.Clip, ClipCurveMapFor(ClipIdx), Task.Time);

                // Adopt the clock's root-motion / event / sync tags onto the sampled pose.
                SetPoseTags(Dst,
                            TimeReg < NumScalar ? ClockDeltas[TimeReg] : FRootMotionDelta(),
                            TimeReg < NumScalar ? ClockEvents[TimeReg] : FEventRange());
                SetPoseSync(Dst, TimeReg < NumScalar ? ClockSync[TimeReg] : FSyncTag());
                break;
            }

            case EAnimOp::SampleBlendSpace:
            {
                const uint16 BlendSpaceIdx = Reader.Read<uint16>();
                const uint16 XReg          = Reader.Read<uint16>();
                const uint16 YReg          = Reader.Read<uint16>();
                const uint16 SpeedReg      = Reader.Read<uint16>();
                const uint16 PhaseSlot     = Reader.Read<uint16>();
                const uint16 Dst           = Reader.Read<uint16>();

                const CBlendSpace* BlendSpace = (BlendSpaceIdx < (uint16)Graph->BlendSpaces.size())
                    ? Graph->BlendSpaces[BlendSpaceIdx].Get() : nullptr;

                FBlendSpaceWeights Weights;
                if (BlendSpace != nullptr)
                {
                    BlendSpace->Evaluate(FVector2(ReadScalar(XReg, 0.0f), ReadScalar(YReg, 0.0f)), Weights);
                }

                if (Weights.Count == 0)
                {
                    FAnimTask Task;
                    Task.Type = EAnimTaskType::ReferencePose;
                    SetPoseTask(Dst, OutTasks.Add(Task));
                    ZeroCurves(Dst);
                    break;
                }

                const FAnimGraphBlendSpaceCurveMap* BlendSpaceCurves = BlendSpaceIdx < Graph->BlendSpaceCurveMaps.size()
                    ? &Graph->BlendSpaceCurveMaps[BlendSpaceIdx] : nullptr;
                float* DstCurves = CurvesOf(Dst);

                // One shared normalized phase across every contributing clip. Sampling each at the same
                // fraction of its own duration is what keeps a walk/run blend on a single stride; the
                // phase speed comes from the weighted duration so it retimes as the blend moves.
                const float PrevPhase = (PhaseSlot < (uint16)State.StateSlots.size()) ? State.StateSlots[PhaseSlot] : 0.0f;
                float Phase = PrevPhase;

                const float BlendedDuration = BlendSpace->GetBlendedDuration(Weights);
                if (BlendedDuration > 1e-4f)
                {
                    Phase += (DeltaTime * ReadScalar(SpeedReg, 1.0f)) / BlendedDuration;
                    Phase -= Math::Floor(Phase);
                }

                if (PhaseSlot < (uint16)State.StateSlots.size())
                {
                    State.StateSlots[PhaseSlot] = Phase;
                }

                int16 Accumulated = FAnimTask::NoTask;
                float AccumulatedWeight = 0.0f;

                // Root motion and notifies are folded with the same running alpha as the pose, so a
                // sample contributing 20% of the pose contributes 20% of the motion.
                FRootMotionDelta BlendedDelta;
                const uint16 EventStart = (uint16)EventScratch.size();

                for (int32 i = 0; i < Weights.Count; ++i)
                {
                    const SBlendSpaceSample& Sample = BlendSpace->Samples[Weights.SampleIndices[i]];
                    CAnimation* SampleClip = Sample.Animation.Get();

                    FAnimTask Task;
                    if (SampleClip != nullptr)
                    {
                        Task.Type = EAnimTaskType::SampleClip;
                        Task.Clip = SampleClip;
                        Task.Time = Phase * SampleClip->GetDuration();
                    }
                    else
                    {
                        Task.Type = EAnimTaskType::ReferencePose;
                    }

                    const int16 SampleTask = OutTasks.Add(Task);

                    // Each sample walks the same normalized phase across its own duration, which is the
                    // interval its root delta and notifies have to be measured over.
                    const float SampleDuration = SampleClip != nullptr ? SampleClip->GetDuration() : 0.0f;
                    const float PrevSampleTime = PrevPhase * SampleDuration;
                    const float SampleTime     = Phase * SampleDuration;

                    FRootMotionDelta SampleDelta;
                    if (bExtractRootMotion && SampleClip != nullptr
                        && SampleClip->bEnableRootMotion && !SampleClip->bLockRootMotion)
                    {
                        if (SampleTime != PrevSampleTime)
                        {
                            SampleDelta = RootMotion::ExtractRootDelta(SampleClip, Skeleton, RootMotionInOut.RootBoneIndex,
                                                                       PrevSampleTime, SampleTime, true, SampleDuration);
                        }

                        // Held even on a paused frame so the branch keeps reading as root-motion driven.
                        SampleDelta.bHasMotion = true;
                    }

                    if (OutEvents != nullptr && SampleClip != nullptr
                        && SampleTime != PrevSampleTime && SampleClip->HasNotifies())
                    {
                        AnimEvents::CollectTriggeredNotifies(SampleClip, PrevSampleTime, SampleTime, true,
                                                             Weights.Weights[i], EventScratch);
                    }

                    const int32 SampleIndex = Weights.SampleIndices[i];
                    const FAnimGraphClipCurveMap* SampleCurveMap =
                        (BlendSpaceCurves != nullptr && SampleIndex >= 0 && SampleIndex < (int32)BlendSpaceCurves->SampleMaps.size())
                        ? &BlendSpaceCurves->SampleMaps[SampleIndex] : nullptr;

                    if (Accumulated == FAnimTask::NoTask)
                    {
                        Accumulated = SampleTask;
                        AccumulatedWeight = Weights.Weights[i];
                        BlendedDelta = SampleDelta;
                        SampleClipCurvesInto(DstCurves, SampleClip, SampleCurveMap, SampleTime);
                        continue;
                    }

                    // Fold in sequentially: each new sample's share of the running total, so three
                    // barycentric weights collapse to two blends without renormalizing by hand.
                    AccumulatedWeight += Weights.Weights[i];
                    const float FoldAlpha = AccumulatedWeight > 1e-5f ? (Weights.Weights[i] / AccumulatedWeight) : 0.0f;

                    FAnimTask BlendTask;
                    BlendTask.Type = EAnimTaskType::Blend;
                    BlendTask.DepA = Accumulated;
                    BlendTask.DepB = SampleTask;
                    BlendTask.Alpha = FoldAlpha;

                    Accumulated = OutTasks.Add(BlendTask);
                    BlendedDelta = RootMotion::BlendRootMotion(BlendedDelta, SampleDelta, FoldAlpha);

                    if (DstCurves != nullptr)
                    {
                        SampleClipCurvesInto(CurveScratch.data(), SampleClip, SampleCurveMap, SampleTime);
                        for (SIZE_T c = 0; c < NumCurves; ++c)
                        {
                            DstCurves[c] += (CurveScratch[c] - DstCurves[c]) * FoldAlpha;
                        }
                    }
                }

                SetPoseTask(Dst, Accumulated);
                SetPoseTags(Dst, BlendedDelta, FEventRange{ EventStart, (uint16)EventScratch.size() });
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
                ZeroCurves(Dst);
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
                LerpCurves(Dst, A, B, BlendAlpha);
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
                LerpCurves(Dst, A, B, Math::Clamp(Task.Alpha, 0.0f, 1.0f));
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

                // A delta pose carries no root motion of its own; its events and curves ride along.
                SetPoseTags(Dst, FRootMotionDelta(), EventsOf(Src));
                SetPoseSync(Dst, SyncOf(Src));
                CopyCurves(Dst, Src);
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
                AddCurves(Dst, Base, Delta, Math::Clamp(Task.Alpha, 0.0f, 1.0f));
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
                    ZeroCurves(Dst);
                    break;
                }

                // Out-of-range slot = corrupt/version-mismatched bytecode; fall back to bind pose.
                if (SM.CurrentStateSlot >= NumState || SM.FromStateSlot >= NumState ||
                    SmIdx >= State.Inertializers.size())
                {
                    SetPoseTask(Dst, OutTasks.Add(RefTask));
                    SetPoseTags(Dst, FRootMotionDelta(), FEventRange());
                    SetPoseSync(Dst, FSyncTag());
                    ZeroCurves(Dst);
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
                    ZeroCurves(Dst);
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
                    CopyCurves(Dst, CurReg);
                    InertializeCurves(Inert, Dst, bStart);

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
                CopyCurves(Dst, Src);
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
                CopyCurves(Dst, Src);
                break;
            }

            case EAnimOp::Output:
            {
                const uint16 Src = Reader.Read<uint16>();
                OutTasks.OutputTask = PoseTaskFor(Src);

                RootMotionInOut.Delta = DeltaOf(Src);

                const float* OutputCurves = CurvesOf(Src);
                if (OutputCurves != nullptr && State.CurveValues.size() == NumCurves)
                {
                    Memory::Memcpy(State.CurveValues.data(), OutputCurves, NumCurves * sizeof(float));
                }

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

            case EAnimOp::GetCurve:
            {
                const uint16 Src      = Reader.Read<uint16>();
                const uint16 CurveIdx = Reader.Read<uint16>();
                const uint16 Dst      = Reader.Read<uint16>();

                if (Dst < NumScalar)
                {
                    const float* SrcCurves = CurvesOf(Src);
                    Scalars[Dst] = (SrcCurves != nullptr && CurveIdx < NumCurves) ? SrcCurves[CurveIdx] : 0.0f;
                }
                break;
            }

            case EAnimOp::SetCurve:
            {
                const uint16 Src      = Reader.Read<uint16>();
                const uint16 CurveIdx = Reader.Read<uint16>();
                const uint16 ValueReg = Reader.Read<uint16>();
                const uint16 Dst      = Reader.Read<uint16>();

                // Curves live entirely in this pass, so overriding one costs no pose work: the
                // destination register forwards the source's task and tags untouched.
                SetPoseTask(Dst, PoseTaskFor(Src));
                SetPoseTags(Dst, DeltaOf(Src), EventsOf(Src));
                SetPoseSync(Dst, SyncOf(Src));
                CopyCurves(Dst, Src);

                float* DstCurves = CurvesOf(Dst);
                if (DstCurves != nullptr && CurveIdx < NumCurves)
                {
                    DstCurves[CurveIdx] = ReadScalar(ValueReg, 0.0f);
                }
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
