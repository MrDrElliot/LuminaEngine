#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "AudioStream.h"

#include "Core/Serialization/Archiver.h"

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
}
