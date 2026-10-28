#include "EditorPCH.h"
#include "AudioNodes_Logic.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_SelectAudio::BuildNode()
    {
        CreateInputPin("Index", EAudioGraphType::Int32);
        CreateInputPin("In 0", EAudioGraphType::Audio);
        CreateInputPin("In 1", EAudioGraphType::Audio);
        CreateInputPin("In 2", EAudioGraphType::Audio);
        CreateInputPin("In 3", EAudioGraphType::Audio);
        CreateInputPin("Fade", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_SelectFloat::BuildNode()
    {
        CreateInputPin("Index", EAudioGraphType::Int32);
        CreateInputPin("In 0", EAudioGraphType::Float);
        CreateInputPin("In 1", EAudioGraphType::Float);
        CreateInputPin("In 2", EAudioGraphType::Float);
        CreateInputPin("In 3", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_CompareFloat::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Float);
        CreateInputPin("B", EAudioGraphType::Float);
        CreateInputPin("Op", EAudioGraphType::Int32);

        CreateOutputPin("Out", EAudioGraphType::Bool);
    }

    void CAudioNode_LogicBool::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Bool);
        CreateInputPin("B", EAudioGraphType::Bool);
        CreateInputPin("Op", EAudioGraphType::Int32);

        CreateOutputPin("Out", EAudioGraphType::Bool);
    }

    void CAudioNode_FloatToInt::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Int32);
    }

    void CAudioNode_IntToFloat::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Int32);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }
}
