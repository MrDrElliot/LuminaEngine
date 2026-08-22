#include "AnimGraphNode_ClipEvaluator.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    void CAnimGraphNode_ClipEvaluator::BuildNode()
    {
        AnimationPin     = CreateAnimPin("Animation", ENodePinDirection::Input, EAnimPinType::Object);
        TimePin          = CreateAnimPin("Time", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        StartPositionPin = CreateAnimPin("Start Position", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        PosePin          = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(TimePin);
        BindFloatPinEditor(StartPositionPin);
    }

    void CAnimGraphNode_ClipEvaluator::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        // A wired Animation pin supplies the clip at runtime, so a missing static asset is fine there.
        const int32 ClipObjectReg = ResolveObjectInput(AnimationPin, Compiler);
        const bool bDynamicClip = ClipObjectReg != INDEX_NONE;

        if (!bDynamicClip && !Clip.IsValid())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Missing Clip";
            NodeError.Description = "Evaluate Animation Clip node has no clip assigned and no Animation input; it will evaluate to the bind pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }

        const uint16 ClipIndex = bDynamicClip ? (uint16)ClipObjectReg : Compiler.AddClip(Clip.Get());

        uint16 TimeReg = ResolveValueInput(TimePin, Compiler);

        // The node has no clock to start, so the offset lands on Time, in whichever units Time is read as.
        if (StartPositionPin->HasConnection() || GetValuePinDefault(StartPositionPin) != 0.0f)
        {
            TimeReg = Compiler.EmitScalarOp(EAnimScalarOp::Add, TimeReg, ResolveValueInput(StartPositionPin, Compiler));
        }

        if (bNormalizedTime)
        {
            if (bDynamicClip)
            {
                EdNodeGraph::FError Warning;
                Warning.Name        = "Normalized Time Needs A Static Clip";
                Warning.Description = "A wired Animation pin is only known at runtime, so its length cannot be baked. Time is read as seconds here; turn Normalized Time off, or assign the clip on the node.";
                Warning.Node        = this;
                Compiler.AddWarning(Warning);
            }
            else if (Clip.IsValid())
            {
                const float Duration = Clip->GetDuration();
                TimeReg = Compiler.EmitScalarOp(EAnimScalarOp::Mul, TimeReg, Compiler.EmitLoadConst(Duration));
            }
        }

        Compiler.SetPinRegister(PosePin, Compiler.EmitSampleAnim(ClipIndex, TimeReg, bDynamicClip));
    }
}
