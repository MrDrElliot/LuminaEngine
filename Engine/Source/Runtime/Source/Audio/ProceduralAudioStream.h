#pragma once

#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Lock-free SPSC ring of interleaved float samples; the producer is the Write caller, the consumer is the audio thread.
    class RUNTIME_API FProceduralAudioStream
    {
    public:

        FProceduralAudioStream(uint32 InSampleRate, uint32 InChannelCount, uint32 BufferFrames);

        FProceduralAudioStream(const FProceduralAudioStream&) = delete;
        FProceduralAudioStream& operator=(const FProceduralAudioStream&) = delete;

        bool IsValid() const { return bInitialized; }

        // Writes up to NumFrames of interleaved float samples. Returns frames actually written.
        uint32 Write(const float* Samples, uint32 NumFrames);

        // Fills NumFrames, padding an underrun with silence, and returns how many carried real data.
        uint32 Read(float* Out, uint32 NumFrames);

        uint32 GetAvailableWriteFrames() const;
        uint32 GetAvailableReadFrames() const;

        uint32 GetSampleRate() const { return SampleRate; }
        uint32 GetChannelCount() const { return ChannelCount; }

    private:

        TVector<float> Storage;

        // Monotonic frame counts, so full and empty stay distinguishable without wasting a slot.
        TAtomic<uint64> WriteCursor{0};
        TAtomic<uint64> ReadCursor{0};

        uint32 CapacityFrames = 0;
        uint32 SampleRate = 0;
        uint32 ChannelCount = 0;
        bool bInitialized = false;
    };
}
