#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphNode.h"
#include "AudioNodes_Generators.generated.h"

namespace Lumina
{
    class CAudioStream;

    /** Spectral shape of a Noise node. */
    REFLECT()
    enum class EAudioNoiseType : uint8
    {
        White,
        Pink,
    };

    REFLECT()
    class CAudioNode_Sine : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Sine"; }
        FStringView GetNodeDisplayName() const override { return "Sine"; }
        FStringView GetNodeTooltip() const override { return "Band limited sine oscillator."; }
        FFixedString GetNodeCategory() const override { return "Generators"; }

        PROPERTY(Editable, Category = "Sine", ClampMin = 0.0f)
        float Frequency = 440.0f;
    };

    REFLECT()
    class CAudioNode_Saw : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Saw"; }
        FStringView GetNodeDisplayName() const override { return "Saw"; }
        FStringView GetNodeTooltip() const override { return "Sawtooth oscillator, anti aliased at the edge."; }
        FFixedString GetNodeCategory() const override { return "Generators"; }

        PROPERTY(Editable, Category = "Saw", ClampMin = 0.0f)
        float Frequency = 220.0f;
    };

    REFLECT()
    class CAudioNode_Square : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Square"; }
        FStringView GetNodeDisplayName() const override { return "Square"; }
        FStringView GetNodeTooltip() const override { return "Pulse oscillator with an adjustable duty cycle."; }
        FFixedString GetNodeCategory() const override { return "Generators"; }

        PROPERTY(Editable, Category = "Square", ClampMin = 0.0f)
        float Frequency = 220.0f;

        PROPERTY(Editable, Category = "Square", ClampMin = 0.01f, ClampMax = 0.99f)
        float PulseWidth = 0.5f;
    };

    REFLECT()
    class CAudioNode_Triangle : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Triangle"; }
        FStringView GetNodeDisplayName() const override { return "Triangle"; }
        FStringView GetNodeTooltip() const override { return "Triangle oscillator."; }
        FFixedString GetNodeCategory() const override { return "Generators"; }

        PROPERTY(Editable, Category = "Triangle", ClampMin = 0.0f)
        float Frequency = 220.0f;
    };

    REFLECT()
    class CAudioNode_Noise : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "Noise"; }
        FStringView GetNodeDisplayName() const override { return "Noise"; }
        FStringView GetNodeTooltip() const override { return "White or pink noise source."; }
        FFixedString GetNodeCategory() const override { return "Generators"; }

        PROPERTY(Editable, Category = "Noise")
        EAudioNoiseType Type = EAudioNoiseType::White;
    };

    REFLECT()
    class EDITOR_API CAudioNode_WavePlayer : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        FName GetOperatorName() const override { return "WavePlayer"; }
        FStringView GetNodeDisplayName() const override { return "Wave Player"; }
        FStringView GetNodeTooltip() const override { return "Plays a wave asset, resampled to the graph rate."; }
        FFixedString GetNodeCategory() const override { return "Generators"; }

        PROPERTY(Editable, Category = "Wave Player")
        TStrongObjectPtr<CAudioStream> Wave;

        PROPERTY(Editable, Category = "Wave Player")
        bool Loop = false;

        PROPERTY(Editable, Category = "Wave Player", ClampMin = 0.01f, ClampMax = 8.0f)
        float Pitch = 1.0f;

        PROPERTY(Editable, Category = "Wave Player", ClampMin = 0.0f, Units = "s")
        float StartTime = 0.0f;

        /** Where a loop jumps back to. */
        PROPERTY(Editable, Category = "Wave Player", ClampMin = 0.0f, Units = "s")
        float LoopStart = 0.0f;

        /** Length of the looped region, or zero to loop to the end of the wave. */
        PROPERTY(Editable, Category = "Wave Player", ClampMin = 0.0f, Units = "s")
        float LoopDuration = 0.0f;
    };

    REFLECT()
    class EDITOR_API CAudioNode_RandomWave : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        void GetNodeWaves(TVector<CAudioStream*>& OutWaves) const override;
        FName GetOperatorName() const override { return "RandomWave"; }
        FStringView GetNodeDisplayName() const override { return "Random Wave"; }
        FStringView GetNodeTooltip() const override { return "Hands a Wave Player one of several waves, picked at random, in turn, or by index. Picks once on the first block and again on each Next."; }
        FFixedString GetNodeCategory() const override { return "Generators"; }

        PROPERTY(Editable, Category = "Random Wave")
        TVector<TStrongObjectPtr<CAudioStream>> Waves;

        PROPERTY(Editable, Category = "Random Wave")
        EAudioPickMode Mode = EAudioPickMode::RandomNoRepeat;

        /** The wave to play in By Index mode, wrapped into the list. */
        PROPERTY(Editable, Category = "Random Wave")
        int32 Index = 0;

        PROPERTY(Editable, Category = "Random Wave")
        int32 Seed = 1;
    };

}
