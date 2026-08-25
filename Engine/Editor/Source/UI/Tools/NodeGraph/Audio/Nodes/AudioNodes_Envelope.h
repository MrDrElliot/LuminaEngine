#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Envelope.generated.h"

namespace Lumina
{
    /** Waveform an LFO traces. */
    REFLECT()
    enum class EAudioLFOShape : uint8
    {
        Sine,
        Saw,
        Square,
        Triangle,
    };

    REFLECT()
    class CAudioNode_ADSR : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "ADSR"; }
        FStringView GetNodeDisplayName() const override { return "ADSR Envelope"; }
        FStringView GetNodeTooltip() const override { return "Attack, decay, sustain and release envelope driven by two triggers."; }
        FFixedString GetNodeCategory() const override { return "Envelopes"; }

        PROPERTY(Editable, Category = "ADSR Envelope", ClampMin = 0.0f)
        float AttackTime = 0.01f;

        PROPERTY(Editable, Category = "ADSR Envelope", ClampMin = 0.0f)
        float DecayTime = 0.1f;

        PROPERTY(Editable, Category = "ADSR Envelope", ClampMin = 0.0f, ClampMax = 1.0f)
        float SustainLevel = 0.7f;

        PROPERTY(Editable, Category = "ADSR Envelope", ClampMin = 0.0f)
        float ReleaseTime = 0.3f;
    };

    REFLECT()
    class CAudioNode_AttackDecay : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "AttackDecay"; }
        FStringView GetNodeDisplayName() const override { return "Attack Decay Envelope"; }
        FStringView GetNodeTooltip() const override { return "One shot percussive envelope."; }
        FFixedString GetNodeCategory() const override { return "Envelopes"; }

        PROPERTY(Editable, Category = "Attack Decay Envelope", ClampMin = 0.0f)
        float AttackTime = 0.005f;

        PROPERTY(Editable, Category = "Attack Decay Envelope", ClampMin = 0.0f)
        float DecayTime = 0.25f;

        PROPERTY(Editable, Category = "Attack Decay Envelope", ClampMin = 0.01f)
        float Curve = 2.0f;
    };

    REFLECT()
    class CAudioNode_LFO : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "LFO"; }
        FStringView GetNodeDisplayName() const override { return "LFO"; }
        FStringView GetNodeTooltip() const override { return "Low frequency oscillator, usable as audio or as a control value."; }
        FFixedString GetNodeCategory() const override { return "Envelopes"; }

        PROPERTY(Editable, Category = "LFO", ClampMin = 0.0f)
        float Frequency = 2.0f;

        PROPERTY(Editable, Category = "LFO")
        EAudioLFOShape Shape = EAudioLFOShape::Sine;

        PROPERTY(Editable, Category = "LFO")
        float Amplitude = 1.0f;

        PROPERTY(Editable, Category = "LFO")
        float Offset = 0.0f;
    };

    REFLECT()
    class CAudioNode_InterpToFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "InterpTo.Float"; }
        FStringView GetNodeDisplayName() const override { return "Interp To (Float)"; }
        FStringView GetNodeTooltip() const override { return "Exponential smoothing toward a target, framerate independent."; }
        FFixedString GetNodeCategory() const override { return "Envelopes"; }

        PROPERTY(Editable, Category = "Interp To (Float)")
        float Target = 0.0f;

        PROPERTY(Editable, Category = "Interp To (Float)", ClampMin = 0.0f)
        float HalfLife = 0.1f;
    };

    REFLECT()
    class CAudioNode_SampleAndHoldFloat : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "SampleAndHold.Float"; }
        FStringView GetNodeDisplayName() const override { return "Sample And Hold (Float)"; }
        FStringView GetNodeTooltip() const override { return "Latches the input value each time the trigger fires."; }
        FFixedString GetNodeCategory() const override { return "Envelopes"; }

        PROPERTY(Editable, Category = "Sample And Hold (Float)")
        float Value = 0.0f;
    };

}
