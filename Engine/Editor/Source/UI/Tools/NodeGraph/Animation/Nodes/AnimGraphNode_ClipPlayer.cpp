#include "AnimGraphNode_ClipPlayer.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    void CAnimGraphNode_ClipPlayer::BuildNode()
    {
        AnimationPin = CreateAnimPin("Animation", ENodePinDirection::Input, EAnimPinType::Object);
        SpeedPin    = CreateAnimPin("Speed", ENodePinDirection::Input, EAnimPinType::Value, 1.0f);
        LoopModePin = CreateAnimPin("Loop Mode", ENodePinDirection::Input, EAnimPinType::Value, (float)LoopMode);
        PosePin     = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);
        FinishedPin = CreateAnimPin("Finished", ENodePinDirection::Output, EAnimPinType::Value);

        BindFloatPinEditor(SpeedPin);
        BindEnumPinEditor(LoopModePin, { "Loop", "Play Once" },
            [this]() { return (int)LoopMode; },
            [this](int Value) { LoopMode = (EClipLoopMode)Value; });
    }

    void CAnimGraphNode_ClipPlayer::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        // A wired Animation pin supplies the clip at runtime, so a missing static asset is fine there.
        const int32 ClipObjectReg = ResolveObjectInput(AnimationPin, Compiler);
        const bool bDynamicClip = ClipObjectReg != INDEX_NONE;

        if (!bDynamicClip && !Clip.IsValid())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Missing Clip";
            NodeError.Description = "Play Animation Clip node has no clip assigned and no Animation input; it will evaluate to the bind pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }

        const uint16 ClipIndex = bDynamicClip ? (uint16)ClipObjectReg : Compiler.AddClip(Clip.Get());
        const uint16 StateSlot = Compiler.AllocClockSlot();
        const uint16 SpeedReg  = ResolveValueInput(SpeedPin, Compiler);

        // Loop mode is register-driven so it can be wired; an unconnected pin
        // bakes the property's current value as a constant.
        const uint16 LoopModeReg = LoopModePin->HasConnection()
            ? ResolveValueInput(LoopModePin, Compiler)
            : Compiler.EmitLoadConst((float)LoopMode);

        const uint16 SyncGroupIndex = SyncGroup.IsNone() ? kAnimNoSyncGroup : Compiler.AddSyncGroup(SyncGroup);

        uint16 FinishedReg = 0;
        const uint16 TimeReg = Compiler.EmitAdvanceClock(StateSlot, SpeedReg, ClipIndex, LoopModeReg, FinishedReg, SyncGroupIndex, bDynamicClip);
        const uint16 PoseReg = Compiler.EmitSampleAnim(ClipIndex, TimeReg, bDynamicClip);

        Compiler.SetPinRegister(PosePin, PoseReg);
        Compiler.SetPinRegister(FinishedPin, FinishedReg);
    }
}
