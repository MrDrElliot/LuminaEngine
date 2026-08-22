#pragma once

#include "Core/Object/Object.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "AnimStateTransition.generated.h"

namespace Lumina
{
    // One test inside a transition's rule.
    REFLECT()
    struct FAnimTransitionCondition
    {
        GENERATED_BODY()

        /** What the compare reads. Parameter and Curve name their source; the other two are implicit. */
        PROPERTY(Editable, Category = "Condition")
        EAnimTransitionSource ConditionSource = EAnimTransitionSource::Parameter;

        /** Graph parameter compared against Compare Value. Leave empty to make this term always pass. */
        PROPERTY(Editable, Category = "Condition", Picker = "Parameter", EditCondition = "ConditionSource == Parameter", EditConditionHides)
        FName ParameterName;

        /** Curve carried by the current state's pose, compared against Compare Value. */
        PROPERTY(Editable, Category = "Condition", Picker = "Curve", EditCondition = "ConditionSource == Curve", EditConditionHides)
        FName CurveName;

        /** How the source is tested against Compare Value. */
        PROPERTY(Editable, Category = "Condition")
        EAnimTransitionCompare Compare = EAnimTransitionCompare::Greater;

        /** Right-hand side of the comparison. Clip Finished reads 1 when the clip has ended, else 0. */
        PROPERTY(Editable, Category = "Condition")
        float CompareValue = 0.0f;

        // "Speed > 0.10", for the wire's badge.
        FString ToText() const;
    };

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

        /** Tests gating this transition. An empty list fires the moment the source state is active. */
        PROPERTY(Editable, Category = "Transition")
        TVector<FAnimTransitionCondition> Conditions;

        /** On, every condition must pass; off, any one of them does. */
        PROPERTY(Editable, Category = "Transition")
        bool bRequireAll = true;

        //~ Single-compare fields kept only to migrate a graph authored before rules were a list.
        PROPERTY()
        EAnimTransitionSource ConditionSource = EAnimTransitionSource::Parameter;

        PROPERTY()
        FName ConditionParameter;

        PROPERTY()
        EAnimTransitionCompare Compare = EAnimTransitionCompare::Greater;

        PROPERTY()
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

        // "Speed > 0.10", or "Always" when nothing gates it.
        FString GetConditionText() const;

        void PostLoad() override;

        // Folds a pre-list transition into one condition, then clears the old fields so it happens once.
        // Idempotent, and called from the compiler too, so no load path can drop an authored rule.
        void MigrateLegacyCondition();
    };
}
