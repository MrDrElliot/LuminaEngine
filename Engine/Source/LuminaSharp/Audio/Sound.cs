using Lumina;

namespace LuminaSharp;

/// <summary>Play sounds in the current world (s&amp;box-style <c>Sound.Play</c>). Wraps <see cref="Game.World"/>'s
/// audio facade and returns a <see cref="PlayingSound"/> you can adjust or stop.</summary>
public static class Sound
{
    public static PlayingSound Play(CAudioStream Clip, float Volume = 1.0f, float Pitch = 1.0f, bool Loop = false)
        => new(Game.World, Game.World.Audio.Play2D(Clip, Volume, Pitch, Loop));

    public static PlayingSound PlayAt(CAudioStream Clip, FVector3 Location, float Volume = 1.0f, float Pitch = 1.0f,
        float MinDistance = 1.0f, float MaxDistance = 50.0f, bool Loop = false)
        => new(Game.World, Game.World.Audio.PlayAtLocation(Clip, Location, Volume, Pitch, MinDistance, MaxDistance, Loop));

    /// <summary>Play with the full parameter set (bus, attenuation, cone, priority, fades).</summary>
    public static PlayingSound PlayEx(CAudioStream Clip, FSoundPlayParams Params)
        => new(Game.World, Game.World.Audio.Play(Clip, Params));

    /// <summary>Play a one-shot on a specific mix group without building a full parameter set.</summary>
    public static PlayingSound PlayOnBus(CAudioStream Clip, EAudioBus Bus, float Volume = 1.0f, float Pitch = 1.0f)
    {
        FSoundPlayParams Params = FSoundPlayParams.Default();
        Params.Bus = Bus;
        Params.Volume = Volume;
        Params.Pitch = Pitch;
        return PlayEx(Clip, Params);
    }

    public static void StopAll(bool FadeOut = false) => Game.World.Audio.StopAll(FadeOut);

    /// <summary>Volume multiplier for a mix group; the knob an options menu drives.</summary>
    public static void SetBusVolume(EAudioBus Bus, float Volume) => Game.World.Audio.SetBusVolume(Bus, Volume);

    public static float GetBusVolume(EAudioBus Bus) => Game.World.Audio.GetBusVolume(Bus);

    public static void SetBusMuted(EAudioBus Bus, bool Muted) => Game.World.Audio.SetBusMuted(Bus, Muted);

    /// <summary>Persists the current bus volumes so they survive a restart.</summary>
    public static void SaveMixSettings() => Game.World.Audio.SaveMixSettings();
}

/// <summary>A live sound returned by <see cref="Sound.Play"/>. Carries its world, so the setters work even
/// after the originating callback returns.</summary>
public readonly struct PlayingSound
{
    private readonly CWorld World;
    public readonly AudioHandle Handle;

    internal PlayingSound(CWorld World, AudioHandle Handle)
    {
        this.World = World;
        this.Handle = Handle;
    }

    public bool IsValid => Handle.IsValid;
    public bool IsPlaying => World.Audio.IsPlaying(Handle);
    public VoiceState State => World.Audio.GetState(Handle);

    /// <summary>Playback position in PCM frames.</summary>
    public ulong PlaybackFrame => World.Audio.GetPlaybackFrame(Handle);

    public float Volume { set => World.Audio.SetVolume(Handle, value); }
    public float Pitch { set => World.Audio.SetPitch(Handle, value); }
    public float Pan { set => World.Audio.SetPan(Handle, value); }
    public FVector3 Position { set => World.Audio.SetPosition(Handle, value); }
    public FVector3 Velocity { set => World.Audio.SetVelocity(Handle, value); }
    public bool Looping { set => World.Audio.SetLooping(Handle, value); }
    public bool Paused { set => World.Audio.SetPaused(Handle, value); }
    public EAudioBus Bus { set => World.Audio.SetBus(Handle, value); }

    /// <summary>0 = clear line of sight, 1 = fully blocked.</summary>
    public void SetOcclusion(float Amount, float LowPassFrequency = 700.0f, float VolumeAttenuation = 0.5f)
        => World.Audio.SetOcclusion(Handle, Amount, LowPassFrequency, VolumeAttenuation);

    public void SetAttenuation(SAudioAttenuation Attenuation) => World.Audio.SetAttenuation(Handle, Attenuation);

    public void FadeTo(float Volume, float Seconds) => World.Audio.FadeTo(Handle, Volume, Seconds);

    public void Stop(bool FadeOut = false, float FadeSeconds = 0.5f) => World.Audio.Stop(Handle, FadeOut, FadeSeconds);
}
