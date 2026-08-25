#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Mix.generated.h"

namespace Lumina
{
    REFLECT()
    class CAudioNode_Gain : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Gain"; }
        FStringView GetNodeDisplayName() const override { return "Gain"; }
        FStringView GetNodeTooltip() const override { return "Scales a signal, ramping the gain across the block so an edit cannot click."; }
        FFixedString GetNodeCategory() const override { return "Mix"; }

        PROPERTY(Editable, Category = "Gain")
        float Gain = 1.0f;
    };

    REFLECT()
    class CAudioNode_Mixer : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Mixer"; }
        FStringView GetNodeDisplayName() const override { return "Mixer (4)"; }
        FStringView GetNodeTooltip() const override { return "Sums four signals with per input gain."; }
        FFixedString GetNodeCategory() const override { return "Mix"; }

        PROPERTY(Editable, Category = "Mixer (4)")
        float Gain0 = 1.0f;

        PROPERTY(Editable, Category = "Mixer (4)")
        float Gain1 = 1.0f;

        PROPERTY(Editable, Category = "Mixer (4)")
        float Gain2 = 1.0f;

        PROPERTY(Editable, Category = "Mixer (4)")
        float Gain3 = 1.0f;
    };

    REFLECT()
    class CAudioNode_Panner : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Panner"; }
        FStringView GetNodeDisplayName() const override { return "Panner"; }
        FStringView GetNodeTooltip() const override { return "Equal power pan of a mono signal. Minus one is hard left."; }
        FFixedString GetNodeCategory() const override { return "Mix"; }

        PROPERTY(Editable, Category = "Panner", ClampMin = -1.0f, ClampMax = 1.0f)
        float Pan = 0.0f;
    };

    REFLECT()
    class CAudioNode_Crossfade : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Crossfade"; }
        FStringView GetNodeDisplayName() const override { return "Crossfade"; }
        FStringView GetNodeTooltip() const override { return "Blends between two signals."; }
        FFixedString GetNodeCategory() const override { return "Mix"; }

        PROPERTY(Editable, Category = "Crossfade", ClampMin = 0.0f, ClampMax = 1.0f)
        float Alpha = 0.0f;
    };

    REFLECT()
    class CAudioNode_StereoWidth : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "StereoWidth"; }
        FStringView GetNodeDisplayName() const override { return "Stereo Width"; }
        FStringView GetNodeTooltip() const override { return "Scales the side signal. Zero collapses to mono, above one widens."; }
        FFixedString GetNodeCategory() const override { return "Mix"; }

        PROPERTY(Editable, Category = "Stereo Width", ClampMin = 0.0f)
        float Width = 1.0f;
    };

}
