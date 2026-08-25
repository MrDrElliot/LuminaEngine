#include "EditorPCH.h"
#include "AudioNodes_Envelope.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_ADSR::BuildNode()
    {
        CreateInputPin("Attack", EAudioGraphType::Trigger);
        CreateInputPin("Release", EAudioGraphType::Trigger);
        CreateInputPin("Attack Time", EAudioGraphType::Float);
        CreateInputPin("Decay Time", EAudioGraphType::Float);
        CreateInputPin("Sustain Level", EAudioGraphType::Float);
        CreateInputPin("Release Time", EAudioGraphType::Float);

        CreateOutputPin("Envelope", EAudioGraphType::Audio);
        CreateOutputPin("Level", EAudioGraphType::Float);
        CreateOutputPin("On Finished", EAudioGraphType::Trigger);
    }

    void CAudioNode_AttackDecay::BuildNode()
    {
        CreateInputPin("Trigger", EAudioGraphType::Trigger);
        CreateInputPin("Attack Time", EAudioGraphType::Float);
        CreateInputPin("Decay Time", EAudioGraphType::Float);
        CreateInputPin("Curve", EAudioGraphType::Float);

        CreateOutputPin("Envelope", EAudioGraphType::Audio);
        CreateOutputPin("On Finished", EAudioGraphType::Trigger);
    }

    void CAudioNode_LFO::BuildNode()
    {
        CreateInputPin("Frequency", EAudioGraphType::Float);
        CreateInputPin("Shape", EAudioGraphType::Int32);
        CreateInputPin("Amplitude", EAudioGraphType::Float);
        CreateInputPin("Offset", EAudioGraphType::Float);
        CreateInputPin("Sync", EAudioGraphType::Trigger);

        CreateOutputPin("Out", EAudioGraphType::Audio);
        CreateOutputPin("Value", EAudioGraphType::Float);
    }

    void CAudioNode_InterpToFloat::BuildNode()
    {
        CreateInputPin("Target", EAudioGraphType::Float);
        CreateInputPin("Half Life", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_SampleAndHoldFloat::BuildNode()
    {
        CreateInputPin("Trigger", EAudioGraphType::Trigger);
        CreateInputPin("Value", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }
}
