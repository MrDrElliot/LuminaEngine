#include "EditorPCH.h"
#include "AudioNodes_Filters.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_OnePoleLowPass::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Cutoff", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_OnePoleHighPass::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Cutoff", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_BiquadFilter::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Type", EAudioGraphType::Int32);
        CreateInputPin("Frequency", EAudioGraphType::Float);
        CreateInputPin("Q", EAudioGraphType::Float);
        CreateInputPin("Gain", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Delay::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Delay Time", EAudioGraphType::Float);
        CreateInputPin("Feedback", EAudioGraphType::Float);
        CreateInputPin("Dry Level", EAudioGraphType::Float);
        CreateInputPin("Wet Level", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Saturation::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Drive", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }
}
