#include "AnimGraphNode_BoneTransform.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_BoneTransform::BuildNode()
    {
        PoseInPin  = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin   = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        SpacePin   = CreateAnimPin("Space", ENodePinDirection::Input, EAnimPinType::Value, (float)Space);
        ModePin    = CreateAnimPin("Mode", ENodePinDirection::Input, EAnimPinType::Value, (float)Mode);
        PoseOutPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
        BindEnumPinEditor(SpacePin, { "Local Bone", "Component" },
            [this]() { return (int)Space; },
            [this](int Value) { Space = (EBoneTransformSpace)Value; });
        BindEnumPinEditor(ModePin, { "Add", "Replace" },
            [this]() { return (int)Mode; },
            [this](int Value) { Mode = (EBoneTransformMode)Value; });
    }

    void CAnimGraphNode_BoneTransform::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const int32 BoneIndex = Compiler.ResolveBoneIndex(BoneName);
        if (BoneIndex == INDEX_NONE)
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Unknown Bone";
            NodeError.Description = BoneName.IsNone()
                ? FString("Bone Transform has no Bone Name set; pose will be passed through unchanged.")
                : FString("Bone Transform references bone '") + BoneName.ToString() + "' which doesn't exist on the graph's skeleton.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);

            const uint16 SrcReg = ResolvePoseInput(PoseInPin, Compiler);
            Compiler.SetPinRegister(PoseOutPin, SrcReg);
            return;
        }

        const uint16 SrcReg   = ResolvePoseInput(PoseInPin, Compiler);
        const uint16 AlphaReg = ResolveAlphaInput(AlphaPin, Compiler, AlphaEasing);

        // Register-driven so they can be wired, and an unconnected pin bakes the property as a constant.
        const uint16 SpaceReg = SpacePin->HasConnection()
            ? ResolveValueInput(SpacePin, Compiler)
            : Compiler.EmitLoadConst((float)Space);
        const uint16 ModeReg = ModePin->HasConnection()
            ? ResolveValueInput(ModePin, Compiler)
            : Compiler.EmitLoadConst((float)Mode);

        const FVector3 RadEuler = Math::Radians(Rotation);
        const FQuat Quat     = Math::Normalize(FQuat(RadEuler));

        const uint16 ResultReg = Compiler.EmitBoneTransform(SrcReg, AlphaReg, (uint16)BoneIndex,
                                                            SpaceReg, ModeReg,
                                                            Translation, Quat, Scale);

        Compiler.SetPinRegister(PoseOutPin, ResultReg);
    }
}
