#include "EditorPCH.h"
#include "AudioNodes_Triggers.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_TriggerRepeat::BuildNode()
    {
        CreateInputPin("Start", EAudioGraphType::Trigger);
        CreateInputPin("Stop", EAudioGraphType::Trigger);
        CreateInputPin("Period", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Trigger);
    }

    void CAudioNode_TriggerDelay::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Trigger);
        CreateInputPin("Delay", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Trigger);
    }

    void CAudioNode_TriggerOnce::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Trigger);
        CreateInputPin("Reset", EAudioGraphType::Trigger);

        CreateOutputPin("Out", EAudioGraphType::Trigger);
    }

    void CAudioNode_TriggerAny::BuildNode()
    {
        CreateInputPin("In 0", EAudioGraphType::Trigger);
        CreateInputPin("In 1", EAudioGraphType::Trigger);
        CreateInputPin("In 2", EAudioGraphType::Trigger);
        CreateInputPin("In 3", EAudioGraphType::Trigger);

        CreateOutputPin("Out", EAudioGraphType::Trigger);
    }

    void CAudioNode_TriggerCounter::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Trigger);
        CreateInputPin("Reset", EAudioGraphType::Trigger);
        CreateInputPin("Reset Count", EAudioGraphType::Int32);

        CreateOutputPin("Count", EAudioGraphType::Float);
        CreateOutputPin("Wrapped", EAudioGraphType::Trigger);
    }

    void CAudioNode_RandomFloat::BuildNode()
    {
        CreateInputPin("Next", EAudioGraphType::Trigger);
        CreateInputPin("Min", EAudioGraphType::Float);
        CreateInputPin("Max", EAudioGraphType::Float);
        CreateInputPin("Seed", EAudioGraphType::Int32);

        CreateOutputPin("Value", EAudioGraphType::Float);
        CreateOutputPin("On Next", EAudioGraphType::Trigger);
    }

    void CAudioNode_TriggerOnThreshold::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Float);
        CreateInputPin("Threshold", EAudioGraphType::Float);

        CreateOutputPin("On Rising", EAudioGraphType::Trigger);
        CreateOutputPin("On Falling", EAudioGraphType::Trigger);
    }
}
