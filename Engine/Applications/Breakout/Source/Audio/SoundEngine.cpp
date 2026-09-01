#include "SoundEngine.h"

#include "Core/Math/Math.h"
#include "Log/Log.h"

namespace Breakout
{
    namespace
    {
        constexpr uint32 kRequestedRate     = 48000;
        constexpr uint32 kRequestedChannels = 2;
        constexpr uint32 kRequestedPeriod   = 480;

        constexpr float kStepSeconds = 60.0f / 128.0f / 4.0f;
        constexpr float kMasterGain  = 0.55f;

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
            case ESound::WallHit:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 1100.0f;
                Spec.EndFreq = 620.0f;
                Spec.Duration = 0.075f;
                Spec.Decay = 0.030f;
                Spec.Release = 0.020f;
                Spec.NoiseMix = 0.18f;
                Spec.CutoffStart = 9000.0f;
                Spec.CutoffEnd = 3000.0f;
                Spec.Volume = 0.26f;
                break;

            case ESound::PaddleHit:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 240.0f;
                Spec.EndFreq = 108.0f;
                Spec.SweepShape = 0.45f;
                Spec.Duration = 0.155f;
                Spec.Decay = 0.070f;
                Spec.Release = 0.045f;
                Spec.PulseWidth = 0.32f;
                Spec.NoiseMix = 0.07f;
                Spec.CutoffStart = 4200.0f;
                Spec.CutoffEnd = 900.0f;
                Spec.Drive = 1.7f;
                Spec.Volume = 0.46f;
                break;

            case ESound::BrickHit:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 700.0f;
                Spec.EndFreq = 590.0f;
                Spec.Duration = 0.085f;
                Spec.Decay = 0.035f;
                Spec.Release = 0.030f;
                Spec.PulseWidth = 0.26f;
                Spec.CutoffStart = 7000.0f;
                Spec.CutoffEnd = 2600.0f;
                Spec.Volume = 0.24f;
                break;

            case ESound::BrickBreak:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 540.0f;
                Spec.EndFreq = 132.0f;
                Spec.SweepShape = 0.55f;
                Spec.Duration = 0.230f;
                Spec.Decay = 0.085f;
                Spec.Release = 0.070f;
                Spec.NoiseMix = 0.33f;
                Spec.DetuneCents = 14.0f;
                Spec.CutoffStart = 7500.0f;
                Spec.CutoffEnd = 700.0f;
                Spec.Drive = 2.0f;
                Spec.Volume = 0.38f;
                break;

            case ESound::SteelHit:
                Spec.Wave = EWave::Noise;
                Spec.StartFreq = 2400.0f;
                Spec.EndFreq = 1600.0f;
                Spec.Duration = 0.090f;
                Spec.Decay = 0.028f;
                Spec.Release = 0.030f;
                Spec.CutoffStart = 3500.0f;
                Spec.CutoffEnd = 3500.0f;
                Spec.bHighPass = true;
                Spec.Volume = 0.34f;
                break;

            case ESound::Explosion:
                Spec.Wave = EWave::Noise;
                Spec.StartFreq = 120.0f;
                Spec.EndFreq = 40.0f;
                Spec.SweepShape = 0.4f;
                Spec.Duration = 0.620f;
                Spec.Attack = 0.002f;
                Spec.Decay = 0.180f;
                Spec.Release = 0.320f;
                Spec.NoiseMix = 0.85f;
                Spec.CutoffStart = 4200.0f;
                Spec.CutoffEnd = 180.0f;
                Spec.Drive = 2.6f;
                Spec.Volume = 0.62f;
                break;

            case ESound::Laser:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 1650.0f;
                Spec.EndFreq = 420.0f;
                Spec.SweepShape = 0.35f;
                Spec.Duration = 0.105f;
                Spec.Decay = 0.038f;
                Spec.Release = 0.045f;
                Spec.DetuneCents = 25.0f;
                Spec.CutoffStart = 6500.0f;
                Spec.CutoffEnd = 1400.0f;
                Spec.Volume = 0.22f;
                break;

            case ESound::FireballStart:
                Spec.Wave = EWave::Noise;
                Spec.StartFreq = 200.0f;
                Spec.EndFreq = 200.0f;
                Spec.Duration = 0.850f;
                Spec.Attack = 0.090f;
                Spec.Decay = 0.400f;
                Spec.Sustain = 0.40f;
                Spec.Release = 0.350f;
                Spec.NoiseMix = 0.70f;
                Spec.CutoffStart = 600.0f;
                Spec.CutoffEnd = 4800.0f;
                Spec.Drive = 1.6f;
                Spec.Volume = 0.40f;
                break;

            case ESound::Catch:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 320.0f;
                Spec.EndFreq = 200.0f;
                Spec.Duration = 0.180f;
                Spec.Decay = 0.080f;
                Spec.Release = 0.070f;
                Spec.NoiseMix = 0.10f;
                Spec.Volume = 0.30f;
                break;

            case ESound::Release:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 300.0f;
                Spec.EndFreq = 900.0f;
                Spec.SweepShape = 0.55f;
                Spec.Duration = 0.140f;
                Spec.Decay = 0.060f;
                Spec.Release = 0.050f;
                Spec.PulseWidth = 0.35f;
                Spec.Volume = 0.26f;
                break;

            case ESound::PowerDown:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 420.0f;
                Spec.EndFreq = 90.0f;
                Spec.SweepShape = 1.3f;
                Spec.Duration = 0.520f;
                Spec.Decay = 0.240f;
                Spec.Sustain = 0.25f;
                Spec.Release = 0.200f;
                Spec.PulseWidth = 0.18f;
                Spec.DetuneCents = 30.0f;
                Spec.CutoffStart = 2400.0f;
                Spec.CutoffEnd = 420.0f;
                Spec.Drive = 1.7f;
                Spec.Volume = 0.36f;
                break;

            case ESound::Danger:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 740.0f;
                Spec.EndFreq = 740.0f;
                Spec.Duration = 0.300f;
                Spec.Attack = 0.010f;
                Spec.Decay = 0.120f;
                Spec.Sustain = 0.40f;
                Spec.Release = 0.120f;
                Spec.PulseWidth = 0.24f;
                Spec.Repeat = 3;
                Spec.RepeatDelay = 0.320f;
                Spec.RepeatSemitones = -2.0f;
                Spec.Volume = 0.28f;
                break;

            case ESound::ComboUp:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 880.0f;
                Spec.EndFreq = 1320.0f;
                Spec.SweepShape = 0.6f;
                Spec.Duration = 0.120f;
                Spec.Decay = 0.060f;
                Spec.Release = 0.050f;
                Spec.Volume = 0.20f;
                break;

            case ESound::Launch:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 190.0f;
                Spec.EndFreq = 980.0f;
                Spec.SweepShape = 0.65f;
                Spec.Duration = 0.260f;
                Spec.Decay = 0.220f;
                Spec.Sustain = 0.55f;
                Spec.Release = 0.070f;
                Spec.PulseWidth = 0.40f;
                Spec.VibratoRate = 14.0f;
                Spec.VibratoCents = 25.0f;
                Spec.CutoffStart = 3000.0f;
                Spec.CutoffEnd = 8000.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::PowerUpDrop:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 1250.0f;
                Spec.EndFreq = 1250.0f;
                Spec.Duration = 0.420f;
                Spec.Attack = 0.010f;
                Spec.Decay = 0.300f;
                Spec.Sustain = 0.35f;
                Spec.Release = 0.140f;
                Spec.VibratoRate = 11.0f;
                Spec.VibratoCents = 90.0f;
                Spec.Volume = 0.14f;
                break;

            case ESound::PowerUpCollect:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 523.25f;
                Spec.EndFreq = 523.25f;
                Spec.Duration = 0.150f;
                Spec.Decay = 0.070f;
                Spec.Release = 0.060f;
                Spec.PulseWidth = 0.35f;
                Spec.CutoffStart = 6000.0f;
                Spec.CutoffEnd = 4000.0f;
                Spec.Repeat = 4;
                Spec.RepeatDelay = 0.065f;
                Spec.RepeatSemitones = 4.0f;
                Spec.Volume = 0.24f;
                break;

            case ESound::MultiBall:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 660.0f;
                Spec.EndFreq = 990.0f;
                Spec.Duration = 0.130f;
                Spec.Decay = 0.060f;
                Spec.Release = 0.050f;
                Spec.Repeat = 6;
                Spec.RepeatDelay = 0.048f;
                Spec.RepeatSemitones = 3.0f;
                Spec.Volume = 0.20f;
                break;

            case ESound::SlowTime:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 760.0f;
                Spec.EndFreq = 118.0f;
                Spec.SweepShape = 1.4f;
                Spec.Duration = 0.950f;
                Spec.Attack = 0.020f;
                Spec.Decay = 0.700f;
                Spec.Sustain = 0.30f;
                Spec.Release = 0.250f;
                Spec.DetuneCents = 22.0f;
                Spec.VibratoRate = 5.5f;
                Spec.VibratoCents = 40.0f;
                Spec.CutoffStart = 5200.0f;
                Spec.CutoffEnd = 320.0f;
                Spec.Volume = 0.26f;
                break;

            case ESound::ExtraLife:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 587.33f;
                Spec.EndFreq = 587.33f;
                Spec.Duration = 0.200f;
                Spec.Decay = 0.110f;
                Spec.Release = 0.080f;
                Spec.Repeat = 5;
                Spec.RepeatDelay = 0.090f;
                Spec.RepeatSemitones = 5.0f;
                Spec.Volume = 0.26f;
                break;

            case ESound::LifeLost:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 420.0f;
                Spec.EndFreq = 82.0f;
                Spec.SweepShape = 1.6f;
                Spec.Duration = 0.900f;
                Spec.Attack = 0.008f;
                Spec.Decay = 0.500f;
                Spec.Sustain = 0.22f;
                Spec.Release = 0.300f;
                Spec.DetuneCents = 18.0f;
                Spec.CutoffStart = 3400.0f;
                Spec.CutoffEnd = 380.0f;
                Spec.Drive = 1.4f;
                Spec.Volume = 0.34f;
                break;

            case ESound::LevelClear:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 392.0f;
                Spec.EndFreq = 392.0f;
                Spec.Duration = 0.260f;
                Spec.Decay = 0.140f;
                Spec.Release = 0.110f;
                Spec.PulseWidth = 0.42f;
                Spec.DetuneCents = 9.0f;
                Spec.CutoffStart = 6500.0f;
                Spec.CutoffEnd = 3200.0f;
                Spec.Repeat = 7;
                Spec.RepeatDelay = 0.115f;
                Spec.RepeatSemitones = 4.0f;
                Spec.Volume = 0.28f;
                break;

            case ESound::GameOver:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 330.0f;
                Spec.EndFreq = 330.0f;
                Spec.Duration = 0.620f;
                Spec.Attack = 0.010f;
                Spec.Decay = 0.320f;
                Spec.Sustain = 0.25f;
                Spec.Release = 0.240f;
                Spec.DetuneCents = 16.0f;
                Spec.CutoffStart = 2600.0f;
                Spec.CutoffEnd = 500.0f;
                Spec.Repeat = 4;
                Spec.RepeatDelay = 0.230f;
                Spec.RepeatSemitones = -3.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::FeverStart:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 261.63f;
                Spec.EndFreq = 261.63f;
                Spec.Duration = 0.320f;
                Spec.Decay = 0.150f;
                Spec.Sustain = 0.30f;
                Spec.Release = 0.130f;
                Spec.PulseWidth = 0.30f;
                Spec.DetuneCents = 18.0f;
                Spec.CutoffStart = 7000.0f;
                Spec.CutoffEnd = 3000.0f;
                Spec.Repeat = 8;
                Spec.RepeatDelay = 0.085f;
                Spec.RepeatSemitones = 5.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::FeverEnd:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 640.0f;
                Spec.EndFreq = 180.0f;
                Spec.SweepShape = 1.2f;
                Spec.Duration = 0.420f;
                Spec.Decay = 0.190f;
                Spec.Release = 0.170f;
                Spec.DetuneCents = 20.0f;
                Spec.CutoffStart = 3200.0f;
                Spec.CutoffEnd = 500.0f;
                Spec.Volume = 0.22f;
                break;

            case ESound::ShieldReady:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 780.0f;
                Spec.EndFreq = 1170.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.190f;
                Spec.Attack = 0.008f;
                Spec.Decay = 0.090f;
                Spec.Release = 0.080f;
                Spec.Repeat = 2;
                Spec.RepeatDelay = 0.090f;
                Spec.RepeatSemitones = 7.0f;
                Spec.Volume = 0.16f;
                break;

            case ESound::ShieldSave:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 180.0f;
                Spec.EndFreq = 1450.0f;
                Spec.SweepShape = 0.45f;
                Spec.Duration = 0.480f;
                Spec.Attack = 0.006f;
                Spec.Decay = 0.220f;
                Spec.Sustain = 0.32f;
                Spec.Release = 0.190f;
                Spec.NoiseMix = 0.14f;
                Spec.DetuneCents = 14.0f;
                Spec.CutoffStart = 1200.0f;
                Spec.CutoffEnd = 8000.0f;
                Spec.Volume = 0.42f;
                break;

            case ESound::BossHit:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 220.0f;
                Spec.EndFreq = 150.0f;
                Spec.Duration = 0.140f;
                Spec.Decay = 0.055f;
                Spec.Release = 0.055f;
                Spec.PulseWidth = 0.20f;
                Spec.NoiseMix = 0.22f;
                Spec.DetuneCents = 24.0f;
                Spec.CutoffStart = 3000.0f;
                Spec.CutoffEnd = 900.0f;
                Spec.Drive = 1.8f;
                Spec.Volume = 0.38f;
                break;

            case ESound::BossDeath:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 300.0f;
                Spec.EndFreq = 55.0f;
                Spec.SweepShape = 1.1f;
                Spec.Duration = 1.150f;
                Spec.Attack = 0.006f;
                Spec.Decay = 0.480f;
                Spec.Sustain = 0.30f;
                Spec.Release = 0.480f;
                Spec.NoiseMix = 0.30f;
                Spec.DetuneCents = 32.0f;
                Spec.CutoffStart = 5200.0f;
                Spec.CutoffEnd = 200.0f;
                Spec.Drive = 2.4f;
                Spec.Repeat = 3;
                Spec.RepeatDelay = 0.130f;
                Spec.RepeatSemitones = -5.0f;
                Spec.Volume = 0.52f;
                break;

            case ESound::Hazard:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 520.0f;
                Spec.EndFreq = 230.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.170f;
                Spec.Decay = 0.070f;
                Spec.Release = 0.060f;
                Spec.PulseWidth = 0.16f;
                Spec.DetuneCents = 16.0f;
                Spec.CutoffStart = 4000.0f;
                Spec.CutoffEnd = 1200.0f;
                Spec.Volume = 0.20f;
                break;

            case ESound::Breach:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 150.0f;
                Spec.EndFreq = 62.0f;
                Spec.SweepShape = 0.9f;
                Spec.Duration = 0.900f;
                Spec.Attack = 0.004f;
                Spec.Decay = 0.360f;
                Spec.Sustain = 0.34f;
                Spec.Release = 0.380f;
                Spec.NoiseMix = 0.35f;
                Spec.DetuneCents = 28.0f;
                Spec.CutoffStart = 2600.0f;
                Spec.CutoffEnd = 220.0f;
                Spec.Drive = 2.2f;
                Spec.Volume = 0.50f;
                break;

            case ESound::UiMove:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 760.0f;
                Spec.EndFreq = 760.0f;
                Spec.Duration = 0.055f;
                Spec.Decay = 0.025f;
                Spec.Release = 0.020f;
                Spec.PulseWidth = 0.3f;
                Spec.Volume = 0.18f;
                break;

            case ESound::Smash:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 180.0f;
                Spec.EndFreq = 1400.0f;
                Spec.SweepShape = 0.35f;
                Spec.Duration = 0.320f;
                Spec.Decay = 0.140f;
                Spec.Release = 0.120f;
                Spec.NoiseMix = 0.30f;
                Spec.DetuneCents = 18.0f;
                Spec.CutoffStart = 1200.0f;
                Spec.CutoffEnd = 9000.0f;
                Spec.Drive = 2.6f;
                Spec.Volume = 0.55f;
                break;

            case ESound::VaultEnter:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 520.0f;
                Spec.EndFreq = 520.0f;
                Spec.Duration = 0.220f;
                Spec.Decay = 0.110f;
                Spec.Release = 0.090f;
                Spec.Repeat = 4;
                Spec.RepeatDelay = 0.065f;
                Spec.RepeatSemitones = 5.0f;
                Spec.VibratoRate = 9.0f;
                Spec.VibratoCents = 12.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::DroneHit:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 900.0f;
                Spec.EndFreq = 300.0f;
                Spec.SweepShape = 0.7f;
                Spec.Duration = 0.110f;
                Spec.Decay = 0.050f;
                Spec.Release = 0.040f;
                Spec.PulseWidth = 0.18f;
                Spec.NoiseMix = 0.25f;
                Spec.CutoffStart = 6000.0f;
                Spec.CutoffEnd = 1500.0f;
                Spec.Volume = 0.28f;
                break;

            case ESound::DroneDeath:
                Spec.Wave = EWave::Noise;
                Spec.StartFreq = 400.0f;
                Spec.EndFreq = 60.0f;
                Spec.Duration = 0.360f;
                Spec.Decay = 0.150f;
                Spec.Release = 0.150f;
                Spec.NoiseMix = 0.60f;
                Spec.CutoffStart = 5000.0f;
                Spec.CutoffEnd = 300.0f;
                Spec.Drive = 2.0f;
                Spec.Repeat = 2;
                Spec.RepeatDelay = 0.05f;
                Spec.RepeatSemitones = -7.0f;
                Spec.Volume = 0.36f;
                break;

            case ESound::Portal:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 220.0f;
                Spec.EndFreq = 1760.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.300f;
                Spec.Decay = 0.120f;
                Spec.Release = 0.140f;
                Spec.DetuneCents = 25.0f;
                Spec.VibratoRate = 22.0f;
                Spec.VibratoCents = 40.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::Bumper:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 330.0f;
                Spec.EndFreq = 660.0f;
                Spec.SweepShape = 0.3f;
                Spec.Duration = 0.130f;
                Spec.Decay = 0.060f;
                Spec.Release = 0.050f;
                Spec.PulseWidth = 0.45f;
                Spec.Drive = 2.2f;
                Spec.Repeat = 2;
                Spec.RepeatDelay = 0.04f;
                Spec.RepeatSemitones = 12.0f;
                Spec.Volume = 0.32f;
                break;

            case ESound::PerkOffer:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 392.0f;
                Spec.EndFreq = 392.0f;
                Spec.Duration = 0.260f;
                Spec.Decay = 0.150f;
                Spec.Release = 0.100f;
                Spec.Repeat = 3;
                Spec.RepeatDelay = 0.11f;
                Spec.RepeatSemitones = 4.0f;
                Spec.DetuneCents = 8.0f;
                Spec.Volume = 0.26f;
                break;

            case ESound::PerkPick:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 523.0f;
                Spec.EndFreq = 523.0f;
                Spec.Duration = 0.200f;
                Spec.Decay = 0.090f;
                Spec.Release = 0.090f;
                Spec.PulseWidth = 0.35f;
                Spec.Repeat = 4;
                Spec.RepeatDelay = 0.07f;
                Spec.RepeatSemitones = 5.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::Repair:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 880.0f;
                Spec.EndFreq = 440.0f;
                Spec.SweepShape = 1.5f;
                Spec.Duration = 0.250f;
                Spec.Decay = 0.120f;
                Spec.Release = 0.100f;
                Spec.Repeat = 2;
                Spec.RepeatDelay = 0.09f;
                Spec.RepeatSemitones = -3.0f;
                Spec.VibratoRate = 14.0f;
                Spec.VibratoCents = 20.0f;
                Spec.Volume = 0.24f;
                break;

            case ESound::Magnet:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 110.0f;
                Spec.EndFreq = 440.0f;
                Spec.SweepShape = 0.6f;
                Spec.Duration = 0.400f;
                Spec.Decay = 0.200f;
                Spec.Release = 0.150f;
                Spec.VibratoRate = 30.0f;
                Spec.VibratoCents = 60.0f;
                Spec.CutoffStart = 800.0f;
                Spec.CutoffEnd = 4000.0f;
                Spec.Volume = 0.26f;
                break;

            case ESound::Bomb:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 200.0f;
                Spec.EndFreq = 90.0f;
                Spec.SweepShape = 0.8f;
                Spec.Duration = 0.260f;
                Spec.Decay = 0.120f;
                Spec.Release = 0.100f;
                Spec.PulseWidth = 0.25f;
                Spec.Repeat = 3;
                Spec.RepeatDelay = 0.09f;
                Spec.RepeatSemitones = 2.0f;
                Spec.Drive = 2.0f;
                Spec.Volume = 0.34f;
                break;

            case ESound::WallUp:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 160.0f;
                Spec.EndFreq = 640.0f;
                Spec.SweepShape = 0.4f;
                Spec.Duration = 0.360f;
                Spec.Decay = 0.180f;
                Spec.Release = 0.140f;
                Spec.DetuneCents = 12.0f;
                Spec.Drive = 1.5f;
                Spec.Volume = 0.32f;
                break;

            case ESound::WallBounce:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 190.0f;
                Spec.EndFreq = 95.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.160f;
                Spec.Decay = 0.070f;
                Spec.Release = 0.060f;
                Spec.Drive = 2.4f;
                Spec.Volume = 0.40f;
                break;

            case ESound::Freeze:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 1568.0f;
                Spec.EndFreq = 1568.0f;
                Spec.Duration = 0.420f;
                Spec.Attack = 0.010f;
                Spec.Decay = 0.250f;
                Spec.Release = 0.150f;
                Spec.Repeat = 4;
                Spec.RepeatDelay = 0.08f;
                Spec.RepeatSemitones = -5.0f;
                Spec.DetuneCents = 6.0f;
                Spec.Volume = 0.22f;
                break;

            case ESound::Reverse:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 880.0f;
                Spec.EndFreq = 110.0f;
                Spec.SweepShape = 1.0f;
                Spec.Duration = 0.420f;
                Spec.Decay = 0.200f;
                Spec.Release = 0.150f;
                Spec.VibratoRate = 6.0f;
                Spec.VibratoCents = 200.0f;
                Spec.CutoffStart = 5000.0f;
                Spec.CutoffEnd = 600.0f;
                Spec.Volume = 0.28f;
                break;

            case ESound::Blind:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 330.0f;
                Spec.EndFreq = 40.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.520f;
                Spec.Decay = 0.250f;
                Spec.Release = 0.200f;
                Spec.PulseWidth = 0.12f;
                Spec.NoiseMix = 0.30f;
                Spec.CutoffStart = 3000.0f;
                Spec.CutoffEnd = 200.0f;
                Spec.Drive = 1.8f;
                Spec.Volume = 0.34f;
                break;

            case ESound::Gold:
                Spec.Wave = EWave::Triangle;
                Spec.StartFreq = 1046.0f;
                Spec.EndFreq = 1046.0f;
                Spec.Duration = 0.240f;
                Spec.Decay = 0.120f;
                Spec.Release = 0.100f;
                Spec.Repeat = 5;
                Spec.RepeatDelay = 0.05f;
                Spec.RepeatSemitones = 4.0f;
                Spec.DetuneCents = 5.0f;
                Spec.Volume = 0.24f;
                break;

            case ESound::Regen:
                Spec.Wave = EWave::Sine;
                Spec.StartFreq = 440.0f;
                Spec.EndFreq = 880.0f;
                Spec.SweepShape = 0.6f;
                Spec.Duration = 0.200f;
                Spec.Decay = 0.100f;
                Spec.Release = 0.080f;
                Spec.Volume = 0.16f;
                break;

            case ESound::Grade:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 392.0f;
                Spec.EndFreq = 392.0f;
                Spec.Duration = 0.300f;
                Spec.Decay = 0.140f;
                Spec.Release = 0.140f;
                Spec.PulseWidth = 0.40f;
                Spec.Repeat = 5;
                Spec.RepeatDelay = 0.10f;
                Spec.RepeatSemitones = 3.0f;
                Spec.DetuneCents = 9.0f;
                Spec.Volume = 0.30f;
                break;

            case ESound::BossIntro:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 55.0f;
                Spec.EndFreq = 41.0f;
                Spec.SweepShape = 0.8f;
                Spec.Duration = 1.400f;
                Spec.Attack = 0.050f;
                Spec.Decay = 0.700f;
                Spec.Sustain = 0.35f;
                Spec.Release = 0.500f;
                Spec.DetuneCents = 20.0f;
                Spec.CutoffStart = 300.0f;
                Spec.CutoffEnd = 1800.0f;
                Spec.Drive = 2.5f;
                Spec.Repeat = 3;
                Spec.RepeatDelay = 0.42f;
                Spec.RepeatSemitones = 1.0f;
                Spec.Volume = 0.42f;
                break;

            case ESound::BossSplit:
                Spec.Wave = EWave::Saw;
                Spec.StartFreq = 600.0f;
                Spec.EndFreq = 150.0f;
                Spec.SweepShape = 0.4f;
                Spec.Duration = 0.600f;
                Spec.Decay = 0.300f;
                Spec.Release = 0.250f;
                Spec.NoiseMix = 0.35f;
                Spec.DetuneCents = 40.0f;
                Spec.CutoffStart = 6000.0f;
                Spec.CutoffEnd = 500.0f;
                Spec.Drive = 2.4f;
                Spec.Repeat = 2;
                Spec.RepeatDelay = 0.12f;
                Spec.RepeatSemitones = 7.0f;
                Spec.Volume = 0.42f;
                break;

            case ESound::ArmorUp:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 260.0f;
                Spec.EndFreq = 260.0f;
                Spec.Duration = 0.220f;
                Spec.Decay = 0.100f;
                Spec.Release = 0.100f;
                Spec.PulseWidth = 0.15f;
                Spec.Repeat = 3;
                Spec.RepeatDelay = 0.07f;
                Spec.RepeatSemitones = -4.0f;
                Spec.NoiseMix = 0.15f;
                Spec.Drive = 1.8f;
                Spec.Volume = 0.28f;
                break;

            default:
                Spec.Wave = EWave::Square;
                Spec.StartFreq = 620.0f;
                Spec.EndFreq = 930.0f;
                Spec.SweepShape = 0.5f;
                Spec.Duration = 0.140f;
                Spec.Decay = 0.070f;
                Spec.Release = 0.055f;
                Spec.PulseWidth = 0.38f;
                Spec.Repeat = 2;
                Spec.RepeatDelay = 0.075f;
                Spec.RepeatSemitones = 7.0f;
                Spec.Volume = 0.24f;
                break;
            }

            return Spec;
        }

        float DuckFor(ESound Sound)
        {
            switch (Sound)
            {
            case ESound::Explosion:  return 0.34f;
            case ESound::BossDeath:  return 0.22f;
            case ESound::Breach:     return 0.30f;
            case ESound::LifeLost:   return 0.36f;
            case ESound::GameOver:   return 0.30f;
            case ESound::LevelClear: return 0.40f;
            case ESound::FeverStart: return 0.35f;
            case ESound::ShieldSave: return 0.45f;
            case ESound::Smash:      return 0.50f;
            case ESound::BossSplit:  return 0.30f;
            case ESound::BossIntro:  return 0.25f;
            case ESound::Grade:      return 0.40f;
            case ESound::PerkPick:   return 0.50f;
            case ESound::Bomb:       return 0.45f;
            case ESound::VaultEnter: return 0.55f;
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

        FSoundSpec SnareSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Noise;
            Spec.StartFreq = 220.0f;
            Spec.EndFreq = 120.0f;
            Spec.Duration = 0.140f;
            Spec.Attack = 0.001f;
            Spec.Decay = 0.060f;
            Spec.Release = 0.060f;
            Spec.NoiseMix = 0.75f;
            Spec.CutoffStart = 5000.0f;
            Spec.CutoffEnd = 1800.0f;
            Spec.Drive = 1.6f;
            Spec.Volume = 0.26f;
            return Spec;
        }

        FSoundSpec TomSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Sine;
            Spec.StartFreq = 190.0f;
            Spec.EndFreq = 70.0f;
            Spec.SweepShape = 0.5f;
            Spec.Duration = 0.240f;
            Spec.Attack = 0.002f;
            Spec.Decay = 0.110f;
            Spec.Release = 0.100f;
            Spec.NoiseMix = 0.08f;
            Spec.Drive = 1.8f;
            Spec.Volume = 0.36f;
            return Spec;
        }

        FSoundSpec LeadSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Saw;
            Spec.Duration = 0.230f;
            Spec.Attack = 0.008f;
            Spec.Decay = 0.090f;
            Spec.Sustain = 0.45f;
            Spec.Release = 0.090f;
            Spec.DetuneCents = 9.0f;
            Spec.VibratoRate = 5.5f;
            Spec.VibratoCents = 14.0f;
            Spec.CutoffStart = 3200.0f;
            Spec.CutoffEnd = 1400.0f;
            Spec.Drive = 1.3f;
            Spec.Volume = 0.13f;
            return Spec;
        }

        FSoundSpec BellSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Sine;
            Spec.Duration = 0.900f;
            Spec.Attack = 0.003f;
            Spec.Decay = 0.600f;
            Spec.Sustain = 0.10f;
            Spec.Release = 0.300f;
            Spec.DetuneCents = 4.0f;
            Spec.Volume = 0.15f;
            return Spec;
        }

        FSoundSpec PluckSpec()
        {
            FSoundSpec Spec;
            Spec.Wave = EWave::Triangle;
            Spec.Duration = 0.160f;
            Spec.Attack = 0.002f;
            Spec.Decay = 0.090f;
            Spec.Release = 0.060f;
            Spec.CutoffStart = 5200.0f;
            Spec.CutoffEnd = 900.0f;
            Spec.Volume = 0.11f;
            return Spec;
        }

        struct FSong
        {
            int32 Chords[4];
            uint16 KickMask;
            uint16 BassMask;
            int8  Lead[16];
        };

        // Chord roots are semitones above the key; masks are one bit per sixteenth step; lead is a minor scale degree, -1 rests.
        const FSong kSongs[4] =
        {
            { { 0, 8, 3, 10 }, 0x1111, 0x4949, {  0, -1,  2,  3, -1,  4, -1,  3,  2, -1,  0, -1, -1,  4,  5,  4 } },
            { { 0, 5, 7, 3 },  0x1111, 0x5555, {  4,  4, -1,  5,  7, -1,  5,  4, -1,  2,  0, -1,  2,  3, -1,  2 } },
            { { 0, 10, 8, 7 }, 0x1119, 0x2929, {  7, -1,  6,  5, -1,  4,  5, -1,  3, -1,  2,  0, -1,  3,  4, -1 } },
            { { 0, 3, 8, 5 },  0x1111, 0x5151, {  0,  2,  4, -1,  7, -1,  4,  2, -1,  5,  4,  2, -1,  0, -1, -1 } },
        };

        const FSong kBossSong  { { 0, 1, 0, 6 },  0x1111, 0xFFFF, {  0,  0,  7,  0,  6,  0,  0,  5,  0,  0,  7,  0,  6,  5,  6,  0 } };
        const FSong kFeverSong { { 0, 8, 5, 10 }, 0x1111, 0x5555, {  7,  8,  7,  5,  7, -1,  8,  9,  7, -1,  5,  7,  4,  5,  7, -1 } };
        const FSong kMenuSong  { { 0, 8, 3, 10 }, 0x0000, 0x0101, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 } };
        const FSong kDraftSong { { 0, 5, 3, 8 },  0x0000, 0x0000, {  0, -1, -1,  4, -1, -1,  7, -1, -1,  4, -1, -1,  9, -1,  7, -1 } };
        const FSong kLossSong  { { 0, 1, 0, 10 }, 0x0000, 0x0001, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 } };

        constexpr int32 kMinorScale[10] = { 0, 2, 3, 5, 7, 8, 10, 12, 14, 15 };
        constexpr int32 kPentatonic[5] = { 0, 3, 5, 7, 10 };

        const FSong& SongFor(EMusicMood Mood, int32 Level)
        {
            switch (Mood)
            {
            case EMusicMood::Menu:  return kMenuSong;
            case EMusicMood::Boss:  return kBossSong;
            case EMusicMood::Fever: return kFeverSong;
            case EMusicMood::Draft: return kDraftSong;
            case EMusicMood::Loss:  return kLossSong;
            case EMusicMood::Clear: return kFeverSong;
            default:                return kSongs[((Level - 1) / 3) & 3];
            }
        }
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

    void FSoundEngine::SetMusic(bool bEnabled, float Intensity, int32 Key, EMusicMood Mood)
    {
        bMusicEnabled.store(bEnabled, Atomic::MemoryOrderRelaxed);
        MusicIntensity.store(Math::Clamp(Intensity, 0.0f, 1.0f), Atomic::MemoryOrderRelaxed);
        MusicKey.store(Key, Atomic::MemoryOrderRelaxed);
        MusicMood.store(uint8(Mood), Atomic::MemoryOrderRelaxed);
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
        const int32 Level = Math::Max(MusicKey.load(Atomic::MemoryOrderRelaxed), 1);
        const EMusicMood Mood = EMusicMood(MusicMood.load(Atomic::MemoryOrderRelaxed));
        const float Openness = MusicFilter.load(Atomic::MemoryOrderRelaxed);
        const FSong& Song = SongFor(Mood, Level);

        const bool bDriving = Mood == EMusicMood::Boss || Mood == EMusicMood::Fever || Mood == EMusicMood::Vault || Mood == EMusicMood::Clear;
        const bool bCalm = Mood == EMusicMood::Menu || Mood == EMusicMood::Draft || Mood == EMusicMood::Loss;
        const int32 KeyShift = Mood == EMusicMood::Boss ? -2 : (Mood == EMusicMood::Fever || Mood == EMusicMood::Clear ? 3 : (Level % 7));

        StepTimer += DeltaSeconds;
        while (StepTimer >= kStepSeconds)
        {
            StepTimer -= kStepSeconds;

            // Where inside this block the step landed, so the beat does not snap to the block edge.
            const float Offset = Math::Clamp(DeltaSeconds - StepTimer, 0.0f, DeltaSeconds);

            const uint32 Step = StepIndex % kSequencerSteps;
            const uint32 Bar = (StepIndex / kSequencerSteps) % 4u;
            const uint32 Phrase = (StepIndex / (kSequencerSteps * 4u)) % 2u;
            ++StepIndex;

            const int32 Root = 45 + KeyShift + Song.Chords[Bar];
            const uint16 Bit = uint16(1u << Step);

            if (Step == 0)
            {
                FSoundSpec Pad = PadSpec();
                Pad.CutoffStart *= Openness;
                Pad.CutoffEnd *= Openness;
                const float PadGain = bCalm ? 1.2f : 1.0f;
                const int32 Third = Mood == EMusicMood::Boss ? 1 : (Mood == EMusicMood::Fever || Mood == EMusicMood::Clear ? 4 : 3);
                StartVoice(Pad, MidiToFreq(float(Root + 12)) / Pad.StartFreq, PadGain, -0.25f, Offset, true);
                StartVoice(Pad, MidiToFreq(float(Root + 12 + Third)) / Pad.StartFreq, PadGain * 0.7f, 0.0f, Offset + 0.01f, true);
                StartVoice(Pad, MidiToFreq(float(Root + 19)) / Pad.StartFreq, PadGain * 0.8f, 0.25f, Offset + 0.02f, true);
            }

            if ((Song.BassMask & Bit) != 0 && (!bCalm || Mood == EMusicMood::Menu))
            {
                FSoundSpec Bass = BassSpec();
                Bass.CutoffStart *= Openness * (bDriving ? 1.4f : 1.0f);
                Bass.CutoffEnd *= Openness;
                if (Mood == EMusicMood::Boss)
                {
                    Bass.Duration = 0.13f;
                    Bass.Drive = 2.2f;
                }
                const int32 Note = Root - 12 + (Mood == EMusicMood::Boss && (Step % 8) == 6 ? 1 : 0);
                StartVoice(Bass, MidiToFreq(float(Note)) / Bass.StartFreq, 0.7f + Intensity * 0.3f, 0.0f, Offset, true);
            }

            if (Intensity > 0.30f && (Song.KickMask & Bit) != 0)
            {
                FSoundSpec Kick = KickSpec();
                StartVoice(Kick, Mood == EMusicMood::Boss ? 0.92f : 1.0f, Intensity, 0.0f, Offset, true);
            }

            const bool bSnareStep = Step == 4 || Step == 12 || (bDriving && Step == 15 && Bar == 3) || (Mood == EMusicMood::Boss && Step == 7);
            if (Intensity > 0.5f && !bCalm && bSnareStep)
            {
                FSoundSpec Snare = SnareSpec();
                StartVoice(Snare, Step == 15 ? 1.15f : 1.0f, Intensity * (Step == 15 ? 0.6f : 0.95f), 0.05f, Offset, true);
            }

            const bool bHatStep = bDriving ? true : (Step % 2) == 1;
            if (Intensity > 0.45f && !bCalm && bHatStep)
            {
                FSoundSpec Hat = HatSpec();
                const bool bOpen = bDriving && Step % 4 == 2;
                if (bOpen)
                {
                    Hat.Duration = 0.12f;
                    Hat.Decay = 0.06f;
                }
                StartVoice(Hat, bOpen ? 0.9f : 1.0f, Intensity * (Step % 2 == 1 ? 0.9f : 0.5f), Step % 4 == 1 ? -0.3f : 0.3f, Offset, true);
            }

            if (Mood == EMusicMood::Boss && (Step == 6 || Step == 7 || Step == 14) && Intensity > 0.3f)
            {
                FSoundSpec Tom = TomSpec();
                StartVoice(Tom, Step == 14 ? 0.8f : 1.0f, Intensity * 0.9f, Step == 6 ? -0.4f : 0.4f, Offset, true);
            }

            if (Intensity > 0.20f && !bCalm)
            {
                const uint32 ArpIndex = StepIndex % 5u;
                const int32 Lift = (Mood == EMusicMood::Fever || Mood == EMusicMood::Vault || Mood == EMusicMood::Clear) ? 12 : 0;
                const int32 Note = Root + 12 + Lift + kPentatonic[ArpIndex] + (Step >= 8 ? 12 : 0);

                FSoundSpec Arp = ArpSpec();
                Arp.CutoffStart *= Openness;
                Arp.CutoffEnd *= Openness;
                const float Pan = (float(ArpIndex) / 4.0f - 0.5f) * 0.8f;
                StartVoice(Arp, MidiToFreq(float(Note)) / Arp.StartFreq, 0.5f + Intensity * 0.5f, Pan, Offset, true);
            }

            const int8 Degree = Song.Lead[Step];
            const bool bLeadOn = Degree >= 0 && (bDriving || Intensity > 0.62f || Mood == EMusicMood::Draft);
            if (bLeadOn)
            {
                const int32 Note = Root + 24 + kMinorScale[Math::Clamp<int32>(Degree, 0, 9)] + (Phrase == 1 && Bar >= 2 ? 12 : 0);
                if (Mood == EMusicMood::Draft)
                {
                    FSoundSpec Bell = BellSpec();
                    StartVoice(Bell, MidiToFreq(float(Note)) / 440.0f, 1.0f, (Step % 2 == 0) ? -0.35f : 0.35f, Offset, true);
                }
                else
                {
                    FSoundSpec Lead = LeadSpec();
                    Lead.CutoffStart *= Openness * (Mood == EMusicMood::Vault ? 1.6f : 1.0f);
                    Lead.CutoffEnd *= Openness;
                    StartVoice(Lead, MidiToFreq(float(Note)) / 440.0f, 0.6f + Intensity * 0.5f, 0.15f, Offset, true);
                }
            }

            if (Mood == EMusicMood::Menu && (Step == 0 || Step == 6 || Step == 10))
            {
                const uint32 Pick = (StepIndex * 7u + Bar * 3u) % 5u;
                FSoundSpec Pluck = PluckSpec();
                StartVoice(Pluck, MidiToFreq(float(Root + 24 + kPentatonic[Pick])) / 440.0f, 1.0f, (Pick % 2 == 0) ? -0.3f : 0.3f, Offset, true);
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
