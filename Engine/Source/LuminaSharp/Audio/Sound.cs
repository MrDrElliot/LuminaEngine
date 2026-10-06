using Lumina;

namespace LuminaSharp;

/// Play sounds, s&amp;box style. Returns a PlayingSound you can adjust or stop. There is one audio device per process, so none of this is per world.
public static class Sound
{
    public static PlayingSound Play(CSoundBase? Clip, float Volume = 1.0f, float Pitch = 1.0f, bool Loop = false)
        => new(CAudioLibrary.PlaySound2D(Clip!, Volume, Pitch, Loop));

    public static PlayingSound PlayAt(CSoundBase? Clip, FVector3 Location, float Volume = 1.0f, float Pitch = 1.0f,
        float MinDistance = 1.0f, float MaxDistance = 50.0f, bool Loop = false)
        => new(CAudioLibrary.PlaySoundAtLocation(Clip!, Location, Volume, Pitch, MinDistance, MaxDistance, Loop));

    // By asset path, so a one-off sound needs no LoadObject of its own. Nothing loads in a process with no audio device.
    public static PlayingSound Play(string ClipPath, float Volume = 1.0f, float Pitch = 1.0f, bool Loop = false)
        => Engine.IsHeadless ? default : Play(ObjectCore.LoadObject<CSoundBase>(ClipPath), Volume, Pitch, Loop);

    public static PlayingSound PlayAt(string ClipPath, FVector3 Location, float Volume = 1.0f, float Pitch = 1.0f,
        float MinDistance = 1.0f, float MaxDistance = 50.0f, bool Loop = false)
        => Engine.IsHeadless ? default : PlayAt(ObjectCore.LoadObject<CSoundBase>(ClipPath), Location, Volume, Pitch, MinDistance, MaxDistance, Loop);

    /// Play with the full parameter set, so bus, attenuation, cone, priority and fades.
    public static PlayingSound PlayEx(CSoundBase? Clip, FAudioPlayParams Params)
        => new(CAudioLibrary.PlaySoundEx(Clip!, Params));

    /// Play a one-shot on a specific mix group without building a full parameter set.
    public static PlayingSound PlayOnBus(CSoundBase? Clip, EAudioBus Bus, float Volume = 1.0f, float Pitch = 1.0f)
    {
        FAudioPlayParams Params = FAudioPlayParams.Default();
        Params.Bus = Bus;
        Params.Volume = Volume;
        Params.Pitch = Pitch;
        return PlayEx(Clip, Params);
    }
}

/// A live voice. Holds only its handle, so it stays valid past the callback that started it, and every setter no-ops once the voice is gone.
public readonly struct PlayingSound
{
    public readonly FAudioHandle Handle;

    internal PlayingSound(FAudioHandle Handle)
    {
        this.Handle = Handle;
    }

    public bool IsValid => Handle.IsValid;

    public bool IsPlaying => CAudioLibrary.GetVoiceState(Handle) == EAudioVoiceState.Playing;

    public EAudioVoiceState State => CAudioLibrary.GetVoiceState(Handle);

    /// Playback position in PCM frames.
    public ulong PlaybackFrame => CAudioLibrary.GetPlaybackFrame(Handle);

    public float Volume { set => CAudioLibrary.SetVolume(Handle, value); }
    public float Pitch { set => CAudioLibrary.SetPitch(Handle, value); }
    public float Pan { set => CAudioLibrary.SetPan(Handle, value); }
    public FVector3 Position { set => CAudioLibrary.SetPosition(Handle, value); }
    public FVector3 Velocity { set => CAudioLibrary.SetVelocity(Handle, value); }
    public FVector3 Direction { set => CAudioLibrary.SetDirection(Handle, value); }
    public bool Looping { set => CAudioLibrary.SetLooping(Handle, value); }
    public bool Paused { set => CAudioLibrary.SetPaused(Handle, value); }
    public EAudioBus Bus { set => CAudioLibrary.SetBus(Handle, value); }
    public float LowPassCutoff { set => CAudioLibrary.SetLowPassCutoff(Handle, value); }

    /// Amount runs from 0 for a clear line of sight to 1 for fully blocked.
    public void SetOcclusion(float Amount, float LowPassFrequency = 700.0f, float VolumeAttenuation = 0.5f)
        => CAudioLibrary.SetOcclusion(Handle, Amount, LowPassFrequency, VolumeAttenuation);

    public void SetAttenuation(SAudioAttenuation Attenuation) => CAudioLibrary.SetAttenuation(Handle, Attenuation);

    public void SetMinMaxDistance(float MinDistance, float MaxDistance)
        => CAudioLibrary.SetMinMaxDistance(Handle, MinDistance, MaxDistance);

    public void FadeTo(float Volume, float Seconds) => CAudioLibrary.FadeTo(Handle, Volume, Seconds);

    public void SeekToFrame(ulong Frame) => CAudioLibrary.SeekToFrame(Handle, Frame);

    public void Stop(bool FadeOut = false, float FadeSeconds = 0.5f)
        => CAudioLibrary.Stop(Handle, FadeOut, FadeSeconds);

    //~ Graph parameters, for a one shot played from a CAudioGraph rather than a wave.

    public bool SetFloat(string Name, float Value) => CAudioLibrary.SetGraphFloat(Handle, Name, Value);
    public bool SetInt(string Name, int Value) => CAudioLibrary.SetGraphInt(Handle, Name, Value);
    public bool SetBool(string Name, bool Value) => CAudioLibrary.SetGraphBool(Handle, Name, Value);
    public bool Trigger(string Name) => CAudioLibrary.TriggerGraph(Handle, Name);

    public float GetFloatOutput(string Name) => CAudioLibrary.GetGraphFloatOutput(Handle, Name);

    /// Monotonic fire count of a graph trigger output. Compare against your own last read.
    public uint GetTriggerCount(string Name) => CAudioLibrary.GetGraphTriggerCount(Handle, Name);
}
