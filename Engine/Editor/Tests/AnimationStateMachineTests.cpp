#include <gtest/gtest.h>

// Pulls in editor-only headers, so these tests compile out when Tests is built without the editor.
#if WITH_EDITOR

#include "Animation/AnimationGraphVM.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/SkeletonResource.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

using namespace Lumina;

namespace
{
    CAnimation* MakeStateClip(float Duration)
    {
        CAnimation* Clip = NewObject<CAnimation>();
        Clip->GetAnimationResource()->Duration = Duration;
        return Clip;
    }

    void MakeOneBoneSkeleton(FSkeletonResource& Skeleton)
    {
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name        = FName("Root");
        Bone.ParentIndex = INDEX_NONE;
        Skeleton.Bones.push_back(Bone);
        Skeleton.BoneNameToIndex[Bone.Name] = 0;
    }

    FAnimGraphTransitionTerm MakeTerm(EAnimTransitionSource Source, FName Name, EAnimTransitionCompare Compare, float Value)
    {
        FAnimGraphTransitionTerm Term;
        Term.ConditionSource = Source;
        Term.Name            = Name;
        Term.Compare         = Compare;
        Term.CompareValue    = Value;
        return Term;
    }

    // One play-once clip player, reported the way the state machine node records a state.
    struct FTestState
    {
        uint16 PoseRegister = 0;
        uint16 ClockSlotFirst = 0;
        uint16 ClockSlotEnd = 0;
        uint16 ClockSlot = 0;
    };

    FTestState CompilePlayOnceState(FAnimationGraphCompiler& Compiler, CAnimation* Clip)
    {
        FTestState Out;
        Out.ClockSlotFirst = (uint16)Compiler.GetClockSlots().size();
        Out.ClockSlot      = Compiler.AllocClockSlot();

        const uint16 SpeedReg    = Compiler.EmitLoadConst(1.0f);
        const uint16 LoopModeReg = Compiler.EmitLoadConst((float)EClipLoopMode::PlayOnce);
        const uint16 ClipIndex   = Compiler.AddClip(Clip);

        uint16 FinishedReg = 0;
        const uint16 StartPosReg = Compiler.EmitLoadConst(0.0f);
        const uint16 TimeReg = Compiler.EmitAdvanceClock(Out.ClockSlot, SpeedReg, ClipIndex, LoopModeReg, StartPosReg, FinishedReg);

        Out.PoseRegister  = Compiler.EmitSampleAnim(ClipIndex, TimeReg);
        Out.ClockSlotEnd  = (uint16)Compiler.GetClockSlots().size();
        return Out;
    }

    struct FTwoStateGraph
    {
        CAnimationGraph* Graph = nullptr;
        CAnimation*      ClipA = nullptr;
        CAnimation*      ClipB = nullptr;
        int32            GoParam = INDEX_NONE;
    };

    // Two play-once states that swap on a Go parameter, optionally bracketed so the VM can skip the inactive one.
    FTwoStateGraph CompileTwoStateGraph(bool bSkipInactive, bool bShareCachedPoseAcrossStates = false)
    {
        FTwoStateGraph Out;
        Out.ClipA = MakeStateClip(1.0f);
        Out.ClipB = MakeStateClip(2.0f);

        FAnimationGraphCompiler Compiler;
        const uint32 EnterOp = bSkipInactive ? Compiler.EmitEnterStateMachine(2) : 0;

        if (bSkipInactive) { Compiler.BeginStateCode(EnterOp, 0); }
        const FTestState A = CompilePlayOnceState(Compiler, Out.ClipA);
        if (bShareCachedPoseAcrossStates) { Compiler.SetCachedPose(FName("Shared"), A.PoseRegister); }
        if (bSkipInactive) { Compiler.EndStateCode(EnterOp, 0); }

        if (bSkipInactive) { Compiler.BeginStateCode(EnterOp, 1); }
        const FTestState B = CompilePlayOnceState(Compiler, Out.ClipB);
        if (bShareCachedPoseAcrossStates)
        {
            uint16 SharedRegister = 0;
            EXPECT_TRUE(Compiler.TryGetCachedPose(FName("Shared"), SharedRegister));
        }
        if (bSkipInactive) { Compiler.EndStateCode(EnterOp, 1); }

        FAnimGraphStateMachine Machine;
        Machine.EntryState          = 0;
        Machine.StatePoseRegisters  = { A.PoseRegister, B.PoseRegister };
        Machine.ClockSlots          = Compiler.GetClockSlots();
        Machine.StateClockSlotFirst = { A.ClockSlotFirst, B.ClockSlotFirst };
        Machine.StateClockSlotEnd   = { A.ClockSlotEnd, B.ClockSlotEnd };
        Machine.CurrentStateSlot    = Compiler.AllocStateSlot();
        Machine.FromStateSlot       = Compiler.AllocStateSlot();
        Machine.TimeInStateSlot     = Compiler.AllocStateSlot();
        Machine.DurationSlot        = Compiler.AllocStateSlot();

        FAnimGraphTransition ToB;
        ToB.FromState     = 0;
        ToB.ToState       = 1;
        ToB.Terms         = { MakeTerm(EAnimTransitionSource::Parameter, FName("Go"), EAnimTransitionCompare::Greater, 0.5f) };
        ToB.BlendDuration = 0.1f;
        Machine.Transitions.push_back(ToB);

        FAnimGraphTransition ToA = ToB;
        ToA.FromState = 1;
        ToA.ToState   = 0;
        ToA.Terms[0].Compare = EAnimTransitionCompare::Less;
        Machine.Transitions.push_back(ToA);

        Out.GoParam = Compiler.AddParameter(FName("Go"), EAnimGraphParamType::Float, 0.0f);
        if (bSkipInactive) { Compiler.FinishStateMachineCode(EnterOp); }
        Compiler.EmitOutput(Compiler.EmitEvalStateMachine(Move(Machine)));

        Out.Graph = NewObject<CAnimationGraph>();
        Compiler.BuildGraph(Out.Graph);
        return Out;
    }

    bool SamplesClip(const FAnimTaskList& Tasks, const CAnimation* Clip)
    {
        for (const FAnimTask& Task : Tasks.Tasks)
        {
            if (Task.Type == EAnimTaskType::SampleClip && Task.Clip == Clip)
            {
                return true;
            }
        }
        return false;
    }
}

// Clocks advance while a state is inactive, so a finished play-once must be wound back to replay.
TEST(AnimationStateMachine, PlayOnceStateRestartsOnReEntry)
{
    CAnimation* OneShotClip = MakeStateClip(1.0f);
    CAnimation* IdleClip    = MakeStateClip(1.0f);

    FAnimationGraphCompiler Compiler;
    const FTestState OneShot = CompilePlayOnceState(Compiler, OneShotClip);
    const FTestState Idle    = CompilePlayOnceState(Compiler, IdleClip);

    FAnimGraphStateMachine Machine;
    Machine.EntryState          = 0;
    Machine.StatePoseRegisters  = { OneShot.PoseRegister, Idle.PoseRegister };
    Machine.ClockSlots          = Compiler.GetClockSlots();
    Machine.StateClockSlotFirst = { OneShot.ClockSlotFirst, Idle.ClockSlotFirst };
    Machine.StateClockSlotEnd   = { OneShot.ClockSlotEnd, Idle.ClockSlotEnd };
    Machine.CurrentStateSlot    = Compiler.AllocStateSlot();
    Machine.FromStateSlot       = Compiler.AllocStateSlot();
    Machine.TimeInStateSlot     = Compiler.AllocStateSlot();
    Machine.DurationSlot        = Compiler.AllocStateSlot();

    FAnimGraphTransition ToIdle;
    ToIdle.FromState          = 0;
    ToIdle.ToState            = 1;
    ToIdle.Terms              = { MakeTerm(EAnimTransitionSource::Parameter, FName("Go"), EAnimTransitionCompare::Greater, 0.5f) };
    ToIdle.BlendDuration      = 0.0f;
    Machine.Transitions.push_back(ToIdle);

    FAnimGraphTransition BackToOneShot = ToIdle;
    BackToOneShot.FromState = 1;
    BackToOneShot.ToState   = 0;
    BackToOneShot.Terms[0].Compare = EAnimTransitionCompare::Less;
    Machine.Transitions.push_back(BackToOneShot);

    const int32 GoParam = Compiler.AddParameter(FName("Go"), EAnimGraphParamType::Float, 0.0f);
    Compiler.EmitOutput(Compiler.EmitEvalStateMachine(Move(Machine)));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    FSkeletonResource Skeleton;
    MakeOneBoneSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;

    // Play the one-shot out past its duration.
    for (int32 Tick = 0; Tick < 30; ++Tick)
    {
        FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    }
    EXPECT_NEAR(State.StateSlots[OneShot.ClockSlot], 1.0f, 1e-4f) << "clip should have clamped at its end";

    // Leave it. Its clock is wound back while it is not the current state.
    State.Parameters[GoParam] = 1.0f;
    for (int32 Tick = 0; Tick < 3; ++Tick)
    {
        FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    }
    EXPECT_NEAR(State.StateSlots[OneShot.ClockSlot], 0.0f, 1e-4f) << "inactive state's clock must not run on";

    // Re-entering plays the clip from the top rather than resuming at its finished end.
    State.Parameters[GoParam] = 0.0f;
    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_NEAR(State.StateSlots[OneShot.ClockSlot], 0.05f, 1e-4f) << "re-entry must restart the clip";
}

// A play-once state hands over on dwell time, so no gameplay code needs to know the clip's length.
TEST(AnimationStateMachine, TimeInStateTransitionFiresAfterItsDwellTime)
{
    CAnimation* FirstClip  = MakeStateClip(1.0f);
    CAnimation* SecondClip = MakeStateClip(1.0f);

    FAnimationGraphCompiler Compiler;
    const FTestState First  = CompilePlayOnceState(Compiler, FirstClip);
    const FTestState Second = CompilePlayOnceState(Compiler, SecondClip);

    FAnimGraphStateMachine Machine;
    Machine.EntryState          = 0;
    Machine.StatePoseRegisters  = { First.PoseRegister, Second.PoseRegister };
    Machine.ClockSlots          = Compiler.GetClockSlots();
    Machine.StateClockSlotFirst = { First.ClockSlotFirst, Second.ClockSlotFirst };
    Machine.StateClockSlotEnd   = { First.ClockSlotEnd, Second.ClockSlotEnd };
    Machine.CurrentStateSlot    = Compiler.AllocStateSlot();
    Machine.FromStateSlot       = Compiler.AllocStateSlot();
    Machine.TimeInStateSlot     = Compiler.AllocStateSlot();
    Machine.DurationSlot        = Compiler.AllocStateSlot();

    FAnimGraphTransition WhenDone;
    WhenDone.FromState       = 0;
    WhenDone.ToState         = 1;
    WhenDone.Terms           = { MakeTerm(EAnimTransitionSource::TimeInState, FName(), EAnimTransitionCompare::GreaterEqual, 0.3f) };
    WhenDone.BlendDuration   = 0.0f;
    Machine.Transitions.push_back(WhenDone);

    const uint16 CurrentStateSlot = Machine.CurrentStateSlot;
    Compiler.EmitOutput(Compiler.EmitEvalStateMachine(Move(Machine)));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    FSkeletonResource Skeleton;
    MakeOneBoneSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;

    for (int32 Tick = 0; Tick < 5; ++Tick)
    {
        FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    }
    EXPECT_NEAR(State.StateSlots[CurrentStateSlot], 0.0f, 1e-4f) << "0.25s in, the dwell time is not up";

    for (int32 Tick = 0; Tick < 3; ++Tick)
    {
        FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    }
    EXPECT_NEAR(State.StateSlots[CurrentStateSlot], 1.0f, 1e-4f) << "past 0.3s it must have moved on";
}

// A reset walking raw slots would zero the inner machine's current state every frame.
TEST(AnimationStateMachine, NestedMachineKeepsItsStateWhileItsOwnerIsInactive)
{
    FAnimationGraphCompiler Compiler;

    const FTestState InnerIdle   = CompilePlayOnceState(Compiler, MakeStateClip(1.0f));
    const FTestState InnerMoving = CompilePlayOnceState(Compiler, MakeStateClip(1.0f));

    FAnimGraphStateMachine Inner;
    Inner.EntryState          = 0;
    Inner.StatePoseRegisters  = { InnerIdle.PoseRegister, InnerMoving.PoseRegister };
    Inner.ClockSlots          = Compiler.GetClockSlots();
    Inner.StateClockSlotFirst = { InnerIdle.ClockSlotFirst, InnerMoving.ClockSlotFirst };
    Inner.StateClockSlotEnd   = { InnerIdle.ClockSlotEnd, InnerMoving.ClockSlotEnd };
    Inner.CurrentStateSlot    = Compiler.AllocStateSlot();
    Inner.FromStateSlot       = Compiler.AllocStateSlot();
    Inner.TimeInStateSlot     = Compiler.AllocStateSlot();
    Inner.DurationSlot        = Compiler.AllocStateSlot();

    FAnimGraphTransition ToInnerMoving;
    ToInnerMoving.FromState          = 0;
    ToInnerMoving.ToState            = 1;
    ToInnerMoving.Terms              = { MakeTerm(EAnimTransitionSource::Parameter, FName("InnerGo"), EAnimTransitionCompare::Greater, 0.5f) };
    ToInnerMoving.BlendDuration      = 0.0f;
    Inner.Transitions.push_back(ToInnerMoving);

    const uint16 InnerCurrentStateSlot = Inner.CurrentStateSlot;
    const uint16 InnerPose = Compiler.EmitEvalStateMachine(Move(Inner));

    const FTestState Airborne = CompilePlayOnceState(Compiler, MakeStateClip(1.0f));

    FAnimGraphStateMachine Outer;
    Outer.EntryState          = 0;
    Outer.StatePoseRegisters  = { InnerPose, Airborne.PoseRegister };
    Outer.ClockSlots          = Compiler.GetClockSlots();
    Outer.StateClockSlotFirst = { InnerIdle.ClockSlotFirst, Airborne.ClockSlotFirst };
    Outer.StateClockSlotEnd   = { InnerMoving.ClockSlotEnd, Airborne.ClockSlotEnd };
    Outer.CurrentStateSlot    = Compiler.AllocStateSlot();
    Outer.FromStateSlot       = Compiler.AllocStateSlot();
    Outer.TimeInStateSlot     = Compiler.AllocStateSlot();
    Outer.DurationSlot        = Compiler.AllocStateSlot();

    FAnimGraphTransition ToAirborne;
    ToAirborne.FromState          = 0;
    ToAirborne.ToState            = 1;
    ToAirborne.Terms              = { MakeTerm(EAnimTransitionSource::Parameter, FName("Airborne"), EAnimTransitionCompare::Greater, 0.5f) };
    ToAirborne.BlendDuration      = 0.0f;
    Outer.Transitions.push_back(ToAirborne);

    FAnimGraphTransition BackToGrounded = ToAirborne;
    BackToGrounded.FromState = 1;
    BackToGrounded.ToState   = 0;
    BackToGrounded.Terms[0].Compare = EAnimTransitionCompare::Less;
    Outer.Transitions.push_back(BackToGrounded);

    const int32 InnerGoParam  = Compiler.AddParameter(FName("InnerGo"), EAnimGraphParamType::Float, 0.0f);
    const int32 AirborneParam = Compiler.AddParameter(FName("Airborne"), EAnimGraphParamType::Float, 0.0f);
    Compiler.EmitOutput(Compiler.EmitEvalStateMachine(Move(Outer)));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    FSkeletonResource Skeleton;
    MakeOneBoneSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;

    // Drive the inner machine into its second state while its owner is active.
    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    State.Parameters[InnerGoParam] = 1.0f;
    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_NEAR(State.StateSlots[InnerCurrentStateSlot], 1.0f, 1e-4f) << "inner machine should have moved on";

    // Leave the owning state. The inner machine's current state must be left exactly where it was.
    State.Parameters[AirborneParam] = 1.0f;
    for (int32 Tick = 0; Tick < 5; ++Tick)
    {
        FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    }
    EXPECT_NEAR(State.StateSlots[InnerCurrentStateSlot], 1.0f, 1e-4f)
        << "an inactive owner must not reset its nested machine";
}

// Skipping inactive states is an optimization, so every clock, state and shown sample must match running them all.
TEST(AnimationStateMachine, SkippingInactiveStatesMatchesRunningThemAll)
{
    const FTwoStateGraph Full    = CompileTwoStateGraph(false);
    const FTwoStateGraph Skipped = CompileTwoStateGraph(true);

    FSkeletonResource Skeleton;
    MakeOneBoneSkeleton(Skeleton);

    FAnimGraphVMState FullState;
    FAnimGraphVMState SkippedState;
    FAnimTaskList FullTasks;
    FAnimTaskList SkippedTasks;
    FAnimGraphRootMotion RootMotion;
    FAnimationGraphVM::InitState(Full.Graph, FullState);
    FAnimationGraphVM::InitState(Skipped.Graph, SkippedState);

    const float GoPattern[] = { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f };
    int32 Tick = 0;
    for (const float Go : GoPattern)
    {
        for (int32 Step = 0; Step < 9; ++Step, ++Tick)
        {
            FullState.Parameters[Full.GoParam]       = Go;
            SkippedState.Parameters[Skipped.GoParam] = Go;
            FAnimationGraphVM::BuildTasks(Full.Graph, &Skeleton, 0.05f, FullState, FullTasks, RootMotion);
            FAnimationGraphVM::BuildTasks(Skipped.Graph, &Skeleton, 0.05f, SkippedState, SkippedTasks, RootMotion);

            ASSERT_EQ(FullState.StateSlots.size(), SkippedState.StateSlots.size());
            for (SIZE_T Slot = 0; Slot < FullState.StateSlots.size(); ++Slot)
            {
                EXPECT_NEAR(FullState.StateSlots[Slot], SkippedState.StateSlots[Slot], 1e-5f) << "slot " << Slot << " tick " << Tick;
            }

            const FAnimTask& FullShown    = FullTasks.Tasks[FullTasks.Tasks[FullTasks.OutputTask].DepA];
            const FAnimTask& SkippedShown = SkippedTasks.Tasks[SkippedTasks.Tasks[SkippedTasks.OutputTask].DepA];
            EXPECT_EQ(FullShown.Clip == Full.ClipA, SkippedShown.Clip == Skipped.ClipA) << "tick " << Tick;
            EXPECT_NEAR(FullShown.Time, SkippedShown.Time, 1e-5f) << "tick " << Tick;
            EXPECT_EQ(FullTasks.Tasks[FullTasks.OutputTask].bCapture, SkippedTasks.Tasks[SkippedTasks.OutputTask].bCapture) << "tick " << Tick;
        }
    }
}

// The inactive state records nothing, which is the whole point of skipping it.
TEST(AnimationStateMachine, AnInactiveStateRecordsNoTasks)
{
    const FTwoStateGraph Skipped = CompileTwoStateGraph(true);

    FSkeletonResource Skeleton;
    MakeOneBoneSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;

    FAnimationGraphVM::BuildTasks(Skipped.Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_TRUE(SamplesClip(Tasks, Skipped.ClipA));
    EXPECT_FALSE(SamplesClip(Tasks, Skipped.ClipB));

    // The update that takes the edge runs the state it enters, so the new clip shows at once.
    State.Parameters[Skipped.GoParam] = 1.0f;
    FAnimationGraphVM::BuildTasks(Skipped.Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_TRUE(SamplesClip(Tasks, Skipped.ClipB));
    EXPECT_EQ(Tasks.Tasks[Tasks.Tasks[Tasks.OutputTask].DepA].Clip, Skipped.ClipB);

    FAnimationGraphVM::BuildTasks(Skipped.Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_FALSE(SamplesClip(Tasks, Skipped.ClipA));
}

// A cached pose read outside the state that saves it needs that state to run, so the graph runs every state.
TEST(AnimationStateMachine, ACachedPoseSharedAcrossStatesKeepsEveryStateRunning)
{
    const FTwoStateGraph Shared = CompileTwoStateGraph(true, true);

    FSkeletonResource Skeleton;
    MakeOneBoneSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;

    FAnimationGraphVM::BuildTasks(Shared.Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_TRUE(SamplesClip(Tasks, Shared.ClipA));
    EXPECT_TRUE(SamplesClip(Tasks, Shared.ClipB));
}

#endif
