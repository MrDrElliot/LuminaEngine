#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
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
