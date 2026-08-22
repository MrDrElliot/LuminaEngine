#include "Platform/GenericPlatform.h"
#include "Scripting/DotNet/LayoutRegistry.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectCore.h"
#include "Memory/SmartPtr.h"
#include "Audio/AudioGlobals.h"
#include "Audio/AudioContext.h"
#include "Audio/AudioSettings.h"
#include "Audio/AudioTypes.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Config/Config.h"
#include "Scripting/DotNet/DotNetExport.h"

// The leading World handle is reserved to keep the facade uniform and leave room for per-world mixers.

using namespace Lumina;
using namespace Lumina::DotNet;

namespace
{
    // A null handle makes the play calls return an invalid handle.
    const TSharedPtr<FAudioData>& AudioDataOf(void* StreamPtr)
    {
        static const TSharedPtr<FAudioData> None;
        const CAudioStream* Stream = reinterpret_cast<const CAudioStream*>(StreamPtr);
        return (Stream != nullptr && Stream->IsValid()) ? Stream->GetAudioData() : None;
    }
}

// Blittable mirror of the managed SoundPlayParams. Kept flat so the marshaller can pass it by value.
struct FScriptAudioPlayParams
{
    float Volume;
    float Pitch;
    int32 bLooping;
    int32 bSpatialized;
    int32 bStartPaused;
    FVector3 Position;
    FVector3 Direction;
    int32 Bus;
    int32 Priority;
    float FadeInSeconds;
    float StartDelaySeconds;
    int32 bUseOcclusion;
    SAudioAttenuation Attenuation;
};

static FAudioPlayParams ToNative(const FScriptAudioPlayParams& In)
{
    FAudioPlayParams Out;
    Out.Volume            = In.Volume;
    Out.Pitch             = In.Pitch;
    Out.bLooping          = In.bLooping != 0;
    Out.bSpatialized      = In.bSpatialized != 0;
    Out.bStartPaused      = In.bStartPaused != 0;
    Out.Position          = In.Position;
    Out.Direction         = In.Direction;
    Out.Bus               = (Lumina::EAudioBus)Math::Clamp(In.Bus, 0, (int32)NumAudioBuses - 1);
    Out.Priority          = (uint8)Math::Clamp(In.Priority, 0, 255);
    Out.FadeInSeconds     = In.FadeInSeconds;
    Out.StartDelaySeconds = In.StartDelaySeconds;
    Out.bUseOcclusion     = In.bUseOcclusion != 0;
    Out.Attenuation       = In.Attenuation;
    return Out;
}

// Play a 2D (non-spatialized) sound, e.g. UI or music. Returns the controlling handle (invalid on failure).
LUMINA_DOTNET_EXPORT(FAudioHandle, Audio_PlaySound2D)(uint64 World, void* Stream, float Volume, float Pitch, int32 bLoop)
{
    (void)World;
    const TSharedPtr<FAudioData>& Data = AudioDataOf(Stream);
    if (Data.get() == nullptr)
    {
        return FAudioHandle::Invalid();
    }
    return Audio::Context().PlayAudio2D(Data, Volume, Pitch, bLoop != 0);
}

// Play a 3D sound attenuated between MinDistance and MaxDistance around Location.
LUMINA_DOTNET_EXPORT(FAudioHandle, Audio_PlaySoundAtLocation)(uint64 World, void* Stream, FVector3 Location,
    float Volume, float Pitch, float MinDistance, float MaxDistance, int32 bLoop)
{
    (void)World;
    const TSharedPtr<FAudioData>& Data = AudioDataOf(Stream);
    if (Data.get() == nullptr)
    {
        return FAudioHandle::Invalid();
    }
    return Audio::Context().PlayAudioAtLocation(Data, Location, Volume, Pitch, MinDistance, MaxDistance, bLoop != 0);
}

// Full-control playback covering bus, attenuation, priority, fades and occlusion.
LUMINA_DOTNET_EXPORT(FAudioHandle, Audio_PlaySoundEx)(uint64 World, void* Stream, FScriptAudioPlayParams Params)
{
    (void)World;
    const TSharedPtr<FAudioData>& Data = AudioDataOf(Stream);
    if (Data.get() == nullptr)
    {
        return FAudioHandle::Invalid();
    }
    return Audio::Context().PlayAudio(Data, ToNative(Params));
}

LUMINA_DOTNET_EXPORT(void, Audio_Stop)(uint64 World, FAudioHandle Handle, int32 bAllowFadeOut, float FadeSeconds)
{
    (void)World;
    Audio::Context().StopSound(Handle,
        bAllowFadeOut != 0 ? EAudioStopMode::AllowFadeOut : EAudioStopMode::Immediate, FadeSeconds);
}

LUMINA_DOTNET_EXPORT(void, Audio_StopAll)(uint64 World, int32 bAllowFadeOut, float FadeSeconds)
{
    (void)World;
    Audio::Context().StopAllSounds(
        bAllowFadeOut != 0 ? EAudioStopMode::AllowFadeOut : EAudioStopMode::Immediate, FadeSeconds);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetVolume)(uint64 World, FAudioHandle Handle, float Volume)
{
    (void)World;
    Audio::Context().SetVolume(Handle, Volume);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetPitch)(uint64 World, FAudioHandle Handle, float Pitch)
{
    (void)World;
    Audio::Context().SetPitch(Handle, Pitch);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetLooping)(uint64 World, FAudioHandle Handle, int32 bLoop)
{
    (void)World;
    Audio::Context().SetLooping(Handle, bLoop != 0);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetPosition)(uint64 World, FAudioHandle Handle, FVector3 Position)
{
    (void)World;
    Audio::Context().SetPosition(Handle, Position);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetVelocity)(uint64 World, FAudioHandle Handle, FVector3 Velocity)
{
    (void)World;
    Audio::Context().SetVelocity(Handle, Velocity);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetDirection)(uint64 World, FAudioHandle Handle, FVector3 Direction)
{
    (void)World;
    Audio::Context().SetDirection(Handle, Direction);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetMinMaxDistance)(uint64 World, FAudioHandle Handle, float MinDistance, float MaxDistance)
{
    (void)World;
    Audio::Context().SetMinMaxDistance(Handle, MinDistance, MaxDistance);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetAttenuation)(uint64 World, FAudioHandle Handle, SAudioAttenuation Attenuation)
{
    (void)World;
    Audio::Context().SetAttenuation(Handle, Attenuation);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetPan)(uint64 World, FAudioHandle Handle, float Pan)
{
    (void)World;
    Audio::Context().SetPan(Handle, Pan);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetPaused)(uint64 World, FAudioHandle Handle, int32 bPaused)
{
    (void)World;
    Audio::Context().SetPaused(Handle, bPaused != 0);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetBus)(uint64 World, FAudioHandle Handle, int32 Bus)
{
    (void)World;
    Audio::Context().SetBus(Handle, (Lumina::EAudioBus)Math::Clamp(Bus, 0, (int32)NumAudioBuses - 1));
}

LUMINA_DOTNET_EXPORT(void, Audio_SetOcclusion)(uint64 World, FAudioHandle Handle, float Amount, float LowPassFrequency, float VolumeAttenuation)
{
    (void)World;
    Audio::Context().SetOcclusion(Handle, Amount, LowPassFrequency, VolumeAttenuation);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetLowPassCutoff)(uint64 World, FAudioHandle Handle, float CutoffHz)
{
    (void)World;
    Audio::Context().SetLowPassCutoff(Handle, CutoffHz);
}

LUMINA_DOTNET_EXPORT(void, Audio_FadeTo)(uint64 World, FAudioHandle Handle, float Volume, float Seconds)
{
    (void)World;
    Audio::Context().FadeTo(Handle, Volume, Seconds);
}

LUMINA_DOTNET_EXPORT(void, Audio_SeekToFrame)(uint64 World, FAudioHandle Handle, uint64 Frame)
{
    (void)World;
    Audio::Context().SeekToFrame(Handle, Frame);
}

LUMINA_DOTNET_EXPORT(int32, Audio_GetVoiceState)(uint64 World, FAudioHandle Handle)
{
    (void)World;
    return (int32)Audio::Context().GetVoiceState(Handle);
}

LUMINA_DOTNET_EXPORT(uint64, Audio_GetPlaybackFrame)(uint64 World, FAudioHandle Handle)
{
    (void)World;
    return Audio::Context().GetPlaybackFrame(Handle);
}

LUMINA_DOTNET_EXPORT(int32, Audio_GetActiveVoiceCount)(uint64 World)
{
    (void)World;
    return (int32)Audio::Context().GetActiveVoiceCount();
}

LUMINA_DOTNET_EXPORT(void, Audio_SetBusVolume)(uint64 World, int32 Bus, float Volume)
{
    (void)World;
    Audio::Context().SetBusVolume((Lumina::EAudioBus)Math::Clamp(Bus, 0, (int32)NumAudioBuses - 1), Volume);
}

LUMINA_DOTNET_EXPORT(float, Audio_GetBusVolume)(uint64 World, int32 Bus)
{
    (void)World;
    // This export's contract with script has always been 0 for no audio, so collapsing it would change that.
    if (!Audio::HasDevice())
    {
        return 0.0f;
    }
    return Audio::Context().GetBusVolume((Lumina::EAudioBus)Math::Clamp(Bus, 0, (int32)NumAudioBuses - 1));
}

LUMINA_DOTNET_EXPORT(void, Audio_SetBusMuted)(uint64 World, int32 Bus, int32 bMuted)
{
    (void)World;
    Audio::Context().SetBusMuted((Lumina::EAudioBus)Math::Clamp(Bus, 0, (int32)NumAudioBuses - 1), bMuted != 0);
}

LUMINA_DOTNET_EXPORT(int32, Audio_IsBusMuted)(uint64 World, int32 Bus)
{
    (void)World;
    return Audio::Context().IsBusMuted((Lumina::EAudioBus)Math::Clamp(Bus, 0, (int32)NumAudioBuses - 1)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, Audio_SetBusReverbSend)(uint64 World, int32 Bus, float SendLevel)
{
    (void)World;
    Audio::Context().SetBusReverbSend((Lumina::EAudioBus)Math::Clamp(Bus, 0, (int32)NumAudioBuses - 1), SendLevel);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetReverbParams)(uint64 World, float RoomSize, float Damping, float Width, float WetLevel)
{
    (void)World;
    FAudioReverbParams Params;
    Params.RoomSize = RoomSize;
    Params.Damping  = Damping;
    Params.Width    = Width;
    Params.WetLevel = WetLevel;
    Audio::Context().SetReverbParams(Params);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetDopplerScale)(uint64 World, float Scale)
{
    (void)World;
    Audio::Context().SetDopplerScale(Scale);
}

LUMINA_DOTNET_EXPORT(void, Audio_SetSuspended)(uint64 World, int32 bSuspended)
{
    (void)World;
    Audio::Context().SetSuspended(bSuspended != 0);
}

// Writes the current bus volumes back into CAudioSettings and persists them; the options-menu save path.
LUMINA_DOTNET_EXPORT(void, Audio_SaveMixSettings)(uint64 World)
{
    (void)World;
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

    for (uint32 i = 0; i < NumAudioBuses; ++i)
    {
        Settings->SetBusVolume((Lumina::EAudioBus)i, Audio::Context().GetBusVolume((Lumina::EAudioBus)i));
    }

    GConfig->SaveSettings(CAudioSettings::StaticClass());
}

// Bootstrap size check against the C# AudioHandle mirror; a drift here silently misreads voice slots.
LE_REGISTER_LAYOUT("AudioHandle", Lumina::FAudioHandle);
