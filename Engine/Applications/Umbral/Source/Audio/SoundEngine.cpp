#include "SoundEngine.h"

#include "Core/Math/Math.h"
#include "Log/Log.h"

namespace Umbral
{
    namespace
    {
        constexpr uint32 kRequestedRate     = 48000;
        constexpr uint32 kRequestedChannels = 2;
        constexpr uint32 kRequestedPeriod   = 480;

        constexpr float kStepSeconds = 60.0f / 96.0f / 4.0f;
        constexpr float kMasterGain  = 0.52f;

        float MidiToFreq(float Note)
        {
            return 440.0f * Math::Pow(2.0f, (Note - 69.0f) / 12.0f);
        }

        float NextNoise(uint32& State)
        {
            State ^= State << 13;
            State ^= State >> 17;
            State ^= State << 5;
            return float(State >> 8) * (1.0f / 8388608.0f) - 1.0f;
        }

        float Oscillate(EWave Wave, float Phase, float PulseWidth, uint32& NoiseState)
        {
            switch (Wave)
            {
            case EWave::Sine:     return Math::Sin(Phase * Math::TwoPi<float>());
            case EWave::Square:   return Phase < PulseWidth ? 1.0f : -1.0f;
            case EWave::Saw:      return Phase * 2.0f - 1.0f;
            case EWave::Triangle: return 1.0f - 4.0f * Math::Abs(Phase - 0.5f);
            default:              return NextNoise(NoiseState);
            }
        }

        float Envelope(const FSoundSpec& Spec, float Age)
        {
            if (Age >= Spec.Duration)
            {
                return 0.0f;
            }

            float Level;
            if (Age < Spec.Attack)
            {
                Level = Spec.Attack > 0.0f ? Age / Spec.Attack : 1.0f;
            }
            else
            {
                const float Decayed = Math::Exp(-(Age - Spec.Attack) / Math::Max(Spec.Decay, 0.001f));
                Level = Spec.Sustain + (1.0f - Spec.Sustain) * Decayed;
            }

            const float ReleaseStart = Math::Max(Spec.Duration - Spec.Release, 0.0f);
            if (Spec.Release > 0.0f && Age > ReleaseStart)
            {
                Level *= 1.0f - (Age - ReleaseStart) / Spec.Release;
            }

            return Math::Max(Level, 0.0f);
        }

        float SoftClip(float Value)
        {
            const float Clamped = Math::Clamp(Value, -3.0f, 3.0f);
            return Clamped * (27.0f + Clamped * Clamped) / (27.0f + 9.0f * Clamped * Clamped);
        }

        FSoundSpec MakeSpec(ESound Sound)
        {
            FSoundSpec Spec;

            switch (Sound)
            {
            case ESound::BladeHit:
                Spec.Wave = EWave::Noise;
                Spec.StartFreq = 2200.0f;
                Spec.EndFreq = 900.0f;
                Spec.Duration = 0.070f;
                Spec.Decay = 0.024f;
                Spec.Release = 0.026f;
                Spec.CutoffStart = 5000.0f;
                Spec.CutoffEnd = 5000.0f;
                Spec.bHighPass = true;
                Spec.Volume = 0.16f;
                break;

            case ESound::BoltFire:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 340.0f;
                Spec.EndFreq = 980.0f;
                Spec.SweepShape = 0.45f;
                Spec.Duration = 0.150f;
                Spec.Decay = 0.060f;
                Spec.Release = 0.060f;
                Spec.DetuneCents = 18.0f;
                Spec.CutoffStart = 2200.0f;
                Spec.CutoffEnd = 6000.0f;
                Spec.Volume = 0.18f;
                break;

            case ESound::BoltHit:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 900.0f;
                Spec.EndFreq = 420.0f;
                Spec.Duration = 0.075f;
                Spec.Decay = 0.028f;
                Spec.Release = 0.030f;
                Spec.PulseWidth = 0.24f;
                Spec.Volume = 0.16f;
                break;

            case ESound::NovaCast:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 420.0f;
                Spec.EndFreq = 58.0f;
                Spec.SweepShape = 0.7f;
                Spec.Duration = 0.900f;
                Spec.Attack = 0.004f;
                Spec.Decay = 0.320f;
                Spec.Sustain = 0.28f;
                Spec.Release = 0.380f;
                Spec.NoiseMix = 0.22f;
                Spec.DetuneCents = 30.0f;
                Spec.CutoffStart = 4600.0f;
                Spec.CutoffEnd = 240.0f;
                Spec.Drive = 2.2f;
                Spec.Volume = 0.46f;
                break;

            case ESound::PyreLight:
                Spec.Wave = EWave::Noise;
                Spec.StartFreq = 300.0f;
                Spec.EndFreq = 300.0f;
                Spec.Duration = 0.520f;
                Spec.Attack = 0.040f;
                Spec.Decay = 0.240f;
                Spec.Sustain = 0.30f;
                Spec.Release = 0.220f;
                Spec.NoiseMix = 0.80f;
                Spec.CutoffStart = 700.0f;
                Spec.CutoffEnd = 3400.0f;
                Spec.Drive = 1.5f;
                Spec.Volume = 0.24f;
                break;

            case ESound::PyreBurn:
                Spec.Wave = EWave::Noise;
                Spec.Duration = 0.180f;
                Spec.Decay = 0.070f;
                Spec.Release = 0.080f;
                Spec.CutoffStart = 1400.0f;
                Spec.CutoffEnd = 500.0f;
                Spec.Volume = 0.10f;
                break;

            case ESound::AgentDie:
                Spec.Wave = EWave::Noise;
                Spec.StartFreq = 620.0f;
                Spec.EndFreq = 180.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.110f;
                Spec.Decay = 0.038f;
                Spec.Release = 0.045f;
                Spec.NoiseMix = 0.70f;
                Spec.CutoffStart = 2600.0f;
                Spec.CutoffEnd = 500.0f;
                Spec.Volume = 0.20f;
                break;

            case ESound::BruteDie:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 210.0f;
                Spec.EndFreq = 62.0f;
                Spec.SweepShape = 0.8f;
                Spec.Duration = 0.420f;
                Spec.Decay = 0.180f;
                Spec.Release = 0.170f;
                Spec.NoiseMix = 0.40f;
                Spec.DetuneCents = 26.0f;
                Spec.CutoffStart = 2200.0f;
                Spec.CutoffEnd = 300.0f;
                Spec.Drive = 1.9f;
                Spec.Volume = 0.34f;
                break;

            case ESound::PlayerHurt:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 260.0f;
                Spec.EndFreq = 120.0f;
                Spec.SweepShape = 0.7f;
                Spec.Duration = 0.190f;
                Spec.Decay = 0.075f;
                Spec.Release = 0.070f;
                Spec.PulseWidth = 0.16f;
                Spec.NoiseMix = 0.30f;
                Spec.DetuneCents = 34.0f;
                Spec.Drive = 1.8f;
                Spec.Volume = 0.30f;
                break;

            case ESound::PlayerDie:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 340.0f;
                Spec.EndFreq = 44.0f;
                Spec.SweepShape = 1.5f;
                Spec.Duration = 1.600f;
                Spec.Attack = 0.010f;
                Spec.Decay = 0.700f;
                Spec.Sustain = 0.26f;
                Spec.Release = 0.700f;
                Spec.NoiseMix = 0.24f;
                Spec.DetuneCents = 40.0f;
                Spec.CutoffStart = 3600.0f;
                Spec.CutoffEnd = 140.0f;
                Spec.Drive = 2.2f;
                Spec.Volume = 0.52f;
                break;

            case ESound::SoulPickup:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 1180.0f;
                Spec.EndFreq = 1560.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.085f;
                Spec.Attack = 0.003f;
                Spec.Decay = 0.035f;
                Spec.Release = 0.040f;
                Spec.Volume = 0.13f;
                break;

            case ESound::LevelUp:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 392.0f;
                Spec.EndFreq = 392.0f;
                Spec.Duration = 0.260f;
                Spec.Decay = 0.130f;
                Spec.Release = 0.110f;
                Spec.DetuneCents = 12.0f;
                Spec.Repeat = 6;
                Spec.RepeatDelay = 0.095f;
                Spec.RepeatSemitones = 5.0f;
                Spec.Volume = 0.26f;
                break;

            case ESound::UpgradePick:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 520.0f;
                Spec.EndFreq = 780.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.160f;
                Spec.Decay = 0.070f;
                Spec.Release = 0.060f;
                Spec.PulseWidth = 0.32f;
                Spec.Repeat = 2;
                Spec.RepeatDelay = 0.080f;
                Spec.RepeatSemitones = 7.0f;
                Spec.Volume = 0.24f;
                break;

            case ESound::WaveWarn:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 92.0f;
                Spec.EndFreq = 74.0f;
                Spec.Duration = 1.400f;
                Spec.Attack = 0.300f;
                Spec.Decay = 0.600f;
                Spec.Sustain = 0.45f;
                Spec.Release = 0.500f;
                Spec.DetuneCents = 22.0f;
                Spec.CutoffStart = 500.0f;
                Spec.CutoffEnd = 180.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::UiMove:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 620.0f;
                Spec.EndFreq = 620.0f;
                Spec.Duration = 0.055f;
                Spec.Decay = 0.025f;
                Spec.Release = 0.020f;
                Spec.PulseWidth = 0.3f;
                Spec.Volume = 0.16f;
                break;

            default:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 330.0f;
                Spec.EndFreq = 660.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.220f;
                Spec.Decay = 0.100f;
                Spec.Release = 0.090f;
                Spec.DetuneCents = 10.0f;
                Spec.Repeat = 3;
                Spec.RepeatDelay = 0.090f;
                Spec.RepeatSemitones = 7.0f;
                Spec.Volume = 0.26f;
                break;
            }

            return Spec;
        }

        float DuckFor(ESound Sound)
        {
            switch (Sound)
            {
            case ESound::NovaCast:   return 0.42f;
            case ESound::PlayerDie:  return 0.22f;
            case ESound::PlayerHurt: return 0.55f;
            case ESound::LevelUp:    return 0.34f;
            case ESound::BruteDie:   return 0.60f;
            default:                 return 1.0f;
            }
        }

        FSoundSpec KickSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Sine;
            Spec.StartFreq = 155.0f;
            Spec.EndFreq = 44.0f;
            Spec.SweepShape = 0.32f;
            Spec.Duration = 0.220f;
            Spec.Attack = 0.002f;
            Spec.Decay = 0.075f;
            Spec.Release = 0.080f;
            Spec.Drive = 1.9f;
            Spec.Volume = 0.50f;
            return Spec;
        }

        FSoundSpec HatSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Noise;
            Spec.Duration = 0.048f;
            Spec.Attack = 0.001f;
            Spec.Decay = 0.018f;
            Spec.Release = 0.020f;
            Spec.CutoffStart = 7000.0f;
            Spec.CutoffEnd = 7000.0f;
            Spec.bHighPass = true;
            Spec.Volume = 0.10f;
            return Spec;
        }

        FSoundSpec BassSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Saw;
            Spec.Duration = 0.210f;
            Spec.Attack = 0.004f;
            Spec.Decay = 0.130f;
            Spec.Sustain = 0.30f;
            Spec.Release = 0.060f;
            Spec.DetuneCents = 11.0f;
            Spec.CutoffStart = 900.0f;
            Spec.CutoffEnd = 280.0f;
            Spec.Drive = 1.5f;
            Spec.Volume = 0.30f;
            return Spec;
        }

        FSoundSpec ArpSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Square;
            Spec.Duration = 0.175f;
            Spec.Attack = 0.003f;
            Spec.Decay = 0.075f;
            Spec.Release = 0.060f;
            Spec.PulseWidth = 0.30f;
            Spec.DetuneCents = 7.0f;
            Spec.CutoffStart = 4800.0f;
            Spec.CutoffEnd = 1800.0f;
            Spec.Volume = 0.13f;
            return Spec;
        }

        FSoundSpec PadSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Triangle;
            Spec.Duration = 2.100f;
            Spec.Attack = 0.500f;
            Spec.Decay = 1.400f;
            Spec.Sustain = 0.55f;
            Spec.Release = 0.900f;
            Spec.DetuneCents = 13.0f;
            Spec.CutoffStart = 1600.0f;
            Spec.CutoffEnd = 900.0f;
            Spec.Volume = 0.11f;
            return Spec;
        }

        // Natural minor triad roots for a i, VI, III, VII turnaround.
        constexpr int32 kChordRoots[4] = { 0, 8, 3, 10 };
        constexpr int32 kPentatonic[5] = { 0, 3, 5, 7, 10 };
    }


    bool FSoundEngine::Initialize()
    {
        const FAudioDeviceConfig Config
        {
            .SampleRate   = kRequestedRate,
            .Channels     = kRequestedChannels,
            .PeriodFrames = kRequestedPeriod,
        };

        Device = Audio::CreateDevice(Config, this);
        if (!Device)
        {
            LOG_WARN("Breakout: no audio endpoint, running silent.");
            return false;
        }

        SampleRate.store(Math::Max(Device->GetSampleRate(), 1u), Atomic::MemoryOrderRelease);
        ChannelCount.store(Math::Max(Device->GetChannelCount(), 1u), Atomic::MemoryOrderRelease);

        LOG_INFO("Breakout: audio online at {} Hz, {} channels.",
            Device->GetSampleRate(), Device->GetChannelCount());
        return true;
    }

    void FSoundEngine::Shutdown()
    {
        if (Device)
        {
            Device->Stop();
            Device.Reset();
        }
    }

    void FSoundEngine::Post(const FSoundRequest& Request)
    {
        const uint32 Write = WriteCursor.load(Atomic::MemoryOrderRelaxed);
        const uint32 Read = ReadCursor.load(Atomic::MemoryOrderAcquire);

        if (Write - Read >= kRequestCapacity)
        {
            return;
        }

        Requests[Write % kRequestCapacity] = Request;
        WriteCursor.store(Write + 1, Atomic::MemoryOrderRelease);
    }

    void FSoundEngine::SetMusic(bool bEnabled, float Intensity, int32 Key)
    {
        bMusicEnabled.store(bEnabled, Atomic::MemoryOrderRelaxed);
        MusicIntensity.store(Math::Clamp(Intensity, 0.0f, 1.0f), Atomic::MemoryOrderRelaxed);
        MusicKey.store(Key, Atomic::MemoryOrderRelaxed);
    }

    void FSoundEngine::SetMusicFilter(float Openness)
    {
        MusicFilter.store(Math::Clamp(Openness, 0.08f, 1.0f), Atomic::MemoryOrderRelaxed);
    }

    void FSoundEngine::SetMuted(bool bInMuted)
    {
        bMuted.store(bInMuted, Atomic::MemoryOrderRelaxed);
    }

    FSoundEngine::FVoice* FSoundEngine::AcquireVoice()
    {
        for (FVoice& Voice : Voices)
        {
            if (!Voice.bActive)
            {
                return &Voice;
            }
        }

        FVoice* Oldest = &Voices[0];
        for (FVoice& Voice : Voices)
        {
            if (Voice.Age > Oldest->Age)
            {
                Oldest = &Voice;
            }
        }
        return Oldest;
    }

    void FSoundEngine::StartVoice(const FSoundSpec& Spec, float Pitch, float Volume, float Pan, float Delay, bool bMusic)
    {
        FVoice& Voice = *AcquireVoice();

        Voice.bMusic       = bMusic;
        Voice.Spec         = Spec;
        Voice.Age          = 0.0f;
        Voice.Delay        = Delay;
        Voice.Phase        = 0.0f;
        Voice.DetunePhase  = 0.0f;
        Voice.VibratoPhase = 0.0f;
        Voice.FilterState  = 0.0f;
        Voice.PitchScale   = Math::Clamp(Pitch, 0.05f, 8.0f);
        Voice.Gain         = Math::Max(Volume, 0.0f) * Spec.Volume;
        Voice.Noise        = 0x9E3779B9u ^ (uint32(Voice.Gain * 65536.0f) | 1u);
        Voice.bActive      = true;

        const float Clamped = Math::Clamp(Pan, -1.0f, 1.0f);
        Voice.GainLeft  = Math::Sqrt(0.5f * (1.0f - Clamped));
        Voice.GainRight = Math::Sqrt(0.5f * (1.0f + Clamped));
    }

    void FSoundEngine::SpawnRequest(const FSoundRequest& Request)
    {
        const FSoundSpec Spec = MakeSpec(Request.Sound);
        const uint8 Repeats = Math::Max<uint8>(Spec.Repeat, 1);

        MusicDuck = Math::Min(MusicDuck, DuckFor(Request.Sound));

        for (uint8 Index = 0; Index < Repeats; ++Index)
        {
            const float Semitones = Spec.RepeatSemitones * float(Index);
            const float Pitch = Request.Pitch * Math::Pow(2.0f, Semitones / 12.0f);
            StartVoice(Spec, Pitch, Request.Volume, Request.Pan, Spec.RepeatDelay * float(Index));
        }
    }

    void FSoundEngine::DrainRequests()
    {
        uint32 Read = ReadCursor.load(Atomic::MemoryOrderRelaxed);
        const uint32 Write = WriteCursor.load(Atomic::MemoryOrderAcquire);

        while (Read != Write)
        {
            SpawnRequest(Requests[Read % kRequestCapacity]);
            ++Read;
        }

        ReadCursor.store(Read, Atomic::MemoryOrderRelease);
    }

    void FSoundEngine::AdvanceSequencer(float DeltaSeconds)
    {
        if (!bMusicEnabled.load(Atomic::MemoryOrderRelaxed))
        {
            StepTimer = 0.0f;
            return;
        }

        const float Intensity = MusicIntensity.load(Atomic::MemoryOrderRelaxed);
        const int32 Key = MusicKey.load(Atomic::MemoryOrderRelaxed);
        const float Openness = MusicFilter.load(Atomic::MemoryOrderRelaxed);

        StepTimer += DeltaSeconds;
        while (StepTimer >= kStepSeconds)
        {
            StepTimer -= kStepSeconds;

            // Where inside this block the step landed, so the beat does not snap to the block edge.
            const float Offset = Math::Clamp(DeltaSeconds - StepTimer, 0.0f, DeltaSeconds);

            const uint32 Step = StepIndex % kSequencerSteps;
            const uint32 Bar = (StepIndex / kSequencerSteps) % 4u;
            ++StepIndex;

            const int32 Root = 45 + (Key % 7) + kChordRoots[Bar];

            if (Step == 0)
            {
                FSoundSpec Pad = PadSpec();
                Pad.CutoffStart *= Openness;
                Pad.CutoffEnd *= Openness;
                StartVoice(Pad, MidiToFreq(float(Root + 12)) / Pad.StartFreq, 1.0f, -0.25f, Offset, true);
                StartVoice(Pad, MidiToFreq(float(Root + 19)) / Pad.StartFreq, 0.8f, 0.25f, Offset + 0.02f, true);
            }

            const bool bBassStep = Step == 0 || Step == 3 || Step == 6 || Step == 8 || Step == 11 || Step == 14;
            if (bBassStep)
            {
                FSoundSpec Bass = BassSpec();
                Bass.CutoffStart *= Openness;
                Bass.CutoffEnd *= Openness;
                StartVoice(Bass, MidiToFreq(float(Root - 12)) / Bass.StartFreq, 0.7f + Intensity * 0.3f, 0.0f, Offset, true);
            }

            if (Intensity > 0.30f && (Step % 4) == 0)
            {
                FSoundSpec Kick = KickSpec();
                StartVoice(Kick, 1.0f, Intensity, 0.0f, Offset, true);
            }

            if (Intensity > 0.45f && (Step % 2) == 1)
            {
                FSoundSpec Hat = HatSpec();
                StartVoice(Hat, 1.0f, Intensity * 0.9f, Step % 4 == 1 ? -0.3f : 0.3f, Offset, true);
            }

            if (Intensity > 0.20f)
            {
                const uint32 ArpIndex = StepIndex % 5u;
                const int32 Note = Root + 12 + kPentatonic[ArpIndex] + (Step >= 8 ? 12 : 0);

                FSoundSpec Arp = ArpSpec();
                Arp.CutoffStart *= Openness;
                Arp.CutoffEnd *= Openness;
                const float Pan = (float(ArpIndex) / 4.0f - 0.5f) * 0.8f;
                StartVoice(Arp, MidiToFreq(float(Note)) / Arp.StartFreq, 0.5f + Intensity * 0.5f, Pan, Offset, true);
            }
        }
    }

    float FSoundEngine::RenderVoice(FVoice& Voice, float InverseRate)
    {
        if (Voice.Delay > 0.0f)
        {
            Voice.Delay -= InverseRate;
            return 0.0f;
        }

        const FSoundSpec& Spec = Voice.Spec;

        Voice.Age += InverseRate;
        if (Voice.Age >= Spec.Duration)
        {
            Voice.bActive = false;
            return 0.0f;
        }

        const float Normalized = Math::Clamp(Voice.Age / Math::Max(Spec.Duration, 0.001f), 0.0f, 1.0f);
        const float SweepAlpha = Math::Pow(Normalized, Math::Max(Spec.SweepShape, 0.01f));

        float Frequency = (Spec.StartFreq + (Spec.EndFreq - Spec.StartFreq) * SweepAlpha) * Voice.PitchScale;

        if (Spec.VibratoCents > 0.0f)
        {
            Voice.VibratoPhase += Spec.VibratoRate * InverseRate;
            const float Cents = Math::Sin(Voice.VibratoPhase * Math::TwoPi<float>()) * Spec.VibratoCents;
            Frequency *= Math::Pow(2.0f, Cents / 1200.0f);
        }

        Frequency = Math::Clamp(Frequency, 10.0f, 18000.0f);

        Voice.Phase += Frequency * InverseRate;
        Voice.Phase -= Math::Floor(Voice.Phase);

        float Sample = Oscillate(Spec.Wave, Voice.Phase, Spec.PulseWidth, Voice.Noise);

        if (Spec.DetuneCents > 0.0f)
        {
            Voice.DetunePhase += Frequency * Math::Pow(2.0f, Spec.DetuneCents / 1200.0f) * InverseRate;
            Voice.DetunePhase -= Math::Floor(Voice.DetunePhase);
            Sample = (Sample + Oscillate(Spec.Wave, Voice.DetunePhase, Spec.PulseWidth, Voice.Noise)) * 0.5f;
        }

        if (Spec.NoiseMix > 0.0f)
        {
            Sample = Sample * (1.0f - Spec.NoiseMix) + NextNoise(Voice.Noise) * Spec.NoiseMix;
        }

        const float Cutoff = Spec.CutoffStart + (Spec.CutoffEnd - Spec.CutoffStart) * Normalized;
        const float Coefficient = Math::Clamp(Cutoff * InverseRate * Math::TwoPi<float>(), 0.0f, 1.0f);
        Voice.FilterState += (Sample - Voice.FilterState) * Coefficient;
        Sample = Spec.bHighPass ? Sample - Voice.FilterState : Voice.FilterState;

        if (Spec.Drive > 1.0f)
        {
            Sample = SoftClip(Sample * Spec.Drive);
        }

        return Sample * Envelope(Spec, Voice.Age) * Voice.Gain;
    }

    void FSoundEngine::RenderAudio(float* Out, uint32 FrameCount)
    {
        const uint32 Channels = ChannelCount.load(Atomic::MemoryOrderAcquire);
        const uint32 Rate = SampleRate.load(Atomic::MemoryOrderAcquire);

        for (uint32 Index = 0; Index < FrameCount * Channels; ++Index)
        {
            Out[Index] = 0.0f;
        }

        if (Rate == 0 || Channels == 0)
        {
            return;
        }

        const float InverseRate = 1.0f / float(Rate);

        DrainRequests();
        AdvanceSequencer(float(FrameCount) * InverseRate);

        const float Target = bMuted.load(Atomic::MemoryOrderRelaxed) ? 0.0f : kMasterGain;

        for (uint32 Frame = 0; Frame < FrameCount; ++Frame)
        {
            float Left = 0.0f;
            float Right = 0.0f;

            for (FVoice& Voice : Voices)
            {
                if (!Voice.bActive)
                {
                    continue;
                }

                float Sample = RenderVoice(Voice, InverseRate);
                if (Voice.bMusic)
                {
                    Sample *= MusicDuck;
                }
                Left  += Sample * Voice.GainLeft;
                Right += Sample * Voice.GainRight;
            }

            MasterFade += (Target - MasterFade) * 0.0015f;
            MusicDuck += (1.0f - MusicDuck) * 0.00006f;

            Left  = SoftClip(Left * MasterFade);
            Right = SoftClip(Right * MasterFade);

            float* Destination = Out + Frame * Channels;
            Destination[0] = Left;
            if (Channels > 1)
            {
                Destination[1] = Right;
                for (uint32 Extra = 2; Extra < Channels; ++Extra)
                {
                    Destination[Extra] = (Left + Right) * 0.5f;
                }
            }
        }
    }
}
