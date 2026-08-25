#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Filters.generated.h"

namespace Lumina
{
    /** Response a Biquad Filter applies. */
    REFLECT()
    enum class EAudioBiquadType : uint8
    {
        LowPass,
        HighPass,
        BandPass,
        Notch,
        Peaking,
        LowShelf,
        HighShelf,
    };

    REFLECT()
    class CAudioNode_OnePoleLowPass : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "OnePoleLowPass"; }
        FStringView GetNodeDisplayName() const override { return "One Pole Low Pass"; }
        FStringView GetNodeTooltip() const override { return "Cheapest smoothing filter, 6 decibels per octave."; }
        FFixedString GetNodeCategory() const override { return "Filters"; }

        PROPERTY(Editable, Category = "One Pole Low Pass", ClampMin = 1.0f)
        float Cutoff = 1000.0f;
    };

    REFLECT()
    class CAudioNode_OnePoleHighPass : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "OnePoleHighPass"; }
        FStringView GetNodeDisplayName() const override { return "One Pole High Pass"; }
        FStringView GetNodeTooltip() const override { return "Cheapest high pass, 6 decibels per octave."; }
        FFixedString GetNodeCategory() const override { return "Filters"; }

        PROPERTY(Editable, Category = "One Pole High Pass", ClampMin = 1.0f)
        float Cutoff = 200.0f;
    };

    REFLECT()
    class CAudioNode_BiquadFilter : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "BiquadFilter"; }
        FStringView GetNodeDisplayName() const override { return "Biquad Filter"; }
        FStringView GetNodeTooltip() const override { return "Resonant two pole filter. Gain applies to the peaking and shelving types."; }
        FFixedString GetNodeCategory() const override { return "Filters"; }

        PROPERTY(Editable, Category = "Biquad Filter")
        EAudioBiquadType Type = EAudioBiquadType::LowPass;

        PROPERTY(Editable, Category = "Biquad Filter", ClampMin = 10.0f)
        float Frequency = 1000.0f;

        PROPERTY(Editable, Category = "Biquad Filter", ClampMin = 0.05f)
        float Q = 0.707f;

        PROPERTY(Editable, Category = "Biquad Filter", Units = "dB")
        float Gain = 0.0f;
    };

    REFLECT()
    class CAudioNode_Delay : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Delay"; }
        FStringView GetNodeDisplayName() const override { return "Delay"; }
        FStringView GetNodeTooltip() const override { return "Feedback delay line. Delay time is capped at two seconds."; }
        FFixedString GetNodeCategory() const override { return "Filters"; }

        PROPERTY(Editable, Category = "Delay", ClampMin = 0.0f, ClampMax = 2.0f)
        float DelayTime = 0.25f;

        PROPERTY(Editable, Category = "Delay", ClampMin = 0.0f, ClampMax = 0.99f)
        float Feedback = 0.4f;

        PROPERTY(Editable, Category = "Delay")
        float DryLevel = 1.0f;

        PROPERTY(Editable, Category = "Delay")
        float WetLevel = 0.5f;
    };

    REFLECT()
    class CAudioNode_Saturation : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Saturation"; }
        FStringView GetNodeDisplayName() const override { return "Saturation"; }
        FStringView GetNodeTooltip() const override { return "Soft clipping overdrive."; }
        FFixedString GetNodeCategory() const override { return "Filters"; }

        PROPERTY(Editable, Category = "Saturation", ClampMin = 0.01f)
        float Drive = 1.0f;
    };

}
