#include "AnimGraphNode_LayeredBlendPerBone.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_LayeredBlendPerBone::BuildNode()
    {
        BasePin    = CreateAnimPin("Base", ENodePinDirection::Input, EAnimPinType::Pose);
        OverlayPin = CreateAnimPin("Overlay", ENodePinDirection::Input, EAnimPinType::Pose);
        AlphaPin   = CreateAnimPin("Alpha", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        ResultPin  = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);

        BindFloatPinEditor(AlphaPin);
    }

    void CAnimGraphNode_LayeredBlendPerBone::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        const uint16 Base    = ResolvePoseInput(BasePin, Compiler);
        const uint16 Overlay = ResolvePoseInput(OverlayPin, Compiler);
        const uint16 Alpha   = ResolveValueInput(AlphaPin, Compiler);

        // 0xFFFF leaves the VM to fall back to a whole-skeleton blend on an unconfigured node.
        uint16 MaskIdx = 0xFFFFu;

        if (!BoneName.IsNone())
        {
            // Build a mask on the fly from the chosen bone's subtree.
            const int32 BoneIndex = Compiler.ResolveBoneIndex(BoneName);
            if (BoneIndex != INDEX_NONE)
            {
                MaskIdx = Compiler.AddBoneSubtreeMask(BoneIndex, bInclusive);
            }
            else
            {
                EdNodeGraph::FError Warning;
                Warning.Name        = "Unknown Bone";
                Warning.Description = FString("Layered Blend Per Bone references bone '") + BoneName.ToString() +
                    "' which doesn't exist on the graph's skeleton; falling back to a whole-skeleton blend.";
                Warning.Node        = this;
                Compiler.AddWarning(Warning);
            }
        }
        else
        {
            const int32 MaskIndex = Compiler.FindBoneMaskIndex(MaskName);
            if (MaskIndex != INDEX_NONE)
            {
                MaskIdx = (uint16)MaskIndex;
            }
            else
            {
                EdNodeGraph::FError Warning;
                Warning.Name        = "No Bone / Mask";
                Warning.Description = MaskName.IsNone()
                    ? FString("Layered Blend Per Bone has no Bone or Mask set; falling back to a whole-skeleton blend.")
                    : FString("Layered Blend Per Bone references mask '") + MaskName.ToString() + "' which isn't defined on the graph asset's Bone Masks list.";
                Warning.Node        = this;
                Compiler.AddWarning(Warning);
            }
        }

        const uint16 Result = Compiler.EmitBlendMasked(Base, Overlay, Alpha, MaskIdx);
        Compiler.SetPinRegister(ResultPin, Result);
    }
}
