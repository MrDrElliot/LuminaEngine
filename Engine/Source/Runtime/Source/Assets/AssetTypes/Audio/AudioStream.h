#pragma once
#include "Audio/AudioTypes.h"
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "SoundBase.h"
#include "Memory/SmartPtr.h"
#include "AudioStream.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EAudioCompression : uint8
    {
        // About a fifth of 16 bit PCM, decoded a frame at a time during playback.
        QOA,
        // The imported file kept bit for bit.
        Uncompressed,
    };

    // Self-contained audio asset with the encoded bytes embedded, shared with the audio thread through GetAudioData() for any in-flight sound.
    REFLECT()
    class RUNTIME_API CAudioStream : public CSoundBase
    {
        GENERATED_BODY()
    public:

        void Serialize(FArchive& Ar) override;

        // Encodes the bytes when the setting asks for compression and they are still PCM, so a save, an import and a cook all store QOA.
        void PreSave() override;

        void PostPropertyChange(FProperty* ChangedProperty) override;

        // Bumped whenever a save encodes differently, so cooked audio misses the cook cache and is encoded again.
        static constexpr uint64 kCookedEncodingVersion = 1;

        bool IsPlayable() const override { return IsValid(); }

        bool IsValid() const { return AudioData && !AudioData->Bytes.IsEmpty(); }
        const TSharedPtr<FAudioData>& GetAudioData() const { return AudioData; }

        float GetDuration() const override { return SampleRate != 0 ? (float)((double)NumFrames / (double)SampleRate) : 0.0f; }

        // Source file the audio was imported from; empty for in-place created assets.
        PROPERTY(Hidden)
        FString SourcePath;

        PROPERTY(Editable, Category = "Audio")
        EAudioCompression Compression = EAudioCompression::QOA;

        PROPERTY(Hidden)
        uint32 SampleRate = 0;

        PROPERTY(Hidden)
        uint32 NumChannels = 0;

        uint64 NumFrames = 0;

        TSharedPtr<FAudioData> AudioData;
    };
}
