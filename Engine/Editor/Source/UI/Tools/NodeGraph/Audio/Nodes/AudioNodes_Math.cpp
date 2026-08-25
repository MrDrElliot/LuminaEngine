#include "EditorPCH.h"
#include "AudioNodes_Math.h"

#include "UI/Tools/NodeGraph/Audio/AudioGraphPin.h"

namespace Lumina
{
    void CAudioNode_AddFloat::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Float);
        CreateInputPin("B", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_SubtractFloat::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Float);
        CreateInputPin("B", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_MultiplyFloat::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Float);
        CreateInputPin("B", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_DivideFloat::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Float);
        CreateInputPin("B", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_MinFloat::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Float);
        CreateInputPin("B", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_MaxFloat::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Float);
        CreateInputPin("B", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_PowerFloat::BuildNode()
    {
        CreateInputPin("Base", EAudioGraphType::Float);
        CreateInputPin("Exponent", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_AddAudio::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Audio);
        CreateInputPin("B", EAudioGraphType::Audio);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_SubtractAudio::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Audio);
        CreateInputPin("B", EAudioGraphType::Audio);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_MultiplyAudio::BuildNode()
    {
        CreateInputPin("A", EAudioGraphType::Audio);
        CreateInputPin("B", EAudioGraphType::Audio);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_ClampFloat::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Float);
        CreateInputPin("Min", EAudioGraphType::Float);
        CreateInputPin("Max", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_MapRangeFloat::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Float);
        CreateInputPin("In Min", EAudioGraphType::Float);
        CreateInputPin("In Max", EAudioGraphType::Float);
        CreateInputPin("Out Min", EAudioGraphType::Float);
        CreateInputPin("Out Max", EAudioGraphType::Float);
        CreateInputPin("Clamp", EAudioGraphType::Bool);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }

    void CAudioNode_MidiToFrequency::BuildNode()
    {
        CreateInputPin("Note", EAudioGraphType::Float);

        CreateOutputPin("Frequency", EAudioGraphType::Float);
    }

    void CAudioNode_DecibelsToLinear::BuildNode()
    {
        CreateInputPin("Decibels", EAudioGraphType::Float);

        CreateOutputPin("Gain", EAudioGraphType::Float);
    }

    void CAudioNode_LinearToDecibels::BuildNode()
    {
        CreateInputPin("Gain", EAudioGraphType::Float);

        CreateOutputPin("Decibels", EAudioGraphType::Float);
    }

    void CAudioNode_SemitonesToRatio::BuildNode()
    {
        CreateInputPin("Semitones", EAudioGraphType::Float);

        CreateOutputPin("Ratio", EAudioGraphType::Float);
    }

    void CAudioNode_FloatToAudio::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Float);

        CreateOutputPin("Out", EAudioGraphType::Audio);
    }

    void CAudioNode_AudioToFloat::BuildNode()
    {
        CreateInputPin("In", EAudioGraphType::Audio);
        CreateInputPin("Mode", EAudioGraphType::Int32);

        CreateOutputPin("Out", EAudioGraphType::Float);
    }
}
