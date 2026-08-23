#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_LayeredBlendPerBone.generated.h"

namespace Lumina
{
    // Per-bone blend of Base and Overlay via a bone mask: each bone's alpha is the global Alpha times its
    // mask weight (e.g. an upper-body mask keeps the lower body on the base). Mask referenced by name.
    REFLECT()
    class CAnimGraphNode_LayeredBlendPerBone : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Layered Blend Per Bone"; }
        FStringView GetNodeTooltip() const override { return "Blends Overlay onto Base over a bone's subtree (pick a bone) or a named bone mask."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Bone whose subtree the overlay affects (this bone + descendants), no authored mask needed.
         *  Takes priority over Mask Name; leave None to use a named mask instead. */
        PROPERTY(Editable, Category = "Bone Mask", Picker = "Bone")
        FName BoneName;

        /** When true the selected bone itself is affected; when false only its
         *  descendants are (the bone stays on the base pose). */
        PROPERTY(Editable, Category = "Bone Mask")
        bool bInclusive = true;

        /** Advanced: name of a bone mask declared on the graph asset's Bone
         *  Masks list. Used only when Bone Name is None. */
        PROPERTY(Editable, Category = "Bone Mask")
        FName MaskName;

        CAnimGraphPin* BasePin = nullptr;
        CAnimGraphPin* OverlayPin = nullptr;
        /** Interpolation applied to Alpha before it blends. Linear leaves the input untouched. */
        PROPERTY(Editable, Category = "Alpha")
        EAnimAlphaEasing AlphaEasing = EAnimAlphaEasing::Linear;

        CAnimGraphPin* AlphaPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };
}
