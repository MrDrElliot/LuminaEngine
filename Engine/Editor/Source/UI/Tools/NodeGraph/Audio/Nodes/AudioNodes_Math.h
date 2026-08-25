#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Math.generated.h"

namespace Lumina
{
    /** How Audio To Float reduces a block to one value. */
    REFLECT()
    enum class EAudioLevelMode : uint8
    {
        LastSample,
        Peak,
        RMS,
    };

    REFLECT()
    class CAudioNode_AddFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Add.Float"; }
        FStringView GetNodeDisplayName() const override { return "Add (Float)"; }
        FStringView GetNodeTooltip() const override { return "Returns A plus B."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Add (Float)")
        float A = 0.0f;

        PROPERTY(Editable, Category = "Add (Float)")
        float B = 0.0f;
    };

    REFLECT()
    class CAudioNode_SubtractFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Subtract.Float"; }
        FStringView GetNodeDisplayName() const override { return "Subtract (Float)"; }
        FStringView GetNodeTooltip() const override { return "Returns A minus B."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Subtract (Float)")
        float A = 0.0f;

        PROPERTY(Editable, Category = "Subtract (Float)")
        float B = 0.0f;
    };

    REFLECT()
    class CAudioNode_MultiplyFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Multiply.Float"; }
        FStringView GetNodeDisplayName() const override { return "Multiply (Float)"; }
        FStringView GetNodeTooltip() const override { return "Returns A times B."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Multiply (Float)")
        float A = 1.0f;

        PROPERTY(Editable, Category = "Multiply (Float)")
        float B = 1.0f;
    };

    REFLECT()
    class CAudioNode_DivideFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Divide.Float"; }
        FStringView GetNodeDisplayName() const override { return "Divide (Float)"; }
        FStringView GetNodeTooltip() const override { return "Returns A over B, or zero when B is zero."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Divide (Float)")
        float A = 1.0f;

        PROPERTY(Editable, Category = "Divide (Float)")
        float B = 1.0f;
    };

    REFLECT()
    class CAudioNode_MinFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Min.Float"; }
        FStringView GetNodeDisplayName() const override { return "Min (Float)"; }
        FStringView GetNodeTooltip() const override { return "Returns the smaller of A and B."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Min (Float)")
        float A = 0.0f;

        PROPERTY(Editable, Category = "Min (Float)")
        float B = 0.0f;
    };

    REFLECT()
    class CAudioNode_MaxFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Max.Float"; }
        FStringView GetNodeDisplayName() const override { return "Max (Float)"; }
        FStringView GetNodeTooltip() const override { return "Returns the larger of A and B."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Max (Float)")
        float A = 0.0f;

        PROPERTY(Editable, Category = "Max (Float)")
        float B = 0.0f;
    };

    REFLECT()
    class CAudioNode_PowerFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Power.Float"; }
        FStringView GetNodeDisplayName() const override { return "Power (Float)"; }
        FStringView GetNodeTooltip() const override { return "Raises Base to Exponent."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Power (Float)")
        float Base = 1.0f;

        PROPERTY(Editable, Category = "Power (Float)")
        float Exponent = 1.0f;
    };

    REFLECT()
    class CAudioNode_AddAudio : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Add.Audio"; }
        FStringView GetNodeDisplayName() const override { return "Add (Audio)"; }
        FStringView GetNodeTooltip() const override { return "Sums two signals."; }
        FFixedString GetNodeCategory() const override { return "Math"; }
    };

    REFLECT()
    class CAudioNode_SubtractAudio : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Subtract.Audio"; }
        FStringView GetNodeDisplayName() const override { return "Subtract (Audio)"; }
        FStringView GetNodeTooltip() const override { return "Subtracts B from A."; }
        FFixedString GetNodeCategory() const override { return "Math"; }
    };

    REFLECT()
    class CAudioNode_MultiplyAudio : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Multiply.Audio"; }
        FStringView GetNodeDisplayName() const override { return "Multiply (Audio)"; }
        FStringView GetNodeTooltip() const override { return "Ring modulation when both inputs are signals."; }
        FFixedString GetNodeCategory() const override { return "Math"; }
    };

    REFLECT()
    class CAudioNode_ClampFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Clamp.Float"; }
        FStringView GetNodeDisplayName() const override { return "Clamp (Float)"; }
        FStringView GetNodeTooltip() const override { return "Holds a value inside a range."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Clamp (Float)")
        float Value = 0.0f;

        PROPERTY(Editable, Category = "Clamp (Float)")
        float Min = 0.0f;

        PROPERTY(Editable, Category = "Clamp (Float)")
        float Max = 1.0f;
    };

    REFLECT()
    class CAudioNode_MapRangeFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "MapRange.Float"; }
        FStringView GetNodeDisplayName() const override { return "Map Range (Float)"; }
        FStringView GetNodeTooltip() const override { return "Rescales a value from one range to another."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Map Range (Float)")
        float Value = 0.0f;

        PROPERTY(Editable, Category = "Map Range (Float)")
        float InMin = 0.0f;

        PROPERTY(Editable, Category = "Map Range (Float)")
        float InMax = 1.0f;

        PROPERTY(Editable, Category = "Map Range (Float)")
        float OutMin = 0.0f;

        PROPERTY(Editable, Category = "Map Range (Float)")
        float OutMax = 1.0f;

        PROPERTY(Editable, Category = "Map Range (Float)")
        bool Clamp = true;
    };

    REFLECT()
    class CAudioNode_MidiToFrequency : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "MidiToFrequency"; }
        FStringView GetNodeDisplayName() const override { return "MIDI To Frequency"; }
        FStringView GetNodeTooltip() const override { return "Note 69 is 440 hertz."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "MIDI To Frequency")
        float Note = 69.0f;
    };

    REFLECT()
    class CAudioNode_DecibelsToLinear : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "DecibelsToLinear"; }
        FStringView GetNodeDisplayName() const override { return "Decibels To Linear"; }
        FStringView GetNodeTooltip() const override { return "Converts decibels to a gain multiplier."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Decibels To Linear")
        float Decibels = 0.0f;
    };

    REFLECT()
    class CAudioNode_LinearToDecibels : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "LinearToDecibels"; }
        FStringView GetNodeDisplayName() const override { return "Linear To Decibels"; }
        FStringView GetNodeTooltip() const override { return "Converts a gain multiplier to decibels."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Linear To Decibels")
        float Gain = 1.0f;
    };

    REFLECT()
    class CAudioNode_SemitonesToRatio : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "SemitonesToRatio"; }
        FStringView GetNodeDisplayName() const override { return "Semitones To Pitch Ratio"; }
        FStringView GetNodeTooltip() const override { return "Converts a semitone offset to a playback ratio."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Semitones To Pitch Ratio")
        float Semitones = 0.0f;
    };

    REFLECT()
    class CAudioNode_FloatToAudio : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "FloatToAudio"; }
        FStringView GetNodeDisplayName() const override { return "Float To Audio"; }
        FStringView GetNodeTooltip() const override { return "Ramps the value across the block, so an edit cannot click."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Float To Audio")
        float Value = 0.0f;
    };

    REFLECT()
    class CAudioNode_AudioToFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "AudioToFloat"; }
        FStringView GetNodeDisplayName() const override { return "Audio To Float"; }
        FStringView GetNodeTooltip() const override { return "Reduces a block of audio to one value."; }
        FFixedString GetNodeCategory() const override { return "Math"; }

        PROPERTY(Editable, Category = "Audio To Float")
        EAudioLevelMode Mode = EAudioLevelMode::RMS;
    };

}
