#include "AnimGraphNode_BlendSpace.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"

namespace Lumina
{
    void CAnimGraphNode_BlendSpace::BuildNode()
    {
        XPin     = CreateAnimPin("X", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        YPin     = CreateAnimPin("Y", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        SpeedPin = CreateAnimPin("Speed", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        PosePin  = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(XPin);
        BindFloatPinEditor(YPin);
        BindFloatPinEditor(SpeedPin);
    }

    void CAnimGraphNode_BlendSpace::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        if (!BlendSpace.IsValid())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Missing Blend Space";
            NodeError.Description = "Blend Space node has no asset assigned; it will evaluate to the bind pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }
        else if (BlendSpace->Samples.empty())
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Empty Blend Space";
            Warning.Description = "Blend Space has no samples; it will evaluate to the bind pose.";
            Warning.Node        = this;
            Compiler.AddWarning(Warning);
        }

        const uint16 BlendSpaceIndex = Compiler.AddBlendSpace(BlendSpace.Get());
        const uint16 PhaseSlot       = Compiler.AllocStateSlot();

        const uint16 XReg     = ResolveValueInput(XPin, Compiler);
        const uint16 SpeedReg = ResolveValueInput(SpeedPin, Compiler);

        // A one-axis blend space ignores Y, so an unconnected pin costs nothing to bake as a constant.
        const uint16 YReg = YPin->HasConnection()
            ? ResolveValueInput(YPin, Compiler)
            : Compiler.EmitLoadConst(0.0f);

        const uint16 PoseReg = Compiler.EmitSampleBlendSpace(BlendSpaceIndex, XReg, YReg, SpeedReg, PhaseSlot);

        Compiler.SetPinRegister(PosePin, PoseReg);
    }
}
