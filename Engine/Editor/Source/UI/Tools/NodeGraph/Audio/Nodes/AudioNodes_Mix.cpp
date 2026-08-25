#include "EditorPCH.h"
#include "AudioNodes_Mix.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_Gain::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Gain", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Mixer::BuildNode()
    {
        CreateInputPin("In 0", EAudioGraphType::Audio);
        CreateInputPin("Gain 0", EAudioGraphType::Float);
        CreateInputPin("In 1", EAudioGraphType::Audio);
        CreateInputPin("Gain 1", EAudioGraphType::Float);
        CreateInputPin("In 2", EAudioGraphType::Audio);
        CreateInputPin("Gain 2", EAudioGraphType::Float);
        CreateInputPin("In 3", EAudioGraphType::Audio);
        CreateInputPin("Gain 3", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Panner::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Pan", EAudioGraphType::Float);

        CreateOutputPin("Out Left", EAudioGraphType::Audio);
        CreateOutputPin("Out Right", EAudioGraphType::Audio);
    }

    void CAudioNode_Crossfade::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Audio);
        CreateInputPin("B", EAudioGraphType::Audio);
        CreateInputPin("Alpha", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_StereoWidth::BuildNode()
    {
        CreateInputPin("In Left", EAudioGraphType::Audio);
        CreateInputPin("In Right", EAudioGraphType::Audio);
        CreateInputPin("Width", EAudioGraphType::Float);

        CreateOutputPin("Out Left", EAudioGraphType::Audio);
        CreateOutputPin("Out Right", EAudioGraphType::Audio);
    }
}
