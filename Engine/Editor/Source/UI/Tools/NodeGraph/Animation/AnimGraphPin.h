#pragma once

#include "Containers/Function.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "AnimGraphPin.generated.h"

namespace Lumina
{
    enum class EAnimPinType : uint8
    {
        Pose,       // a blended skeletal pose flowing between nodes
        Value,      // a scalar float (blend alphas, playback speeds, parameter outputs)
        StateFlow,  // a state-machine edge: connects State nodes on the state machine canvas
        Object,     // an asset reference (animation, blend space) a sampler node can swap at runtime
    };

    // Typed pin for the animation node graph. The graph schema only permits
    // connections between pins of the same EAnimPinType.
    REFLECT()
    class CAnimGraphPin : public CEdNodeGraphPin
    {
        GENERATED_BODY()
    public:

        void SetPinType(EAnimPinType InType);
        EAnimPinType GetPinType() const { return PinType; }

        // Draws the inline editor (when set) for an unconnected input pin, so its value/enum
        // is editable on the node face. Bound via CAnimGraphNode::BindFloatPinEditor / BindEnumPinEditor.
        float DrawPin() override;
        bool HasInlineEditor() const override { return (bool)InlineEditor; }

        /** Compact widget drawn in the pin row when this input pin is unconnected.
         *  Set by the owning node in BuildNode; not serialized (rebuilt each load). */
        TFunction<void()> InlineEditor;

        /** Fed into the compiler as a LoadConst when a Value input is left unconnected. */
        float DefaultValue = 0.0f;

        /** When true, this input pin accepts more than one incoming wire (e.g. the
         *  State Machine node's "States" pin). The anim graph schema honors this. */
        bool bAllowMultipleConnections = false;

        EAnimPinType PinType = EAnimPinType::Value;
    };
}
