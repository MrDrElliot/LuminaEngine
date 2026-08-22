#include <gtest/gtest.h>

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
