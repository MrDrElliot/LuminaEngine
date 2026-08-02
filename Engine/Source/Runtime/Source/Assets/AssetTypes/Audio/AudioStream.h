#pragma once
#include "Audio/AudioTypes.h"
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "Memory/SmartPtr.h"
#include "AudioStream.generated.h"

namespace Lumina
{
    // Self-contained audio asset; the encoded source bytes (wav) are embedded so cooked builds
    // need no source file. Playback shares the bytes with the audio thread via GetAudioData(),
    // which keeps them alive for the duration of any in-flight sound.
    REFLECT()
    class RUNTIME_API CAudioStream : public CObject
    {
        GENERATED_BODY()
    public:

        void Serialize(FArchive& Ar) override;
        bool IsAsset() const override { return true; }

        bool IsValid() const { return AudioData && !AudioData->Bytes.empty(); }
        const TSharedPtr<FAudioData>& GetAudioData() const { return AudioData; }

        float GetDuration() const { return SampleRate != 0 ? (float)((double)NumFrames / (double)SampleRate) : 0.0f; }

        // Source file the audio was imported from; empty for in-place created assets.
        PROPERTY()
        FString SourcePath;

        PROPERTY()
        uint32 SampleRate = 0;

        PROPERTY()
        uint32 NumChannels = 0;

        uint64 NumFrames = 0;

        TSharedPtr<FAudioData> AudioData;
    };
}
