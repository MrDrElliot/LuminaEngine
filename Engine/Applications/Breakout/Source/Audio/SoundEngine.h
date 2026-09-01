#pragma once

#include "SoundTypes.h"
#include "Audio/AudioDevice.h"
#include "Core/Threading/Atomic.h"
#include "Memory/SmartPtr.h"

namespace Breakout
{
    enum class EWave : uint8
    {
        Sine,
        Square,
        Saw,
        Triangle,
        Noise,
    };

    // One synthesized voice, authored in code so the game ships no audio assets.
    struct FSoundSpec
    {
        EWave  Wave        = EWave::Square;
        float  StartFreq   = 440.0f;
        float  EndFreq     = 440.0f;
        float  SweepShape  = 1.0f;
        float  Attack      = 0.004f;
        float  Decay       = 0.09f;
        float  Sustain     = 0.0f;
        float  Release     = 0.03f;
        float  Duration    = 0.16f;
        float  Volume      = 0.35f;
        float  PulseWidth  = 0.5f;
        float  NoiseMix    = 0.0f;
        float  VibratoRate = 0.0f;
        float  VibratoCents = 0.0f;
        float  CutoffStart = 20000.0f;
        float  CutoffEnd   = 20000.0f;
        float  DetuneCents = 0.0f;
        float  Drive       = 1.0f;
        bool   bHighPass   = false;
        uint8  Repeat      = 1;
        float  RepeatDelay = 0.0f;
        float  RepeatSemitones = 0.0f;
    };

    // Renders on the device thread. The game thread only ever posts requests and sets music state.
    class FSoundEngine final : public Lumina::IAudioRenderCallback
    {
    public:

        bool Initialize();
        void Shutdown();

        NODISCARD bool IsRunning() const { return Device != nullptr; }

        void Post(const FSoundRequest& Request);
        void SetMusic(bool bEnabled, float Intensity, int32 Key, EMusicMood Mood);
        void SetMusicFilter(float Openness);
        void SetMuted(bool bMuted);

        void RenderAudio(float* Out, uint32 FrameCount) override;

    private:

        static constexpr uint32 kMaxVoices       = 64;
        static constexpr uint32 kRequestCapacity = 256;
        static constexpr uint32 kSequencerSteps  = 16;

        struct FVoice
        {
            FSoundSpec Spec;
            bool   bMusic     = false;
            float  Age        = 0.0f;
            float  Delay      = 0.0f;
            float  Phase      = 0.0f;
            float  DetunePhase = 0.0f;
            float  VibratoPhase = 0.0f;
            float  FilterState = 0.0f;
            float  PitchScale = 1.0f;
            float  Gain       = 1.0f;
            float  GainLeft   = 1.0f;
            float  GainRight  = 1.0f;
            uint32 Noise      = 1u;
            bool   bActive    = false;
        };

        FVoice* AcquireVoice();
        void StartVoice(const FSoundSpec& Spec, float Pitch, float Volume, float Pan, float Delay, bool bMusic = false);
        void SpawnRequest(const FSoundRequest& Request);
        void DrainRequests();
        void AdvanceSequencer(float DeltaSeconds);
        float RenderVoice(FVoice& Voice, float InverseRate);

        Lumina::TUniquePtr<Lumina::IAudioDevice> Device;

        FVoice Voices[kMaxVoices];

        FSoundRequest Requests[kRequestCapacity];
        Lumina::TAtomic<uint32> WriteCursor { 0 };
        Lumina::TAtomic<uint32> ReadCursor  { 0 };

        Lumina::TAtomic<uint32> SampleRate { 48000 };
        Lumina::TAtomic<uint32> ChannelCount { 2 };
        Lumina::TAtomic<bool>   bMuted { false };

        Lumina::TAtomic<bool>  bMusicEnabled { false };
        Lumina::TAtomic<float> MusicIntensity { 0.0f };
        Lumina::TAtomic<int32> MusicKey { 0 };
        Lumina::TAtomic<uint8> MusicMood { 0 };
        Lumina::TAtomic<float> MusicFilter { 1.0f };

        float StepTimer = 0.0f;
        uint32 StepIndex = 0;
        float MasterFade = 0.0f;
        float MusicDuck  = 1.0f;
    };
}
