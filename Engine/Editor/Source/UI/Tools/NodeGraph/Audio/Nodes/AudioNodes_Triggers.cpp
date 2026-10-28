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

    void CAudioNode_TriggerSelect::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Trigger);
        CreateInputPin("Reset", EAudioGraphType::Trigger);
        CreateInputPin("Mode", EAudioGraphType::Int32);
        CreateInputPin("Index", EAudioGraphType::Int32);
        CreateInputPin("Count", EAudioGraphType::Int32);
        CreateInputPin("Seed", EAudioGraphType::Int32);

        CreateOutputPin("Out 0", EAudioGraphType::Trigger);
        CreateOutputPin("Out 1", EAudioGraphType::Trigger);
        CreateOutputPin("Out 2", EAudioGraphType::Trigger);
        CreateOutputPin("Out 3", EAudioGraphType::Trigger);
        CreateOutputPin("Selected", EAudioGraphType::Int32);
    }

    void CAudioNode_TriggerProbability::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Trigger);
        CreateInputPin("Probability", EAudioGraphType::Float);
        CreateInputPin("Seed", EAudioGraphType::Int32);

        CreateOutputPin("On Pass", EAudioGraphType::Trigger);
        CreateOutputPin("On Fail", EAudioGraphType::Trigger);
    }

    void CAudioNode_TriggerGate::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Trigger);
        CreateInputPin("Open", EAudioGraphType::Trigger);
        CreateInputPin("Close", EAudioGraphType::Trigger);
        CreateInputPin("Start Open", EAudioGraphType::Bool);

        CreateOutputPin("Out", EAudioGraphType::Trigger);
    }

    void CAudioNode_TriggerOnBool::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Bool);

        CreateOutputPin("On True", EAudioGraphType::Trigger);
        CreateOutputPin("On False", EAudioGraphType::Trigger);
    }

    void CAudioNode_RandomInt::BuildNode()
    {
        CreateInputPin("Next", EAudioGraphType::Trigger);
        CreateInputPin("Min", EAudioGraphType::Int32);
        CreateInputPin("Max", EAudioGraphType::Int32);
        CreateInputPin("Seed", EAudioGraphType::Int32);
        CreateInputPin("No Repeat", EAudioGraphType::Bool);

        CreateOutputPin("Value", EAudioGraphType::Int32);
        CreateOutputPin("On Next", EAudioGraphType::Trigger);
    }

    void CAudioNode_BeatClock::BuildNode()
    {
        CreateInputPin("Start", EAudioGraphType::Trigger);
        CreateInputPin("Stop", EAudioGraphType::Trigger);
        CreateInputPin("BPM", EAudioGraphType::Float);
        CreateInputPin("Beats Per Bar", EAudioGraphType::Int32);
        CreateInputPin("Subdivision", EAudioGraphType::Int32);

        CreateOutputPin("On Tick", EAudioGraphType::Trigger);
        CreateOutputPin("On Beat", EAudioGraphType::Trigger);
        CreateOutputPin("On Bar", EAudioGraphType::Trigger);
        CreateOutputPin("Beat", EAudioGraphType::Int32);
    }
}
