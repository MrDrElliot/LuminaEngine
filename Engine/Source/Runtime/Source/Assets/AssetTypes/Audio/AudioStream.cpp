#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AudioStream.h"

#include "Audio/AudioDecode.h"
#include "Core/Serialization/Archiver.h"
#include "Platform/Filesystem/FileHelper.h"

namespace Lumina
{
    void CAudioStream::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Audio");
        Super::Serialize(Ar);

        Ar << NumFrames;

        if (!AudioData)
        {
            AudioData = MakeShared<FAudioData>();
        }
        Ar << AudioData->Bytes;
    }

    void CAudioStream::PreSave()
    {
        Super::PreSave();

        if (Compression != EAudioCompression::QOA || !IsValid() || Audio::IsCompressed(AudioData->Bytes.GetData(), AudioData->Bytes.Num()))
        {
            return;
        }

        // A fresh buffer, since a playing voice may still hold the old one.
        TSharedPtr<FAudioData> Encoded = MakeShared<FAudioData>();
        if (Audio::Compress(AudioData->Bytes.GetData(), AudioData->Bytes.Num(), Encoded->Bytes))
        {
            LOG_INFO("CAudioStream '{}' compressed to QOA, {} KB from {} KB.", GetName(), Encoded->Bytes.Num() / 1024, AudioData->Bytes.Num() / 1024);
            AudioData = Move(Encoded);
        }
    }

    void CAudioStream::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);

        // QOA is lossy, so going back to uncompressed needs the source file the asset was imported from.
        if (Compression == EAudioCompression::Uncompressed && IsValid() && Audio::IsCompressed(AudioData->Bytes.GetData(), AudioData->Bytes.Num()))
        {
            TVector<uint8> Source;
            if (SourcePath.IsEmpty() || !FileHelper::LoadFileToArray(Source, *SourcePath) || Source.IsEmpty())
            {
                LOG_WARN("CAudioStream '{}' is stored as QOA and its source '{}' cannot be read, so it stays compressed.", GetName(), SourcePath);
                return;
            }
            TSharedPtr<FAudioData> Restored = MakeShared<FAudioData>();
            Restored->Bytes = Move(Source);
            AudioData = Move(Restored);
        }
    }
}
