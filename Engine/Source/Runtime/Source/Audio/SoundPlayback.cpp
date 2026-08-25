#include "RuntimePCH.h"
#include "SoundPlayback.h"

#include "Assets/AssetTypes/Audio/AudioGraph.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/AssetTypes/Audio/SoundBase.h"
#include "Audio/AudioGlobals.h"
#include "Audio/Graph/AudioGraphInstance.h"
#include "Core/Object/Cast.h"

namespace Lumina::Audio
{
    FSoundPlayResult PlaySound(CSoundBase* Sound, const FAudioPlayParams& Params)
    {
        FSoundPlayResult Result;

        if (Sound == nullptr || !Sound->IsPlayable() || !HasDevice())
        {
            return Result;
        }

        if (CAudioGraph* Graph = Cast<CAudioGraph>(Sound))
        {
            const FAudioDeviceInfo DeviceInfo = Context().GetDeviceInfo();
            const uint32 SampleRate = DeviceInfo.SampleRate != 0 ? DeviceInfo.SampleRate : 48000;

            Result.GraphInstance = Graph->CreateInstance(SampleRate, 2);
            if (!Result.GraphInstance)
            {
                return Result;
            }

            Result.Handle = Context().PlayAudioGraph(Result.GraphInstance, Params);
            if (!Result.Handle.IsValid())
            {
                Result.GraphInstance.reset();
            }

            return Result;
        }

        if (CAudioStream* Stream = Cast<CAudioStream>(Sound))
        {
            Result.Handle = Context().PlayAudio(Stream->GetAudioData(), Params);
        }

        return Result;
    }
}
