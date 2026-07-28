using System;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// Opaque handle to a playing sound (the blittable mirror of the engine's FAudioHandle). Returned by the
/// <c>World.Audio.Play*</c> calls and passed back to control or stop that voice. A default/invalid handle
/// is safe to pass to every control call (they no-op).
/// </summary>
[NativeLayout("AudioHandle")]
[StructLayout(LayoutKind.Sequential)]
public readonly struct AudioHandle
{
    public readonly uint Generation;
    public readonly uint Index;

    /// <summary>False for a sound that failed to start (no asset data / audio disabled / voice cap hit).</summary>
    public bool IsValid => Generation != 0;
}

/// <summary>Live state of a voice, as published by the audio pump.</summary>
public enum VoiceState
{
    /// <summary>The voice has finished, been stopped, or was evicted.</summary>
    Free = 0,
    Playing = 1,
    Paused = 2,
}

/// <summary>
/// Full set of knobs for <see cref="Audio.Play"/>. Blittable mirror of the engine's FScriptAudioPlayParams;
/// field order and types must stay in lock-step with DotNetAudio.cpp.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct FSoundPlayParams
{
    public float Volume;
    public float Pitch;
    private int LoopingFlag;
    private int SpatializedFlag;
    private int StartPausedFlag;
    public FVector3 Position;
    public FVector3 Direction;
    private int BusIndex;
    public int Priority;
    public float FadeInSeconds;
    public float StartDelaySeconds;
    private int UseOcclusionFlag;
    public SAudioAttenuation Attenuation;

    public bool Looping
    {
        get => LoopingFlag != 0;
        set => LoopingFlag = value ? 1 : 0;
    }

    public bool Spatialized
    {
        get => SpatializedFlag != 0;
        set => SpatializedFlag = value ? 1 : 0;
    }

    public bool StartPaused
    {
        get => StartPausedFlag != 0;
        set => StartPausedFlag = value ? 1 : 0;
    }

    /// <summary>Reserves the per-voice filter up front so the first occlusion update doesn't rewire the graph.</summary>
    public bool UseOcclusion
    {
        get => UseOcclusionFlag != 0;
        set => UseOcclusionFlag = value ? 1 : 0;
    }

    public EAudioBus Bus
    {
        get => (EAudioBus)BusIndex;
        set => BusIndex = (int)value;
    }

    /// <summary>Sensible defaults: full volume, unlooped, 2D, SFX bus, mid priority.</summary>
    public static FSoundPlayParams Default()
    {
        FSoundPlayParams Params = default;
        Params.Volume = 1.0f;
        Params.Pitch = 1.0f;
        Params.Direction = new FVector3(0.0f, 0.0f, 1.0f);
        Params.Priority = 128;
        Params.Bus = EAudioBus.SFX;
        Params.Attenuation = new SAudioAttenuation
        {
            Model = EAudioAttenuationModel.Inverse,
            MinDistance = 1.0f,
            MaxDistance = 50.0f,
            Rolloff = 1.0f,
            MinGain = 0.0f,
            MaxGain = 1.0f,
            ConeInnerAngle = 360.0f,
            ConeOuterAngle = 360.0f,
            ConeOuterGain = 0.0f,
            DopplerFactor = 1.0f,
            DirectionalFactor = 0.0f,
            Pan = 0.0f,
            Positioning = EAudioPositioning.Absolute,
        };
        return Params;
    }
}

/// <summary>
/// A world's audio interface (<c>World.Audio</c>). Plays <see cref="CAudioStream"/> assets as 2D (UI/music)
/// or spatialized 3D sounds and controls them through the returned <see cref="AudioHandle"/>. Also owns the
/// mix: per-bus volumes and mutes, the reverb return, doppler scale and global suspend. The engine audio
/// context is process-global, so the World handle is currently unused; the facade hangs off the world for
/// discoverability. Game thread only; every member forwards to a flat <c>LuminaSharp_Audio_*</c> shim
/// (DotNetAudio.cpp). Each call queues a command for the audio pump, so a stale handle simply no-ops.
/// </summary>
public readonly unsafe partial struct Audio
{
    internal readonly ulong Handle;

    internal Audio(ulong Handle)
    {
        this.Handle = Handle;
    }

    /// <summary>Play a non-spatialized sound (UI, music, 2D SFX). Returns the controlling handle.</summary>
    public AudioHandle Play2D(CAudioStream? Sound, float Volume = 1.0f, float Pitch = 1.0f, bool Loop = false)
        => Sound is null ? default : PlaySound2DRaw(Sound.Handle, Volume, Pitch, Loop ? 1 : 0);

    /// <summary>Play a spatialized sound at a world location, attenuated between Min and Max distance (meters).</summary>
    public AudioHandle PlayAtLocation(CAudioStream? Sound, FVector3 Location, float Volume = 1.0f, float Pitch = 1.0f,
        float MinDistance = 1.0f, float MaxDistance = 50.0f, bool Loop = false)
        => Sound is null ? default : PlaySoundAtLocationRaw(Sound.Handle, Location, Volume, Pitch, MinDistance, MaxDistance, Loop ? 1 : 0);

    /// <summary>Play with the full parameter set: bus, attenuation, cone, priority, fade in, start delay.</summary>
    public AudioHandle Play(CAudioStream? Sound, FSoundPlayParams Params)
        => Sound is null ? default : PlaySoundExRaw(Sound.Handle, Params);

    /// <summary>Stop a playing sound. <paramref name="FadeOut"/> ramps it down over <paramref name="FadeSeconds"/>.</summary>
    public void Stop(AudioHandle Handle, bool FadeOut = false, float FadeSeconds = 0.5f)
        => StopRaw(Handle, FadeOut ? 1 : 0, FadeSeconds);

    /// <summary>Stop every playing sound.</summary>
    public void StopAll(bool FadeOut = false, float FadeSeconds = 0.5f) => StopAllRaw(FadeOut ? 1 : 0, FadeSeconds);

    public void SetVolume(AudioHandle Handle, float Volume) => SetVolumeRaw(Handle, Volume);
    public void SetPitch(AudioHandle Handle, float Pitch) => SetPitchRaw(Handle, Pitch);
    public void SetLooping(AudioHandle Handle, bool Loop) => SetLoopingRaw(Handle, Loop ? 1 : 0);

    /// <summary>Move a spatialized sound to a new world position (e.g. tracking a moving emitter).</summary>
    public void SetPosition(AudioHandle Handle, FVector3 Position) => SetPositionRaw(Handle, Position);

    /// <summary>Emitter velocity, in meters per second. Only used when the attenuation's doppler factor is non-zero.</summary>
    public void SetVelocity(AudioHandle Handle, FVector3 Velocity) => SetVelocityRaw(Handle, Velocity);

    /// <summary>Forward axis of the emitter's cone.</summary>
    public void SetDirection(AudioHandle Handle, FVector3 Direction) => SetDirectionRaw(Handle, Direction);

    public void SetMinMaxDistance(AudioHandle Handle, float MinDistance, float MaxDistance) => SetMinMaxDistanceRaw(Handle, MinDistance, MaxDistance);
    public void SetAttenuation(AudioHandle Handle, SAudioAttenuation Attenuation) => SetAttenuationRaw(Handle, Attenuation);
    public void SetPan(AudioHandle Handle, float Pan) => SetPanRaw(Handle, Pan);
    public void SetPaused(AudioHandle Handle, bool Paused) => SetPausedRaw(Handle, Paused ? 1 : 0);
    public void SetBus(AudioHandle Handle, EAudioBus Bus) => SetBusRaw(Handle, (int)Bus);

    /// <summary>
    /// Muffle a voice as if geometry were in the way. Amount is 0 (clear) to 1 (fully blocked); it drives a
    /// low-pass down to <paramref name="LowPassFrequency"/> and scales gain toward <paramref name="VolumeAttenuation"/>.
    /// Smooth the value yourself, the engine applies it as given.
    /// </summary>
    public void SetOcclusion(AudioHandle Handle, float Amount, float LowPassFrequency = 700.0f, float VolumeAttenuation = 0.5f)
        => SetOcclusionRaw(Handle, Amount, LowPassFrequency, VolumeAttenuation);

    /// <summary>Direct low-pass control for non-occlusion effects (underwater, radio). 0 bypasses the filter.</summary>
    public void SetLowPassCutoff(AudioHandle Handle, float CutoffHz) => SetLowPassCutoffRaw(Handle, CutoffHz);

    /// <summary>Ramp a voice to a new volume over time instead of jumping to it.</summary>
    public void FadeTo(AudioHandle Handle, float Volume, float Seconds) => FadeToRaw(Handle, Volume, Seconds);

    public void SeekToFrame(AudioHandle Handle, ulong Frame) => SeekToFrameRaw(Handle, Frame);

    public VoiceState GetState(AudioHandle Handle) => (VoiceState)GetVoiceStateRaw(Handle);
    public bool IsPlaying(AudioHandle Handle) => GetVoiceStateRaw(Handle) == (int)VoiceState.Playing;

    /// <summary>Playback position in PCM frames. Divide by the clip's sample rate for seconds.</summary>
    public ulong GetPlaybackFrame(AudioHandle Handle) => GetPlaybackFrameRaw(Handle);

    /// <summary>Voices currently held by the mixer, for budgeting and debug overlays.</summary>
    public int ActiveVoiceCount => GetActiveVoiceCountRaw();

    /// <summary>Volume multiplier for a mix group. Master scales every other bus. This is the options-menu knob.</summary>
    public void SetBusVolume(EAudioBus Bus, float Volume) => SetBusVolumeRaw((int)Bus, Volume);
    public float GetBusVolume(EAudioBus Bus) => GetBusVolumeRaw((int)Bus);
    public void SetBusMuted(EAudioBus Bus, bool Muted) => SetBusMutedRaw((int)Bus, Muted ? 1 : 0);
    public bool IsBusMuted(EAudioBus Bus) => IsBusMutedRaw((int)Bus) != 0;

    /// <summary>How much of a bus is sent into the reverb return. 0 leaves the bus dry and costs nothing.</summary>
    public void SetBusReverbSend(EAudioBus Bus, float SendLevel) => SetBusReverbSendRaw((int)Bus, SendLevel);

    /// <summary>Reshape the shared reverb, e.g. when the listener walks into a cave or a tiled bathroom.</summary>
    public void SetReverb(float RoomSize, float Damping, float Width, float WetLevel)
        => SetReverbParamsRaw(RoomSize, Damping, Width, WetLevel);

    /// <summary>Global doppler multiplier. 0 disables doppler for every voice.</summary>
    public void SetDopplerScale(float Scale) => SetDopplerScaleRaw(Scale);

    /// <summary>Stops the output device without dropping voices. Playback resumes exactly where it left off.</summary>
    public void SetSuspended(bool Suspended) => SetSuspendedRaw(Suspended ? 1 : 0);

    /// <summary>Persists the current bus volumes to the project's AudioSettings.json. Call after an options menu applies.</summary>
    public void SaveMixSettings() => SaveMixSettingsRaw();

    // Flat shims (Runtime module). The world Handle is injected first; the CAudioStream crosses as its native pointer.
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_PlaySound2D")]
    private partial AudioHandle PlaySound2DRaw(IntPtr Stream, float Volume, float Pitch, int Loop);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_PlaySoundAtLocation")]
    private partial AudioHandle PlaySoundAtLocationRaw(IntPtr Stream, FVector3 Location, float Volume, float Pitch, float MinDistance, float MaxDistance, int Loop);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_PlaySoundEx")]
    private partial AudioHandle PlaySoundExRaw(IntPtr Stream, FSoundPlayParams Params);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_Stop")]
    private partial void StopRaw(AudioHandle Voice, int AllowFadeOut, float FadeSeconds);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_StopAll")]
    private partial void StopAllRaw(int AllowFadeOut, float FadeSeconds);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetVolume")]
    private partial void SetVolumeRaw(AudioHandle Voice, float Volume);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetPitch")]
    private partial void SetPitchRaw(AudioHandle Voice, float Pitch);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetLooping")]
    private partial void SetLoopingRaw(AudioHandle Voice, int Loop);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetPosition")]
    private partial void SetPositionRaw(AudioHandle Voice, FVector3 Position);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetVelocity")]
    private partial void SetVelocityRaw(AudioHandle Voice, FVector3 Velocity);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetDirection")]
    private partial void SetDirectionRaw(AudioHandle Voice, FVector3 Direction);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetMinMaxDistance")]
    private partial void SetMinMaxDistanceRaw(AudioHandle Voice, float MinDistance, float MaxDistance);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetAttenuation")]
    private partial void SetAttenuationRaw(AudioHandle Voice, SAudioAttenuation Attenuation);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetPan")]
    private partial void SetPanRaw(AudioHandle Voice, float Pan);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetPaused")]
    private partial void SetPausedRaw(AudioHandle Voice, int Paused);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetBus")]
    private partial void SetBusRaw(AudioHandle Voice, int Bus);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetOcclusion")]
    private partial void SetOcclusionRaw(AudioHandle Voice, float Amount, float LowPassFrequency, float VolumeAttenuation);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetLowPassCutoff")]
    private partial void SetLowPassCutoffRaw(AudioHandle Voice, float CutoffHz);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_FadeTo")]
    private partial void FadeToRaw(AudioHandle Voice, float Volume, float Seconds);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SeekToFrame")]
    private partial void SeekToFrameRaw(AudioHandle Voice, ulong Frame);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_GetVoiceState")]
    private partial int GetVoiceStateRaw(AudioHandle Voice);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_GetPlaybackFrame")]
    private partial ulong GetPlaybackFrameRaw(AudioHandle Voice);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_GetActiveVoiceCount")]
    private partial int GetActiveVoiceCountRaw();

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetBusVolume")]
    private partial void SetBusVolumeRaw(int Bus, float Volume);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_GetBusVolume")]
    private partial float GetBusVolumeRaw(int Bus);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetBusMuted")]
    private partial void SetBusMutedRaw(int Bus, int Muted);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_IsBusMuted")]
    private partial int IsBusMutedRaw(int Bus);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetBusReverbSend")]
    private partial void SetBusReverbSendRaw(int Bus, float SendLevel);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetReverbParams")]
    private partial void SetReverbParamsRaw(float RoomSize, float Damping, float Width, float WetLevel);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetDopplerScale")]
    private partial void SetDopplerScaleRaw(float Scale);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SetSuspended")]
    private partial void SetSuspendedRaw(int Suspended);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Audio_SaveMixSettings")]
    private partial void SaveMixSettingsRaw();
}
