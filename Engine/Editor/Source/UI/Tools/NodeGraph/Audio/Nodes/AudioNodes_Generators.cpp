#include "EditorPCH.h"
#include "AudioNodes_Generators.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_Sine::BuildNode()
    {
        CreateInputPin("Frequency", EAudioGraphType::Float);
        CreateInputPin("Modulation", EAudioGraphType::Audio);
        CreateInputPin("Sync", EAudioGraphType::Trigger);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Saw::BuildNode()
    {
        CreateInputPin("Frequency", EAudioGraphType::Float);
        CreateInputPin("Modulation", EAudioGraphType::Audio);
        CreateInputPin("Sync", EAudioGraphType::Trigger);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Square::BuildNode()
    {
        CreateInputPin("Frequency", EAudioGraphType::Float);
        CreateInputPin("Modulation", EAudioGraphType::Audio);
        CreateInputPin("Sync", EAudioGraphType::Trigger);
        CreateInputPin("Pulse Width", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Triangle::BuildNode()
    {
        CreateInputPin("Frequency", EAudioGraphType::Float);
        CreateInputPin("Modulation", EAudioGraphType::Audio);
        CreateInputPin("Sync", EAudioGraphType::Trigger);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_Noise::BuildNode()
    {
        CreateInputPin("Type", EAudioGraphType::Int32);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_WavePlayer::BuildNode()
    {
        CreateInputPin("Wave", EAudioGraphType::Wave);
        CreateInputPin("Play", EAudioGraphType::Trigger);
        CreateInputPin("Stop", EAudioGraphType::Trigger);
        CreateInputPin("Loop", EAudioGraphType::Bool);
        CreateInputPin("Pitch", EAudioGraphType::Float);
        CreateInputPin("Start Time", EAudioGraphType::Float);

        CreateOutputPin("Out Left", EAudioGraphType::Audio);
        CreateOutputPin("Out Right", EAudioGraphType::Audio);
        CreateOutputPin("On Finished", EAudioGraphType::Trigger);
    }
}
