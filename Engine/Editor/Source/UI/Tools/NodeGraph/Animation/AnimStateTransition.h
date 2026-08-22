#pragma once

#include "Core/Object/Object.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "AnimStateTransition.generated.h"

namespace Lumina
{
    // Data behind a transition wire (not a graph node): holds the condition, shown in the properties
    // panel when its wire is selected. CAnimStateMachineGraph owns and reconciles these in ValidateGraph().
    REFLECT()
    class CAnimStateTransition : public CObject
    {
        GENERATED_BODY()
    public:

        // Node IDs of the source / destination State nodes this transition wires
        // together. Used to match the object back to its canvas link.
        PROPERTY()
        int64 FromStateNodeID = 0;

        PROPERTY()
        int64 ToStateNodeID = 0;

        /** What Compare Value is tested against: a graph parameter, or how long the state machine has
         *  been in its current state. Time In State is how a play-once state hands over when it ends. */
        PROPERTY(Editable, Category = "Transition")
        EAnimTransitionSource ConditionSource = EAnimTransitionSource::Parameter;

        /** Graph parameter compared against Compare Value to gate the transition.
         *  Leave empty for an unconditional / always-true transition. */
        PROPERTY(Editable, Category = "Transition", Picker = "Parameter")
        FName ConditionParameter;

        /** How the parameter is tested against Compare Value. */
        PROPERTY(Editable, Category = "Transition")
        EAnimTransitionCompare Compare = EAnimTransitionCompare::Greater;

        /** Right-hand side of the comparison. */
        PROPERTY(Editable, Category = "Transition")
        float CompareValue = 0.0f;

        /** Cross-fade length in seconds when this transition fires. 0 snaps instantly. */
        PROPERTY(Editable, Category = "Blending")
        float BlendDuration = 0.2f;

        /** Order this transition is tested in; lower goes first and the first passing edge wins.
         *  Ties fall back to the order the wires were created. */
        PROPERTY(Editable, Category = "Transition", ClampMin = 0)
        int32 Priority = 0;

        /** When true, this transition is re-checked every frame DURING an
         *  in-flight cross-fade and can pre-empt it. Use sparingly: a small
         *  visible pop occurs at the seam unless the new blend duration is long. */
        PROPERTY(Editable, Category = "Blending")
        bool bCanInterrupt = false;

        // "Speed > 0.10", or "Always" when no parameter gates it.
        FString GetConditionText() const;
    };
}
