#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "AnimGraphNode_GetObjectParameter.generated.h"

namespace Lumina
{
    // Outputs an asset reference, so which clip a sampler plays is decided at runtime, not at compile.
    REFLECT()
    class CAnimGraphNode_GetObjectParameter : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Get Object Parameter"; }
        FStringView GetNodeTooltip() const override { return "Reads an asset reference from a blackboard object key."; }
        FString GetNodeTitleText() const override;

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Blackboard object key to read. */
        PROPERTY(Editable, Category = "Parameter", Picker = "ObjectParameter")
        FName ParameterName;

        /** Asset kind consumers should expect; a mismatch is caught when the node is wired. */
        PROPERTY(Editable, Category = "Parameter")
        EAnimObjectParamType ObjectType = EAnimObjectParamType::Animation;

        CAnimGraphPin* ObjectPin = nullptr;
    };
}
