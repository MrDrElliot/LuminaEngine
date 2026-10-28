#include "EditorPCH.h"
#include "AudioNodes_Effects.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_Reverb::BuildNode()
    {
        CreateInputPin("In Left", EAudioGraphType::Audio);
        CreateInputPin("In Right", EAudioGraphType::Audio);
        CreateInputPin("Room Size", EAudioGraphType::Float);
        CreateInputPin("Damping", EAudioGraphType::Float);
        CreateInputPin("Width", EAudioGraphType::Float);
        CreateInputPin("Wet", EAudioGraphType::Float);
        CreateInputPin("Dry", EAudioGraphType::Float);

        CreateOutputPin("Out Left", EAudioGraphType::Audio);
        CreateOutputPin("Out Right", EAudioGraphType::Audio);
    }

    void CAudioNode_Chorus::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Rate", EAudioGraphType::Float);
        CreateInputPin("Depth", EAudioGraphType::Float);
        CreateInputPin("Delay", EAudioGraphType::Float);
        CreateInputPin("Feedback", EAudioGraphType::Float);
        CreateInputPin("Mix", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Bitcrusher::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Bits", EAudioGraphType::Float);
        CreateInputPin("Downsample", EAudioGraphType::Float);
        CreateInputPin("Mix", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Compressor::BuildNode()
    {
        CreateInputPin("In Left", EAudioGraphType::Audio);
        CreateInputPin("In Right", EAudioGraphType::Audio);
        CreateInputPin("Sidechain", EAudioGraphType::Audio);
        CreateInputPin("Use Sidechain", EAudioGraphType::Bool);
        CreateInputPin("Threshold", EAudioGraphType::Float);
        CreateInputPin("Ratio", EAudioGraphType::Float);
        CreateInputPin("Attack", EAudioGraphType::Float);
        CreateInputPin("Release", EAudioGraphType::Float);
        CreateInputPin("Makeup", EAudioGraphType::Float);
        CreateInputPin("Knee", EAudioGraphType::Float);

        CreateOutputPin("Out Left", EAudioGraphType::Audio);
        CreateOutputPin("Out Right", EAudioGraphType::Audio);
        CreateOutputPin("Gain Reduction", EAudioGraphType::Float);
    }

    void CAudioNode_EnvelopeFollower::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Attack", EAudioGraphType::Float);
        CreateInputPin("Release", EAudioGraphType::Float);

        CreateOutputPin("Envelope", EAudioGraphType::Float);
        CreateOutputPin("Signal", EAudioGraphType::Audio);
    }
}
