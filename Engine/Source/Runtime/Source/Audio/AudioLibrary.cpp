#include "RuntimePCH.h"

#include "AudioLibrary.h"

#include "Audio/AudioContext.h"
#include "Audio/AudioGlobals.h"
#include "Audio/AudioReverb.h"
#include "Audio/AudioSettings.h"
#include "Audio/SoundPlayback.h"
#include "Config/Config.h"

namespace Lumina
{
    namespace
    {
        // Clamped rather than trusted, since an enum crossing from script can hold anything its width allows.
        EAudioBus SafeBus(EAudioBus Bus)
        {
            const int32 Index = Math::Clamp((int32)Bus, 0, (int32)NumAudioBuses - 1);
            return (EAudioBus)Index;
        }
    }

    SAudioHandle CAudioLibrary::PlaySound2D(CSoundBase* Sound, float Volume, float Pitch, bool bLoop)
    {
        SAudioPlayParams Params;
        Params.Volume = Volume;
        Params.Pitch = Pitch;
        Params.bLooping = bLoop;
        return Audio::PlaySound(Sound, Params).Handle;
    }

    SAudioHandle CAudioLibrary::PlaySoundAtLocation(CSoundBase* Sound, FVector3 Location, float Volume,
        float Pitch, float MinDistance, float MaxDistance, bool bLoop)
    {
        SAudioPlayParams Params;
        Params.Volume = Volume;
        Params.Pitch = Pitch;
        Params.bLooping = bLoop;
        Params.bSpatialized = true;
        Params.Position = Location;
        Params.Attenuation.MinDistance = MinDistance;
        Params.Attenuation.MaxDistance = MaxDistance;
        return Audio::PlaySound(Sound, Params).Handle;
    }

    SAudioHandle CAudioLibrary::PlaySoundEx(CSoundBase* Sound, SAudioPlayParams Params)
    {
        Params.Bus = SafeBus(Params.Bus);
        return Audio::PlaySound(Sound, Params).Handle;
    }

    SAudioPlayParams CAudioLibrary::DefaultPlayParams()
    {
        return SAudioPlayParams();
    }

    void CAudioLibrary::Stop(SAudioHandle Handle, bool bAllowFadeOut, float FadeSeconds)
    {
        Audio::Context().StopSound(Handle,
            bAllowFadeOut ? EAudioStopMode::AllowFadeOut : EAudioStopMode::Immediate, FadeSeconds);
    }

    void CAudioLibrary::StopAll(bool bAllowFadeOut, float FadeSeconds)
    {
        Audio::Context().StopAllSounds(
            bAllowFadeOut ? EAudioStopMode::AllowFadeOut : EAudioStopMode::Immediate, FadeSeconds);
    }

    void CAudioLibrary::SetVolume(SAudioHandle Handle, float Volume)
    {
        Audio::Context().SetVolume(Handle, Volume);
    }

    void CAudioLibrary::SetPitch(SAudioHandle Handle, float Pitch)
    {
        Audio::Context().SetPitch(Handle, Pitch);
    }

    void CAudioLibrary::SetLooping(SAudioHandle Handle, bool bLoop)
    {
        Audio::Context().SetLooping(Handle, bLoop);
    }

    void CAudioLibrary::SetPaused(SAudioHandle Handle, bool bPaused)
    {
        Audio::Context().SetPaused(Handle, bPaused);
    }

    void CAudioLibrary::SetPan(SAudioHandle Handle, float Pan)
    {
        Audio::Context().SetPan(Handle, Pan);
    }

    void CAudioLibrary::SetBus(SAudioHandle Handle, EAudioBus Bus)
    {
        Audio::Context().SetBus(Handle, SafeBus(Bus));
    }

    void CAudioLibrary::FadeTo(SAudioHandle Handle, float Volume, float Seconds)
    {
        Audio::Context().FadeTo(Handle, Volume, Seconds);
    }

    void CAudioLibrary::SeekToFrame(SAudioHandle Handle, uint64 Frame)
    {
        Audio::Context().SeekToFrame(Handle, Frame);
    }

    EAudioVoiceState CAudioLibrary::GetVoiceState(SAudioHandle Handle)
    {
        return Audio::Context().GetVoiceState(Handle);
    }

    uint64 CAudioLibrary::GetPlaybackFrame(SAudioHandle Handle)
    {
        return Audio::Context().GetPlaybackFrame(Handle);
    }

    void CAudioLibrary::SetPosition(SAudioHandle Handle, FVector3 Position)
    {
        Audio::Context().SetPosition(Handle, Position);
    }

    void CAudioLibrary::SetVelocity(SAudioHandle Handle, FVector3 Velocity)
    {
        Audio::Context().SetVelocity(Handle, Velocity);
    }

    void CAudioLibrary::SetDirection(SAudioHandle Handle, FVector3 Direction)
    {
        Audio::Context().SetDirection(Handle, Direction);
    }

    void CAudioLibrary::SetMinMaxDistance(SAudioHandle Handle, float MinDistance, float MaxDistance)
    {
        Audio::Context().SetMinMaxDistance(Handle, MinDistance, MaxDistance);
    }

    void CAudioLibrary::SetAttenuation(SAudioHandle Handle, SAudioAttenuation Attenuation)
    {
        Audio::Context().SetAttenuation(Handle, Attenuation);
    }

    void CAudioLibrary::SetOcclusion(SAudioHandle Handle, float Amount, float LowPassFrequency,
        float VolumeAttenuation)
    {
        Audio::Context().SetOcclusion(Handle, Amount, LowPassFrequency, VolumeAttenuation);
    }

    void CAudioLibrary::SetLowPassCutoff(SAudioHandle Handle, float CutoffHz)
    {
        Audio::Context().SetLowPassCutoff(Handle, CutoffHz);
    }

    bool CAudioLibrary::SetGraphFloat(SAudioHandle Handle, const FName& Name, float Value)
    {
        return Audio::Context().SetGraphFloatParameter(Handle, Name, Value);
    }

    bool CAudioLibrary::SetGraphInt(SAudioHandle Handle, const FName& Name, int32 Value)
    {
        return Audio::Context().SetGraphIntParameter(Handle, Name, Value);
    }

    bool CAudioLibrary::SetGraphBool(SAudioHandle Handle, const FName& Name, bool Value)
    {
        return Audio::Context().SetGraphBoolParameter(Handle, Name, Value);
    }

    bool CAudioLibrary::TriggerGraph(SAudioHandle Handle, const FName& Name)
    {
        return Audio::Context().TriggerGraphParameter(Handle, Name);
    }

    float CAudioLibrary::GetGraphFloatOutput(SAudioHandle Handle, const FName& Name)
    {
        return Audio::Context().GetGraphFloatOutput(Handle, Name);
    }

    uint32 CAudioLibrary::GetGraphTriggerCount(SAudioHandle Handle, const FName& Name)
    {
        return Audio::Context().GetGraphTriggerOutputCount(Handle, Name);
    }

    int32 CAudioLibrary::GetActiveVoiceCount()
    {
        return (int32)Audio::Context().GetActiveVoiceCount();
    }

    void CAudioLibrary::SetBusVolume(EAudioBus Bus, float Volume)
    {
        Audio::Context().SetBusVolume(SafeBus(Bus), Volume);
    }

    float CAudioLibrary::GetBusVolume(EAudioBus Bus)
    {
        if (!Audio::HasDevice())
        {
            return 0.0f;
        }
        return Audio::Context().GetBusVolume(SafeBus(Bus));
    }

    void CAudioLibrary::SetBusMuted(EAudioBus Bus, bool bMuted)
    {
        Audio::Context().SetBusMuted(SafeBus(Bus), bMuted);
    }

    bool CAudioLibrary::IsBusMuted(EAudioBus Bus)
    {
        return Audio::Context().IsBusMuted(SafeBus(Bus));
    }

    void CAudioLibrary::SetBusReverbSend(EAudioBus Bus, float SendLevel)
    {
        Audio::Context().SetBusReverbSend(SafeBus(Bus), SendLevel);
    }

    void CAudioLibrary::SetReverbParams(float RoomSize, float Damping, float Width, float WetLevel)
    {
        FAudioReverbParams Params;
        Params.RoomSize = RoomSize;
        Params.Damping = Damping;
        Params.Width = Width;
        Params.WetLevel = WetLevel;
        Audio::Context().SetReverbParams(Params);
    }

    void CAudioLibrary::SetDopplerScale(float Scale)
    {
        Audio::Context().SetDopplerScale(Scale);
    }

    void CAudioLibrary::SetSuspended(bool bSuspended)
    {
        Audio::Context().SetSuspended(bSuspended);
    }

    void CAudioLibrary::SaveMixSettings()
    {
        // Collapsing this would overwrite the user's saved mix with defaults on a headless save.
        if (!Audio::HasDevice() || GConfig == nullptr)
        {
            return;
        }

        CAudioSettings* Settings = GetMutableDefault<CAudioSettings>();
        if (Settings == nullptr)
        {
            return;
        }

        for (uint32 Index = 0; Index < NumAudioBuses; ++Index)
        {
            Settings->SetBusVolume((EAudioBus)Index, Audio::Context().GetBusVolume((EAudioBus)Index));
        }

        GConfig->SaveSettings(CAudioSettings::StaticClass());
    }
}
