#pragma once
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Writes H.264 video into an MP4 file one RGBA frame at a time, through the encoder Windows ships with.
    class EDITOR_API FVideoEncoder
    {
    public:

        FVideoEncoder() = default;
        ~FVideoEncoder();

        FVideoEncoder(const FVideoEncoder&) = delete;
        FVideoEncoder& operator=(const FVideoEncoder&) = delete;

        // Width and Height must be even, since H.264 stores color at half resolution. A zero AudioSampleRate writes no sound track.
        bool Open(const FString& Path, uint32 InWidth, uint32 InHeight, uint32 InFrameRate, uint32 BitRate, FString& OutError,
                  uint32 InAudioSampleRate = 0, uint32 InAudioChannels = 0);

        // Interleaved float samples at the rate and channel count given to Open, which AAC stores as stereo or mono.
        bool WriteAudio(const float* Interleaved, uint32 FrameCount, FString& OutError);

        bool HasAudio() const { return AudioSampleRate != 0; }

        // Tightly packed RGBA8 at the size given to Open, top row first.
        bool WriteFrame(const uint8* RGBA, FString& OutError);

        // Finishes the file, after which it plays.
        bool Close(FString& OutError);

        bool IsOpen() const { return Writer != nullptr; }
        uint64 GetFramesWritten() const { return FrameIndex; }

    private:

        void*  Writer = nullptr;
        uint32 StreamIndex = 0;
        uint32 Width = 0;
        uint32 Height = 0;
        uint32 FrameRate = 30;
        uint64 FrameIndex = 0;
        uint32 AudioStreamIndex = 0;
        uint32 AudioSampleRate = 0;
        uint32 AudioSourceChannels = 0;
        uint32 AudioChannels = 0;
        uint64 AudioFramesWritten = 0;
        bool   bStartedMediaFoundation = false;
    };
}
