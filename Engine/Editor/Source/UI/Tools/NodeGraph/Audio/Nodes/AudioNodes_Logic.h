#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Logic.generated.h"

namespace Lumina
{
    REFLECT()
    class CAudioNode_SelectAudio : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Select.Audio"; }
        FStringView GetNodeDisplayName() const override { return "Select (Audio)"; }
        FStringView GetNodeTooltip() const override { return "Plays the input Index names, fading over Fade seconds when it changes."; }
        FFixedString GetNodeCategory() const override { return "Logic"; }

        PROPERTY(Editable, Category = "Select (Audio)", ClampMin = 0, ClampMax = 3)
        int32 Index = 0;

        PROPERTY(Editable, Category = "Select (Audio)", ClampMin = 0.0f, Units = "s")
        float Fade = 0.05f;
    };

    REFLECT()
    class CAudioNode_SelectFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Select.Float"; }
        FStringView GetNodeDisplayName() const override { return "Select (Float)"; }
        FStringView GetNodeTooltip() const override { return "Returns the value Index names."; }
        FFixedString GetNodeCategory() const override { return "Logic"; }

        PROPERTY(Editable, Category = "Select (Float)", ClampMin = 0, ClampMax = 3)
        int32 Index = 0;

        PROPERTY(Editable, Category = "Select (Float)")
        float In0 = 0.0f;

        PROPERTY(Editable, Category = "Select (Float)")
        float In1 = 0.0f;

        PROPERTY(Editable, Category = "Select (Float)")
        float In2 = 0.0f;

        PROPERTY(Editable, Category = "Select (Float)")
        float In3 = 0.0f;
    };

    REFLECT()
    class CAudioNode_CompareFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Compare.Float"; }
        FStringView GetNodeDisplayName() const override { return "Compare (Float)"; }
        FStringView GetNodeTooltip() const override { return "True when A relates to B as Op says."; }
        FFixedString GetNodeCategory() const override { return "Logic"; }

        PROPERTY(Editable, Category = "Compare (Float)")
        float A = 0.0f;

        PROPERTY(Editable, Category = "Compare (Float)")
        float B = 0.0f;

        PROPERTY(Editable, Category = "Compare (Float)")
        EAudioCompareOp Op = EAudioCompareOp::Greater;
    };

    REFLECT()
    class CAudioNode_LogicBool : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Logic.Bool"; }
        FStringView GetNodeDisplayName() const override { return "Logic"; }
        FStringView GetNodeTooltip() const override { return "Combines two bools. Not A ignores B."; }
        FFixedString GetNodeCategory() const override { return "Logic"; }

        PROPERTY(Editable, Category = "Logic")
        bool A = false;

        PROPERTY(Editable, Category = "Logic")
        bool B = false;

        PROPERTY(Editable, Category = "Logic")
        EAudioLogicOp Op = EAudioLogicOp::And;
    };

    REFLECT()
    class CAudioNode_FloatToInt : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "FloatToInt"; }
        FStringView GetNodeDisplayName() const override { return "Float To Int"; }
        FStringView GetNodeTooltip() const override { return "Rounds to the nearest whole number."; }
        FFixedString GetNodeCategory() const override { return "Logic"; }

        PROPERTY(Editable, Category = "Float To Int")
        float Value = 0.0f;
    };

    REFLECT()
    class CAudioNode_IntToFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "IntToFloat"; }
        FStringView GetNodeDisplayName() const override { return "Int To Float"; }
        FStringView GetNodeTooltip() const override { return "Converts a whole number to a float."; }
        FFixedString GetNodeCategory() const override { return "Logic"; }

        PROPERTY(Editable, Category = "Int To Float")
        int32 Value = 0;
    };
}
