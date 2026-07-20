#include "AnimGraphNode_ClipPlayer.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    void CAnimGraphNode_ClipPlayer::BuildNode()
    {
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
        if (!Clip.IsValid())
        {
            EdNodeGraph::FError Error;
            Error.Name        = "Missing Clip";
            Error.Description = "Play Animation Clip node has no clip assigned; it will evaluate to the bind pose.";
            Error.Node        = this;
            Compiler.AddError(Error);
        }

        const uint16 ClipIndex = Compiler.AddClip(Clip.Get());
        const uint16 StateSlot = Compiler.AllocStateSlot();
        const uint16 SpeedReg  = ResolveValueInput(SpeedPin, Compiler);

        // Loop mode is register-driven so it can be wired; an unconnected pin
        // bakes the property's current value as a constant.
        const uint16 LoopModeReg = LoopModePin->HasConnection()
            ? ResolveValueInput(LoopModePin, Compiler)
            : Compiler.EmitLoadConst((float)LoopMode);

        const uint16 SyncGroupIndex = SyncGroup.IsNone() ? kAnimNoSyncGroup : Compiler.AddSyncGroup(SyncGroup);

        uint16 FinishedReg = 0;
        const uint16 TimeReg = Compiler.EmitAdvanceClock(StateSlot, SpeedReg, ClipIndex, LoopModeReg, FinishedReg, SyncGroupIndex);
        const uint16 PoseReg = Compiler.EmitSampleAnim(ClipIndex, TimeReg);

        Compiler.SetPinRegister(PosePin, PoseReg);
        Compiler.SetPinRegister(FinishedPin, FinishedReg);
    }
}
