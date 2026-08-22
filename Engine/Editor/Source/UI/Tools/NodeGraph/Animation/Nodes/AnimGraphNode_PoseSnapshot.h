#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_PoseSnapshot.generated.h"

namespace Lumina
{
    // Copies the pose flowing through it into a named buffer that outlives the frame.
    REFLECT()
    class CAnimGraphNode_SavePoseSnapshot : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Save Pose Snapshot"; }
        FStringView GetNodeTooltip() const override { return "Stores the incoming pose in a named slot while Request is above 0.5, and passes it through unchanged. Pose Snapshot reads the same slot back, which is how a graph blends out of the pose it was showing when something happened."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        FString GetNodeTitleText() const override;

        /** Slot written to. A Pose Snapshot node naming the same slot reads it back. */
        PROPERTY(Editable, Category = "Snapshot")
        FName SnapshotName = "Snapshot";

        CAnimGraphPin* PoseInputPin = nullptr;
        CAnimGraphPin* RequestPin = nullptr;
        CAnimGraphPin* ResultPin = nullptr;
    };

    // Reads a named buffer back as a pose.
    REFLECT()
    class CAnimGraphNode_PoseSnapshot : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Pose Snapshot"; }
        FStringView GetNodeTooltip() const override { return "Outputs the pose stored in a named slot by Save Pose Snapshot. A slot nothing has written yet reads as the bind pose. The stored pose carries no root motion, notifies or curves."; }
        FFixedString GetNodeCategory() const override { return "Animation"; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        FString GetNodeTitleText() const override;

        /** Slot read back. Must match the name on the Save Pose Snapshot node that writes it. */
        PROPERTY(Editable, Category = "Snapshot")
        FName SnapshotName = "Snapshot";

        CAnimGraphPin* PosePin = nullptr;
    };
}
