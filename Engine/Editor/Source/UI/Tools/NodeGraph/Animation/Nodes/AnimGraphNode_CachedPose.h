#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_CachedPose.generated.h"

namespace Lumina
{
    // Names the pose flowing into it so branches elsewhere in the graph can reuse it without re-evaluating.
    REFLECT()
    class CAnimGraphNode_SaveCachedPose : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Save Cached Pose"; }
        FStringView GetNodeTooltip() const override { return "Evaluates the incoming pose once and publishes it under a name. Use Cached Pose nodes naming the same cache reuse that result rather than evaluating the branch again."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        FString GetNodeTitleText() const override;

        /** Cache written to. A Use Cached Pose node naming the same cache reads it back. */
        PROPERTY(Editable, Category = "Cached Pose")
        FName CacheName = "CachedPose";

        CAnimGraphPin* PoseInputPin = nullptr;
    };

    // Reuses a pose a Save Cached Pose node already evaluated this frame.
    REFLECT()
    class CAnimGraphNode_UseCachedPose : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Use Cached Pose"; }
        FStringView GetNodeTooltip() const override { return "Outputs the pose published by the Save Cached Pose node with a matching name. Both read the same register, so reusing a pose costs nothing beyond its original evaluation."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        FString GetNodeTitleText() const override;

        /** Cache read back. Must match the name on the Save Cached Pose node that writes it. */
        PROPERTY(Editable, Category = "Cached Pose")
        FName CacheName = "CachedPose";

        CAnimGraphPin* PosePin = nullptr;
    };
}
