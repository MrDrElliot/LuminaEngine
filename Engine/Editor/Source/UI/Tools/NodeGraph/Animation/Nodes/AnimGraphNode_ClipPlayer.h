#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "Animation/AnimationGraphVM.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimGraphNode_ClipPlayer.generated.h"

namespace Lumina
{
    class CAnimation;

    // Plays a clip on a persistent VM-slot playback clock (time survives across frames);
    // the Speed input scales how fast the clock advances.
    REFLECT()
    class CAnimGraphNode_ClipPlayer : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FString GetNodeDisplayName() const override { return "Play Animation Clip"; }
        FString GetNodeTooltip() const override { return "Samples an animation clip on a looping playback clock."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Animation clip sampled by this node. */
        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CAnimation> Clip;

        /** How the playback clock behaves when it reaches the clip's duration.
         *  Loop wraps around (default); PlayOnce clamps at the end and the
         *  Finished output goes to 1. */
        PROPERTY(Editable, Category = "Animation")
        EClipLoopMode LoopMode = EClipLoopMode::Loop;

        /**
         * Clips sharing a sync group name advance one shared normalized phase instead of independent
         * clocks, so blends between them stay stride-aligned (no foot slide). Clips must be
         * phase-aligned by authoring (all starting on the same foot plant). Synced clips always loop.
         */
        PROPERTY(Editable, Category = "Animation")
        FName SyncGroup;

        CAnimGraphPin* SpeedPin = nullptr;
        CAnimGraphPin* LoopModePin = nullptr;
        CAnimGraphPin* PosePin = nullptr;
        CAnimGraphPin* FinishedPin = nullptr;
    };
}
