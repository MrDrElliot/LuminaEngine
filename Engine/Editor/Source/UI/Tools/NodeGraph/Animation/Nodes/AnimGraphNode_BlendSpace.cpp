#include "AnimGraphNode_BlendSpace.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"

namespace Lumina
{
    void CAnimGraphNode_BlendSpace::BuildNode()
    {
        BlendSpacePin = CreateAnimPin("Blend Space", ENodePinDirection::Input, EAnimPinType::Object);
        XPin     = CreateAnimPin("X", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        YPin     = CreateAnimPin("Y", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);
        SpeedPin = CreateAnimPin("Speed", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);

        // Normalized, because the samples share one phase and have no common duration to measure seconds against.
        StartPositionPin = CreateAnimPin("Start Position", ENodePinDirection::Input, EAnimPinType::Value, 0.0f);

        PosePin  = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(XPin);
        BindFloatPinEditor(YPin);
        BindFloatPinEditor(SpeedPin);
        BindFloatPinEditor(StartPositionPin);
    }

    void CAnimGraphNode_BlendSpace::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        // A wired Blend Space pin supplies the asset at runtime, so a missing static asset is fine there.
        const int32 BlendSpaceObjectReg = ResolveObjectInput(BlendSpacePin, Compiler);
        const bool bDynamicBlendSpace = BlendSpaceObjectReg != INDEX_NONE;

        if (!bDynamicBlendSpace && !BlendSpace.IsValid())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Missing Blend Space";
            NodeError.Description = "Blend Space node has no asset assigned and no Blend Space input; it will evaluate to the bind pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }
        else if (!bDynamicBlendSpace && BlendSpace.IsValid() && BlendSpace->Samples.empty())
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Empty Blend Space";
            Warning.Description = "Blend Space has no samples; it will evaluate to the bind pose.";
            Warning.Node        = this;
            Compiler.AddWarning(Warning);
        }

        const uint16 BlendSpaceIndex = bDynamicBlendSpace ? (uint16)BlendSpaceObjectReg
                                                          : Compiler.AddBlendSpace(BlendSpace.Get());
        const uint16 PhaseSlot       = Compiler.AllocClockSlot();

        const uint16 SpeedReg    = ResolveValueInput(SpeedPin, Compiler);
        const uint16 StartPosReg = ResolveValueInput(StartPositionPin, Compiler);

        uint16 XReg = ResolveValueInput(XPin, Compiler);
        if (XSmoothingHalfLife > 0.0f)
        {
            XReg = Compiler.EmitSmoothScalar(XReg, Compiler.EmitLoadConst(XSmoothingHalfLife));
        }

        // A one-axis blend space ignores Y, so an unconnected pin costs nothing to bake as a constant.
        uint16 YReg = YPin->HasConnection()
            ? ResolveValueInput(YPin, Compiler)
            : Compiler.EmitLoadConst(0.0f);
        if (YSmoothingHalfLife > 0.0f)
        {
            YReg = Compiler.EmitSmoothScalar(YReg, Compiler.EmitLoadConst(YSmoothingHalfLife));
        }

        const uint16 PoseReg = Compiler.EmitSampleBlendSpace(BlendSpaceIndex, XReg, YReg, SpeedReg, PhaseSlot, StartPosReg, bDynamicBlendSpace);

        Compiler.SetPinRegister(PosePin, PoseReg);
    }
}
