#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Effects.generated.h"

namespace Lumina
{
    REFLECT()
    class CAudioNode_Reverb : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Reverb"; }
        FStringView GetNodeDisplayName() const override { return "Reverb"; }
        FStringView GetNodeTooltip() const override { return "Stereo room reverb. Wire a mono source to both inputs."; }
        FFixedString GetNodeCategory() const override { return "Effects"; }

        PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
        float RoomSize = 0.5f;

        PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
        float Damping = 0.5f;

        PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f, ClampMax = 1.0f)
        float Width = 1.0f;

        PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f)
        float Wet = 0.35f;

        PROPERTY(Editable, Category = "Reverb", ClampMin = 0.0f)
        float Dry = 1.0f;
    };

    REFLECT()
    class CAudioNode_Chorus : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Chorus"; }
        FStringView GetNodeDisplayName() const override { return "Chorus"; }
        FStringView GetNodeTooltip() const override { return "Swept delay. Tens of milliseconds give chorus, a few milliseconds with feedback give flanging."; }
        FFixedString GetNodeCategory() const override { return "Effects"; }

        PROPERTY(Editable, Category = "Chorus", ClampMin = 0.0f, Units = "Hz")
        float Rate = 0.8f;

        PROPERTY(Editable, Category = "Chorus", ClampMin = 0.0f, ClampMax = 0.05f, Units = "s")
        float Depth = 0.004f;

        PROPERTY(Editable, Category = "Chorus", ClampMin = 0.0f, ClampMax = 0.05f, Units = "s")
        float Delay = 0.02f;

        PROPERTY(Editable, Category = "Chorus", ClampMin = -0.95f, ClampMax = 0.95f)
        float Feedback = 0.0f;

        PROPERTY(Editable, Category = "Chorus", ClampMin = 0.0f, ClampMax = 1.0f)
        float Mix = 0.5f;
    };

    REFLECT()
    class CAudioNode_Bitcrusher : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Bitcrusher"; }
        FStringView GetNodeDisplayName() const override { return "Bitcrusher"; }
        FStringView GetNodeTooltip() const override { return "Lowers bit depth and holds samples for a lo-fi, aliased sound."; }
        FFixedString GetNodeCategory() const override { return "Effects"; }

        PROPERTY(Editable, Category = "Bitcrusher", ClampMin = 1.0f, ClampMax = 24.0f)
        float Bits = 8.0f;

        /** Each sample is held this many frames. */
        PROPERTY(Editable, Category = "Bitcrusher", ClampMin = 1.0f, ClampMax = 64.0f)
        float Downsample = 4.0f;

        PROPERTY(Editable, Category = "Bitcrusher", ClampMin = 0.0f, ClampMax = 1.0f)
        float Mix = 1.0f;
    };

    REFLECT()
    class CAudioNode_Compressor : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Compressor"; }
        FStringView GetNodeDisplayName() const override { return "Compressor"; }
        FStringView GetNodeTooltip() const override { return "Stereo linked compressor. With Use Sidechain the Sidechain input drives the gain, which is how one sound ducks another."; }
        FFixedString GetNodeCategory() const override { return "Dynamics"; }

        PROPERTY(Editable, Category = "Compressor")
        bool UseSidechain = false;

        PROPERTY(Editable, Category = "Compressor", ClampMax = 0.0f, Units = "dB")
        float Threshold = -18.0f;

        PROPERTY(Editable, Category = "Compressor", ClampMin = 1.0f)
        float Ratio = 4.0f;

        PROPERTY(Editable, Category = "Compressor", ClampMin = 0.0f, Units = "s")
        float Attack = 0.005f;

        PROPERTY(Editable, Category = "Compressor", ClampMin = 0.0f, Units = "s")
        float Release = 0.15f;

        PROPERTY(Editable, Category = "Compressor", Units = "dB")
        float Makeup = 0.0f;

        PROPERTY(Editable, Category = "Compressor", ClampMin = 0.0f, Units = "dB")
        float Knee = 6.0f;
    };

    REFLECT()
    class CAudioNode_EnvelopeFollower : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "EnvelopeFollower"; }
        FStringView GetNodeDisplayName() const override { return "Envelope Follower"; }
        FStringView GetNodeTooltip() const override { return "Tracks a signal's level, as a float for gameplay or as a signal for modulation."; }
        FFixedString GetNodeCategory() const override { return "Dynamics"; }

        PROPERTY(Editable, Category = "Envelope Follower", ClampMin = 0.0f, Units = "s")
        float Attack = 0.01f;

        PROPERTY(Editable, Category = "Envelope Follower", ClampMin = 0.0f, Units = "s")
        float Release = 0.2f;
    };
}
