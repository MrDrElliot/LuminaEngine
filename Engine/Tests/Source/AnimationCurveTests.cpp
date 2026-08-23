#include <gtest/gtest.h>

// The graph compiler is editor-only, so these tests compile out when Tests is built without it.
#if WITH_EDITOR

#include "Animation/AnimationGraphVM.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/SkeletonResource.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

using namespace Lumina;

namespace
{
    CAnimation* MakeClip(float Duration)
    {
        CAnimation* Clip = NewObject<CAnimation>();
        Clip->GetAnimationResource()->Duration = Duration;
        return Clip;
    }

    void AddCurve(CAnimation* Clip, const FName& Name, float StartValue, float EndValue)
    {
        FAnimationCurve Curve;
        Curve.Name = Name;
        Curve.Curve.AddKey(0.0f, StartValue);
        Curve.Curve.AddKey(Clip->GetDuration(), EndValue);
        Clip->GetAnimationResource()->Curves.push_back(Curve);
    }

    // Curve evaluation never touches bone transforms, so a one-bone skeleton is enough.
    void MakeSkeleton(FSkeletonResource& Skeleton)
    {
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name        = FName("Root");
        Bone.ParentIndex = INDEX_NONE;
        Skeleton.Bones.push_back(Bone);
        Skeleton.BoneNameToIndex[Bone.Name] = 0;
    }

    float RunGraph(CAnimationGraph* Graph, const FName& CurveName)
    {
        FSkeletonResource Skeleton;
        MakeSkeleton(Skeleton);

        FAnimGraphVMState State;
        FAnimTaskList Tasks;
        FAnimGraphRootMotion RootMotion;
        FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.016f, State, Tasks, RootMotion);

        const int32 Slot = Graph->FindCurveIndex(CurveName);
        return (Slot != INDEX_NONE && Slot < (int32)State.CurveValues.size()) ? State.CurveValues[Slot] : NAN;
    }
}

// A curve rides its pose, so blending two poses blends their curve values by the same alpha.
TEST(AnimationCurves, BlendLerpsCurveValues)
{
    CAnimation* WalkClip = MakeClip(1.0f);
    AddCurve(WalkClip, FName("Speed"), 0.0f, 1.0f);

    CAnimation* RunClip = MakeClip(1.0f);
    AddCurve(RunClip, FName("Speed"), 10.0f, 10.0f);

    FAnimationGraphCompiler Compiler;
    const uint16 TimeReg  = Compiler.EmitLoadConst(0.5f);
    const uint16 PoseA    = Compiler.EmitSampleAnim(Compiler.AddClip(WalkClip), TimeReg);
    const uint16 PoseB    = Compiler.EmitSampleAnim(Compiler.AddClip(RunClip), TimeReg);
    const uint16 AlphaReg = Compiler.EmitLoadConst(0.25f);
    Compiler.EmitOutput(Compiler.EmitBlend(PoseA, PoseB, AlphaReg));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    // Walk sits at 0.5 half way through its ramp; run is a flat 10.
    EXPECT_NEAR(RunGraph(Graph, FName("Speed")), 0.5f + (10.0f - 0.5f) * 0.25f, 1e-4f);
}

// A curve only one branch authors must fade in with that branch's weight rather than snapping in.
TEST(AnimationCurves, CurveMissingFromOneSideFadesWithTheBlend)
{
    CAnimation* WithCurve = MakeClip(1.0f);
    AddCurve(WithCurve, FName("HandIK"), 1.0f, 1.0f);

    CAnimation* WithoutCurve = MakeClip(1.0f);

    FAnimationGraphCompiler Compiler;
    const uint16 TimeReg  = Compiler.EmitLoadConst(0.0f);
    const uint16 PoseA    = Compiler.EmitSampleAnim(Compiler.AddClip(WithCurve), TimeReg);
    const uint16 PoseB    = Compiler.EmitSampleAnim(Compiler.AddClip(WithoutCurve), TimeReg);
    const uint16 AlphaReg = Compiler.EmitLoadConst(0.25f);
    Compiler.EmitOutput(Compiler.EmitBlend(PoseA, PoseB, AlphaReg));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    EXPECT_NEAR(RunGraph(Graph, FName("HandIK")), 0.75f, 1e-4f);
}

// The Get Curve node's opcode reads the value off the pose it is wired to, in the same frame.
TEST(AnimationCurves, GetCurveReadsTheValueOffThePose)
{
    CAnimation* Clip = MakeClip(2.0f);
    AddCurve(Clip, FName("Speed"), 0.0f, 4.0f);

    FAnimationGraphCompiler Compiler;
    const uint16 TimeReg  = Compiler.EmitLoadConst(1.5f);
    const uint16 Pose     = Compiler.EmitSampleAnim(Compiler.AddClip(Clip), TimeReg);
    const uint16 ValueReg = Compiler.EmitGetCurve(Pose, 0);
    Compiler.EmitOutput(Pose);

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    ASSERT_EQ(Graph->FindCurveIndex(FName("Speed")), 0);

    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;
    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.016f, State, Tasks, RootMotion);

    ASSERT_LT(ValueReg, State.ScalarRegisters.size());
    EXPECT_NEAR(State.ScalarRegisters[ValueReg], 3.0f, 1e-4f);
}

// Set Curve overrides the sampled value and passes the pose through untouched.
TEST(AnimationCurves, SetCurveOverridesTheSampledValue)
{
    CAnimation* Clip = MakeClip(1.0f);
    AddCurve(Clip, FName("Speed"), 5.0f, 5.0f);

    FAnimationGraphCompiler Compiler;
    const uint16 TimeReg   = Compiler.EmitLoadConst(0.0f);
    const uint16 Pose      = Compiler.EmitSampleAnim(Compiler.AddClip(Clip), TimeReg);
    const uint16 ValueReg  = Compiler.EmitLoadConst(-2.0f);
    const uint16 Overriden = Compiler.EmitSetCurve(Pose, 0, ValueReg);
    Compiler.EmitOutput(Overriden);

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    EXPECT_NEAR(RunGraph(Graph, FName("Speed")), -2.0f, 1e-4f);
}

// An additive layer adds its curves on top of the base, scaled by the layer's alpha.
TEST(AnimationCurves, ApplyAdditiveAddsCurvesScaledByAlpha)
{
    CAnimation* BaseClip = MakeClip(1.0f);
    AddCurve(BaseClip, FName("Lean"), 1.0f, 1.0f);

    CAnimation* AdditiveClip = MakeClip(1.0f);
    AddCurve(AdditiveClip, FName("Lean"), 2.0f, 2.0f);

    FAnimationGraphCompiler Compiler;
    const uint16 TimeReg  = Compiler.EmitLoadConst(0.0f);
    const uint16 Base     = Compiler.EmitSampleAnim(Compiler.AddClip(BaseClip), TimeReg);
    const uint16 Sampled  = Compiler.EmitSampleAnim(Compiler.AddClip(AdditiveClip), TimeReg);
    const uint16 Delta    = Compiler.EmitMakeAdditive(Sampled);
    const uint16 AlphaReg = Compiler.EmitLoadConst(0.5f);
    Compiler.EmitOutput(Compiler.EmitApplyAdditive(Base, Delta, AlphaReg));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    EXPECT_NEAR(RunGraph(Graph, FName("Lean")), 1.0f + 2.0f * 0.5f, 1e-4f);
}

// A transition cross-fades the pose, so its curves must ease across the same window.
TEST(AnimationCurves, StateTransitionEasesCurvesAcrossTheBlend)
{
    CAnimation* IdleClip = MakeClip(1.0f);
    AddCurve(IdleClip, FName("Speed"), 1.0f, 1.0f);

    CAnimation* RunClip = MakeClip(1.0f);
    AddCurve(RunClip, FName("Speed"), 5.0f, 5.0f);

    FAnimationGraphCompiler Compiler;
    const uint16 TimeReg  = Compiler.EmitLoadConst(0.0f);
    const uint16 IdlePose = Compiler.EmitSampleAnim(Compiler.AddClip(IdleClip), TimeReg);
    const uint16 RunPose  = Compiler.EmitSampleAnim(Compiler.AddClip(RunClip), TimeReg);

    FAnimGraphStateMachine Machine;
    Machine.EntryState           = 0;
    Machine.StatePoseRegisters   = { IdlePose, RunPose };
    Machine.CurrentStateSlot     = Compiler.AllocStateSlot();
    Machine.FromStateSlot        = Compiler.AllocStateSlot();
    Machine.TimeInStateSlot      = Compiler.AllocStateSlot();
    Machine.DurationSlot         = Compiler.AllocStateSlot();

    FAnimGraphTransition ToRun;
    ToRun.FromState          = 0;
    ToRun.ToState            = 1;
    FAnimGraphTransitionTerm GoTerm;
    GoTerm.ConditionSource   = EAnimTransitionSource::Parameter;
    GoTerm.Name              = FName("Go");
    GoTerm.Compare           = EAnimTransitionCompare::Greater;
    GoTerm.CompareValue      = 0.5f;
    ToRun.Terms              = { GoTerm };
    ToRun.BlendDuration      = 0.2f;
    Machine.Transitions.push_back(ToRun);

    const int32 GoParam = Compiler.AddParameter(FName("Go"), EAnimGraphParamType::Float, 0.0f);
    Compiler.EmitOutput(Compiler.EmitEvalStateMachine(Move(Machine)));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    const int32 Slot = Graph->FindCurveIndex(FName("Speed"));
    ASSERT_NE(Slot, INDEX_NONE);

    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;

    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_NEAR(State.CurveValues[Slot], 1.0f, 1e-4f) << "idle state's curve";

    // The frame the transition starts still shows the previous value, undecayed.
    State.Parameters[GoParam] = 1.0f;
    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_NEAR(State.CurveValues[Slot], 1.0f, 1e-3f) << "curve must not step to the target on the seam";

    // Mid-transition it sits between the two.
    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    EXPECT_GT(State.CurveValues[Slot], 1.0f);
    EXPECT_LT(State.CurveValues[Slot], 5.0f);

    // Past the blend duration it is fully the target state's value.
    for (int32 i = 0; i < 8; ++i)
    {
        FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.05f, State, Tasks, RootMotion);
    }
    EXPECT_NEAR(State.CurveValues[Slot], 5.0f, 1e-4f);
}

// A graph with no curves at all must not allocate or address any curve state.
TEST(AnimationCurves, GraphWithoutCurvesCarriesNoCurveState)
{
    CAnimation* Clip = MakeClip(1.0f);

    FAnimationGraphCompiler Compiler;
    const uint16 TimeReg = Compiler.EmitLoadConst(0.0f);
    Compiler.EmitOutput(Compiler.EmitSampleAnim(Compiler.AddClip(Clip), TimeReg));

    CAnimationGraph* Graph = NewObject<CAnimationGraph>();
    Compiler.BuildGraph(Graph);

    EXPECT_TRUE(Graph->CurveNames.empty());

    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton);

    FAnimGraphVMState State;
    FAnimTaskList Tasks;
    FAnimGraphRootMotion RootMotion;
    FAnimationGraphVM::BuildTasks(Graph, &Skeleton, 0.016f, State, Tasks, RootMotion);

    EXPECT_TRUE(State.CurveValues.empty());
    EXPECT_TRUE(Tasks.HasWork());
}

#endif
