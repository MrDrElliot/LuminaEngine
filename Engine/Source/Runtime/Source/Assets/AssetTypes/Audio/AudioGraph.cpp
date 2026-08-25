#include "RuntimePCH.h"
#include "AudioGraph.h"

#include "AudioStream.h"
#include "Audio/AudioDecode.h"
#include "Core/Serialization/Archiver.h"
#include "Log/Log.h"
#include "Memory/MemoryTracking.h"

namespace Lumina
{
    void CAudioGraph::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Audio");
        Super::Serialize(Ar);

        Ar << Program;

        if (Ar.IsReading())
        {
            ReleaseDecodedWaves();
        }
    }

    void CAudioGraph::SetProgram(FAudioGraphProgram&& InProgram, TVector<TObjectPtr<CAudioStream>>&& InWaves)
    {
        Program         = Move(InProgram);
        ReferencedWaves = Move(InWaves);
        ReleaseDecodedWaves();
    }

    void CAudioGraph::ReleaseDecodedWaves()
    {
        DecodedWaves.clear();
        bWavesDecoded = false;
    }

    void CAudioGraph::EnsureWavesDecoded()
    {
        if (bWavesDecoded)
        {
            return;
        }

        LUMINA_MEMORY_SCOPE("Audio");
        bWavesDecoded = true;

        DecodedWaves.clear();
        DecodedWaves.resize(ReferencedWaves.size());

        for (size_t Index = 0; Index < ReferencedWaves.size(); ++Index)
        {
            CAudioStream* Stream = ReferencedWaves[Index];
            if (Stream == nullptr || !Stream->IsValid())
            {
                continue;
            }

            const TSharedPtr<FAudioData>& Data = Stream->GetAudioData();

            Audio::FAudioInfo Info;
            TSharedPtr<FAudioGraphWaveResource> Resource = MakeShared<FAudioGraphWaveResource>();

            if (!Audio::DecodePCM(Data->Bytes.data(), Data->Bytes.size(), Info, Resource->Samples))
            {
                LOG_WARN("AudioGraph: failed to decode wave '{}'", Stream->GetName().c_str());
                continue;
            }

            Resource->SampleRate  = Info.SampleRate;
            Resource->NumChannels = Info.NumChannels;
            Resource->NumFrames   = Info.NumFrames;

            DecodedWaves[Index] = Move(Resource);
        }
    }

    TSharedPtr<FAudioGraphInstance> CAudioGraph::CreateInstance(uint32 SampleRate, uint32 NumChannels)
    {
        if (!Program.IsValid())
        {
            return nullptr;
        }

        EnsureWavesDecoded();

        TSharedPtr<FAudioGraphInstance> Instance = MakeShared<FAudioGraphInstance>();
        if (!Instance->Initialize(Program, DecodedWaves, SampleRate, NumChannels))
        {
            return nullptr;
        }

        return Instance;
    }
}
