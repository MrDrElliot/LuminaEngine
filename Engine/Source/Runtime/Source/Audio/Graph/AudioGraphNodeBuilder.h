#pragma once

#include "AudioGraphOperator.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"

namespace Lumina::AudioGraphOperators
{
    /** Pin ABI literal, so an operator's pin order reads next to the Execute that indexes it. */
    inline FAudioGraphOperatorSignature Signature(
        std::initializer_list<EAudioGraphType> Inputs,
        std::initializer_list<EAudioGraphType> Outputs)
    {
        FAudioGraphOperatorSignature Result;

        Result.Inputs.reserve(Inputs.size());
        for (EAudioGraphType Type : Inputs)
        {
            Result.Inputs.push_back(Type);
        }

        Result.Outputs.reserve(Outputs.size());
        for (EAudioGraphType Type : Outputs)
        {
            Result.Outputs.push_back(Type);
        }

        return Result;
    }

    // Prefixed, because a bare Audio collides with the Lumina::Audio namespace in a unity build.
    inline constexpr EAudioGraphType PinAudio   = EAudioGraphType::Audio;
    inline constexpr EAudioGraphType PinFloat   = EAudioGraphType::Float;
    inline constexpr EAudioGraphType PinInt32   = EAudioGraphType::Int32;
    inline constexpr EAudioGraphType PinBool    = EAudioGraphType::Bool;
    inline constexpr EAudioGraphType PinTrigger = EAudioGraphType::Trigger;
    inline constexpr EAudioGraphType PinWave    = EAudioGraphType::Wave;
}

/** Registers one operator at static init, binding its name and pin ABI to the class that renders it. */
#define LUMINA_AUDIO_GRAPH_OPERATOR(Symbol, OperatorClass, OperatorName, ...) \
    static const ::Lumina::FAudioGraphNodeRegistrar Symbol = []() -> ::Lumina::FAudioGraphNodeClass \
    { \
        using namespace ::Lumina::AudioGraphOperators; \
        ::Lumina::FAudioGraphNodeClass Class; \
        Class.Name      = OperatorName; \
        Class.Signature = Signature(__VA_ARGS__); \
        Class.Factory   = [](const ::Lumina::FAudioGraphOperatorBuildParams& Params) \
        { \
            return ::Lumina::MakeUnique<OperatorClass>(Params); \
        }; \
        return Class; \
    }();
