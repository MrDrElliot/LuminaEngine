#pragma once

#include "Containers/Algorithm.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "AudioGraphTypes.generated.h"

namespace Lumina
{
    // Fixed so operators size their scratch state once and the arena stays one contiguous allocation.
    inline constexpr uint32 kAudioGraphBlockFrames = 256;

    inline constexpr uint16 kAudioGraphInvalidSlot = 0xFFFFu;

    // Bumped when the compiled program layout changes, so a stale program is refused, not misparsed.
    inline constexpr uint16 kAudioGraphProgramVersion = 1;

    /** Value kind carried by a graph pin. The ordinal is baked into compiled programs, so append only. */
    REFLECT()
    enum class EAudioGraphType : uint8
    {
        Invalid,

        /** A block of kAudioGraphBlockFrames float samples. */
        Audio,

        /** One float, constant across the block. */
        Float,

        Int32,
        Bool,

        /** Sample-accurate events, as frame offsets within the block. */
        Trigger,

        /** A CAudioStream reference, resolved to decoded PCM by the node that plays it. */
        Wave,
    };

    RUNTIME_API const char* ToString(EAudioGraphType Type);

    /** How a node choosing among several options picks the next one. */
    REFLECT()
    enum class EAudioPickMode : uint8
    {
        Random,

        /** Random, but never the same option twice in a row. */
        RandomNoRepeat,

        Sequential,

        /** The option the Index pin names, wrapped into range. */
        ByIndex,
    };

    REFLECT()
    enum class EAudioCompareOp : uint8
    {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
    };

    REFLECT()
    enum class EAudioLogicOp : uint8
    {
        And,
        Or,
        Xor,
        NotA,
        Nand,
        Nor,
    };

    /** A graph input set before a voice starts, such as one an audio source component carries. */
    REFLECT()
    struct RUNTIME_API SAudioGraphParameterValue
    {
        GENERATED_BODY()

        /** The Graph Input this sets. */
        PROPERTY(Editable)
        FName Name;

        /** Read as the input's own type, so it sets a float, an int or a bool, or fires a trigger when nonzero. */
        PROPERTY(Editable)
        float Value = 0.0f;
    };

    /** Events fired inside one block, in ascending frame order. */
    struct FAudioGraphTriggerBuffer
    {
        // Inline rather than heap, because the audio thread must not allocate to fire an event.
        static constexpr uint32 MaxEvents = 32;

        uint32 Count = 0;
        uint32 Frames[MaxEvents] = {};

        void Reset() { Count = 0; }

        void Add(uint32 Frame)
        {
            if (Count < MaxEvents)
            {
                Frames[Count++] = Frame;
            }
        }

        bool IsEmpty() const { return Count == 0; }

        uint32 operator[](uint32 Index) const { return Frames[Index]; }
    };

    /** Fully decoded PCM for one wave the graph references, owned by the instance that plays it. */
    struct FAudioGraphWaveResource
    {
        /** Interleaved float samples at the source file's rate and channel count. */
        TVector<float> Samples;

        uint32 SampleRate  = 0;
        uint32 NumChannels = 0;
        uint64 NumFrames   = 0;

        bool IsValid() const { return NumChannels > 0 && NumFrames > 0 && !Samples.empty(); }
    };


    /** Walks a trigger buffer alongside a per sample loop, one Consume call per frame. */
    struct FAudioGraphTriggerCursor
    {
        uint32 Index = 0;

        void Reset() { Index = 0; }

        bool Consume(const FAudioGraphTriggerBuffer& Buffer, uint32 Frame)
        {
            bool bFired = false;
            while (Index < Buffer.Count && Buffer.Frames[Index] <= Frame)
            {
                ++Index;
                bFired = true;
            }
            return bFired;
        }
    };

    /** Ramps a block rate float across the block, so a parameter edit cannot click. */
    class FAudioGraphSmoothedFloat
    {
    public:

        void Reset() { bPrimed = false; Value = 0.0f; Step = 0.0f; Target = 0.0f; }

        void Begin(float InTarget, uint32 NumFrames)
        {
            Target = InTarget;

            if (!bPrimed)
            {
                bPrimed = true;
                Value   = InTarget;
                Step    = 0.0f;
                return;
            }

            Step = NumFrames != 0 ? (InTarget - Value) / (float)NumFrames : 0.0f;
        }

        FORCEINLINE float Next()
        {
            const float Current = Value;
            Value += Step;
            return Current;
        }

        /** Snaps to the target, so rounding across the ramp cannot drift. */
        void End() { Value = Target; Step = 0.0f; }

        float GetValue() const { return Value; }

    private:

        float Value  = 0.0f;
        float Step   = 0.0f;
        float Target = 0.0f;
        bool  bPrimed = false;
    };

    // Seeded, so a graph's random choices repeat for a given seed.
    struct FAudioGraphRandom
    {
        uint32 State = 0x9E3779B9u;
        bool   bSeeded = false;

        void Seed(int32 Seed) { State = (uint32)Seed * 2654435761u + 1u; bSeeded = true; }

        // Seed pins hold their value only once the instance applied its initial values, so seed on the first block.
        void SeedOnce(int32 InSeed)
        {
            if (!bSeeded)
            {
                Seed(InSeed);
            }
        }

        FORCEINLINE float NextUnit()
        {
            State ^= State << 13;
            State ^= State >> 17;
            State ^= State << 5;
            return (float)(State >> 8) * (1.0f / 16777216.0f);
        }

        FORCEINLINE uint32 NextIndex(uint32 Count) { return Count > 1 ? Math::Min((uint32)(NextUnit() * (float)Count), Count - 1) : 0u; }

        uint32 NextIndexExcept(uint32 Count, uint32 Previous)
        {
            if (Count < 2 || Previous >= Count)
            {
                return NextIndex(Count);
            }
            const uint32 Pick = NextIndex(Count - 1);
            return Pick >= Previous ? Pick + 1 : Pick;
        }
    };

    inline uint32 PickNextOption(EAudioPickMode Mode, uint32 Count, uint32 Current, int32 Index, FAudioGraphRandom& Random)
    {
        if (Count == 0)
        {
            return 0u;
        }

        switch (Mode)
        {
        case EAudioPickMode::RandomNoRepeat: return Random.NextIndexExcept(Count, Current);
        case EAudioPickMode::Sequential:     return (Current + 1) % Count;
        case EAudioPickMode::ByIndex:        return (uint32)(((Index % (int32)Count) + (int32)Count) % (int32)Count);
        default:                             return Random.NextIndex(Count);
        }
    }

    class FAudioGraphDelayLine
    {
    public:

        // Allocates, so call it while the operator is being built.
        void Initialize(uint32 MaxDelayFrames)
        {
            Buffer.assign((size_t)MaxDelayFrames + 2, 0.0f);
            WriteIndex = 0;
        }

        void Clear()
        {
            Algo::Fill(Buffer, 0.0f);
            WriteIndex = 0;
        }

        float GetMaxDelay() const { return Buffer.size() > 2 ? (float)(Buffer.size() - 2) : 0.0f; }

        // Delay counts frames back from the next write and may be fractional.
        FORCEINLINE float Read(float Delay) const
        {
            const size_t Capacity = Buffer.size();

            float Position = (float)WriteIndex - Delay;
            while (Position < 0.0f)
            {
                Position += (float)Capacity;
            }

            const size_t Index = (size_t)Position % Capacity;
            const size_t Next  = (Index + 1) % Capacity;
            const float  Alpha = Math::Fract(Position);
            return Buffer[Index] + (Buffer[Next] - Buffer[Index]) * Alpha;
        }

        FORCEINLINE void Write(float Sample)
        {
            Buffer[WriteIndex] = Sample;
            WriteIndex = (WriteIndex + 1) % Buffer.size();
        }

        bool IsEmpty() const { return Buffer.empty(); }

    private:

        TVector<float> Buffer;
        size_t         WriteIndex = 0;
    };

    /** Per-block state handed to every operator's Execute. */
    struct FAudioGraphBlockContext
    {
        /** Frames to process this call, never more than kAudioGraphBlockFrames. */
        uint32 NumFrames = kAudioGraphBlockFrames;

        uint32 SampleRate = 48000;

        float InverseSampleRate = 1.0f / 48000.0f;

        /** Seconds of audio this instance rendered before the block started. */
        double BlockStartTime = 0.0;
    };
}
