#pragma once

#include "Platform/GenericPlatform.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    struct FProceduralAudioStreamImpl;

    // Lock-free SPSC ring of PCM float samples: producer = Write caller (game/Lua), consumer = audio thread.
    // The miniaudio ring lives behind a Pimpl to keep miniaudio.h out of this header.
    class RUNTIME_API FProceduralAudioStream
    {
    public:

        FProceduralAudioStream(uint32 InSampleRate, uint32 InChannelCount, uint32 BufferFrames);
        ~FProceduralAudioStream();

        FProceduralAudioStream(const FProceduralAudioStream&) = delete;
        FProceduralAudioStream& operator=(const FProceduralAudioStream&) = delete;

        // Writes up to NumFrames of interleaved float samples. Returns frames actually written.
        uint32 Write(const float* Samples, uint32 NumFrames);

        uint32 GetAvailableWriteFrames();
        uint32 GetAvailableReadFrames();

        uint32 GetSampleRate() const { return SampleRate; }
        uint32 GetChannelCount() const { return ChannelCount; }

        // Underlying miniaudio data source (a ma_data_source*, which is void*); only the audio
        // thread should pass this to ma_sound. Returns null until successfully initialized.
        void* GetDataSource();

    private:

        TUniquePtr<FProceduralAudioStreamImpl> Impl;
        uint32 SampleRate = 0;
        uint32 ChannelCount = 0;
        bool bInitialized = false;
    };
}
