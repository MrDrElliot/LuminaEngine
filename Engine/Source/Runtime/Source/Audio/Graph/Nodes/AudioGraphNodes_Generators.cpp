#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        constexpr float kTwoPi = 6.28318530717958647692f;

        // Restarts a non-finite phase, since a huge or infinite frequency input would otherwise never wrap.
        FORCEINLINE float WrapPhase(float Phase)
        {
            if (!std::isfinite(Phase))
            {
                return 0.0f;
            }
            const float Wrapped = Math::Fract(Phase);
            return Wrapped < 1.0f ? Wrapped : 0.0f;
        }

        // Polynomial correction subtracted at a discontinuity, so a hard edged oscillator does not alias.
        FORCEINLINE float PolyBlep(float Phase, float PhaseIncrement)
        {
            if (PhaseIncrement <= 0.0f)
            {
                return 0.0f;
            }

            if (Phase < PhaseIncrement)
            {
                const float T = Phase / PhaseIncrement;
                return T + T - T * T - 1.0f;
            }

            if (Phase > 1.0f - PhaseIncrement)
            {
                const float T = (Phase - 1.0f) / PhaseIncrement;
                return T * T + T + T + 1.0f;
            }

            return 0.0f;
        }

        /** Shared phase accumulator for the four oscillators. */
        class FOscillatorBase : public IAudioGraphOperator
        {
        public:

            FOscillatorBase(const FAudioGraphOperatorBuildParams& Params)
                : FrequencyIn(Params.FloatIn(0))
                , ModulationIn(Params.AudioIn(1))
                , SyncIn(Params.TriggerIn(2))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override
            {
                Phase = 0.0f;
                Frequency.Reset();
            }

        protected:

            const float*                    FrequencyIn;
            const float*                    ModulationIn;
            const FAudioGraphTriggerBuffer* SyncIn;
            float*                          Out;

            float                      Phase = 0.0f;
            FAudioGraphSmoothedFloat   Frequency;
            FAudioGraphTriggerCursor   SyncCursor;
        };

        class FSineOperator final : public FOscillatorBase
        {
        public:

            using FOscillatorBase::FOscillatorBase;

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Frequency.Begin(*FrequencyIn, Context.NumFrames);
                SyncCursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (SyncCursor.Consume(*SyncIn, Frame))
                    {
                        Phase = 0.0f;
                    }

                    Out[Frame] = Math::Sin(Phase * kTwoPi);

                    const float Hertz = Frequency.Next() + ModulationIn[Frame];
                    Phase = WrapPhase(Phase + Hertz * Context.InverseSampleRate);
                }

                Frequency.End();
            }
        };

        class FSawOperator final : public FOscillatorBase
        {
        public:

            using FOscillatorBase::FOscillatorBase;

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Frequency.Begin(*FrequencyIn, Context.NumFrames);
                SyncCursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (SyncCursor.Consume(*SyncIn, Frame))
                    {
                        Phase = 0.0f;
                    }

                    const float Hertz     = Frequency.Next() + ModulationIn[Frame];
                    const float Increment = Math::Abs(Hertz) * Context.InverseSampleRate;

                    Out[Frame] = 2.0f * Phase - 1.0f - PolyBlep(Phase, Increment);

                    Phase = WrapPhase(Phase + Hertz * Context.InverseSampleRate);
                }

                Frequency.End();
            }
        };

        class FSquareOperator final : public FOscillatorBase
        {
        public:

            FSquareOperator(const FAudioGraphOperatorBuildParams& Params)
                : FOscillatorBase(Params)
                , PulseWidthIn(Params.FloatIn(3))
            {}

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Frequency.Begin(*FrequencyIn, Context.NumFrames);
                SyncCursor.Reset();

                const float PulseWidth = Math::Clamp(*PulseWidthIn, 0.01f, 0.99f);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (SyncCursor.Consume(*SyncIn, Frame))
                    {
                        Phase = 0.0f;
                    }

                    const float Hertz     = Frequency.Next() + ModulationIn[Frame];
                    const float Increment = Math::Abs(Hertz) * Context.InverseSampleRate;

                    float Value = Phase < PulseWidth ? 1.0f : -1.0f;
                    Value += PolyBlep(WrapPhase(Phase + 1.0f - PulseWidth), Increment);
                    Value -= PolyBlep(Phase, Increment);

                    Out[Frame] = Value;

                    Phase = WrapPhase(Phase + Hertz * Context.InverseSampleRate);
                }

                Frequency.End();
            }

        private:

            const float* PulseWidthIn;
        };

        class FTriangleOperator final : public FOscillatorBase
        {
        public:

            using FOscillatorBase::FOscillatorBase;

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Frequency.Begin(*FrequencyIn, Context.NumFrames);
                SyncCursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (SyncCursor.Consume(*SyncIn, Frame))
                    {
                        Phase = 0.0f;
                    }

                    Out[Frame] = 4.0f * Math::Abs(Phase - 0.5f) - 1.0f;

                    const float Hertz = Frequency.Next() + ModulationIn[Frame];
                    Phase = WrapPhase(Phase + Hertz * Context.InverseSampleRate);
                }

                Frequency.End();
            }
        };

        class FNoiseOperator final : public IAudioGraphOperator
        {
        public:

            explicit FNoiseOperator(const FAudioGraphOperatorBuildParams& Params)
                : TypeIn(Params.IntIn(0))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override
            {
                Random = FAudioGraphRandom();
                Algo::Fill(PinkState, 0.0f);
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const bool bPink = *TypeIn == 1;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float White = Random.NextUnit() * 2.0f - 1.0f;
                    Out[Frame] = bPink ? NextPink(White) : White;
                }
            }

        private:

            // Paul Kellet's filter bank, an inexpensive one-over-f approximation.
            FORCEINLINE float NextPink(float White)
            {
                PinkState[0] = 0.99886f * PinkState[0] + White * 0.0555179f;
                PinkState[1] = 0.99332f * PinkState[1] + White * 0.0750759f;
                PinkState[2] = 0.96900f * PinkState[2] + White * 0.1538520f;
                PinkState[3] = 0.86650f * PinkState[3] + White * 0.3104856f;
                PinkState[4] = 0.55000f * PinkState[4] + White * 0.5329522f;
                PinkState[5] = -0.7616f * PinkState[5] - White * 0.0168980f;

                const float Sum = PinkState[0] + PinkState[1] + PinkState[2] + PinkState[3]
                    + PinkState[4] + PinkState[5] + PinkState[6] + White * 0.5362f;

                PinkState[6] = White * 0.115926f;
                return Sum * 0.11f;
            }

            const int32* TypeIn;
            float*       Out;

            FAudioGraphRandom Random;
            float             PinkState[7] = {};
        };

        class FWavePlayerOperator final : public IAudioGraphOperator
        {
        public:

            explicit FWavePlayerOperator(const FAudioGraphOperatorBuildParams& Params)
                : WaveIn(Params.WaveIn(0))
                , PlayIn(Params.TriggerIn(1))
                , StopIn(Params.TriggerIn(2))
                , LoopIn(Params.BoolIn(3))
                , PitchIn(Params.FloatIn(4))
                , StartSecondsIn(Params.FloatIn(5))
                , LoopStartIn(Params.FloatIn(6))
                , LoopDurationIn(Params.FloatIn(7))
                , OutLeft(Params.AudioOut(0))
                , OutRight(Params.AudioOut(1))
                , FinishedOut(Params.TriggerOut(2))
                , LoopedOut(Params.TriggerOut(3))
                , TimeOut(Params.FloatOut(4))
            {}

            void Reset() override
            {
                Wave     = nullptr;
                Position = 0.0;
                bPlaying = false;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                PlayCursor.Reset();
                StopCursor.Reset();

                const float Pitch = Math::Clamp(*PitchIn, 0.01f, 8.0f);
                const bool  bLoop = *LoopIn != 0;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (PlayCursor.Consume(*PlayIn, Frame))
                    {
                        Start(Frame);
                    }

                    if (StopCursor.Consume(*StopIn, Frame) && bPlaying)
                    {
                        bPlaying = false;
                        FinishedOut->Add(Frame);
                    }

                    if (!bPlaying)
                    {
                        OutLeft[Frame]  = 0.0f;
                        OutRight[Frame] = 0.0f;
                        continue;
                    }

                    Sample(OutLeft[Frame], OutRight[Frame]);
                    Position += (double)Wave->SampleRate / (double)Context.SampleRate * (double)Pitch;

                    if (bLoop && Position >= LoopEnd)
                    {
                        Position = LoopStart + Math::Mod(Position - LoopStart, LoopEnd - LoopStart);
                        LoopedOut->Add(Frame);
                    }
                    else if (Position >= (double)Wave->NumFrames)
                    {
                        bPlaying = false;
                        FinishedOut->Add(Frame);
                    }
                }

                *TimeOut = bPlaying ? (float)(Position / (double)Wave->SampleRate) : 0.0f;
            }

        private:

            // Latches the wave, so one chosen for the next play cannot swap under one already sounding.
            void Start(uint32 Frame)
            {
                Wave = *WaveIn;
                if (Wave == nullptr || !Wave->IsValid())
                {
                    Wave     = nullptr;
                    bPlaying = false;
                    return;
                }

                const double Rate   = (double)Wave->SampleRate;
                const double Length = (double)Wave->NumFrames;

                Position = Math::Max(0.0, (double)*StartSecondsIn * Rate);
                if (Position >= Length)
                {
                    bPlaying = false;
                    FinishedOut->Add(Frame);
                    return;
                }

                // At least one frame long, so the wrap in Execute always makes progress.
                LoopStart = Math::Clamp((double)*LoopStartIn * Rate, 0.0, Length - 1.0);
                LoopEnd   = *LoopDurationIn > 0.0f
                    ? Math::Clamp(LoopStart + (double)*LoopDurationIn * Rate, LoopStart + 1.0, Length)
                    : Length;
                bPlaying  = true;
            }

            void Sample(float& OutLeftSample, float& OutRightSample) const
            {
                const uint64 Index = (uint64)Position;
                const uint64 Next  = Index + 1 < Wave->NumFrames ? Index + 1 : Index;
                const float  Alpha = (float)(Position - (double)Index);

                const uint32 Channels = Wave->NumChannels;
                const float* A = Wave->Samples.data() + Index * Channels;
                const float* B = Wave->Samples.data() + Next * Channels;

                OutLeftSample  = A[0] + (B[0] - A[0]) * Alpha;
                OutRightSample = Channels > 1 ? A[1] + (B[1] - A[1]) * Alpha : OutLeftSample;
            }

            const FAudioGraphWaveResource** WaveIn;
            const FAudioGraphTriggerBuffer* PlayIn;
            const FAudioGraphTriggerBuffer* StopIn;
            const uint8*                    LoopIn;
            const float*                    PitchIn;
            const float*                    StartSecondsIn;
            const float*                    LoopStartIn;
            const float*                    LoopDurationIn;

            float*                          OutLeft;
            float*                          OutRight;
            FAudioGraphTriggerBuffer*       FinishedOut;
            FAudioGraphTriggerBuffer*       LoopedOut;
            float*                          TimeOut;

            FAudioGraphTriggerCursor PlayCursor;
            FAudioGraphTriggerCursor StopCursor;

            const FAudioGraphWaveResource* Wave = nullptr;

            double Position  = 0.0;
            double LoopStart = 0.0;
            double LoopEnd   = 0.0;
            bool   bPlaying  = false;
        };

        // Hands a Wave Player one of the node's waves, so one graph covers every variation of a sound.
        class FRandomWaveOperator final : public IAudioGraphOperator
        {
        public:

            explicit FRandomWaveOperator(const FAudioGraphOperatorBuildParams& Params)
                : NextIn(Params.TriggerIn(0))
                , ModeIn(Params.IntIn(1))
                , IndexIn(Params.IntIn(2))
                , SeedIn(Params.IntIn(3))
                , WaveOut(Params.WaveOut(0))
                , IndexOut(Params.IntOut(1))
            {
                const uint32 Listed = Params.NumInputs() > kFixedInputs ? Params.NumInputs() - kFixedInputs : 0u;
                WaveCount = Math::Min(Listed, kMaxWaves);
                for (uint32 Index = 0; Index < WaveCount; ++Index)
                {
                    Waves[Index] = Params.WaveIn(kFixedInputs + Index);
                }
            }

            void Reset() override
            {
                Random  = FAudioGraphRandom();
                bPicked = false;
                Current = 0;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const EAudioPickMode Mode = (EAudioPickMode)*ModeIn;

                // Picks on the first block too, so a graph that never fires Next still varies per voice.
                if (!bPicked)
                {
                    bPicked = true;
                    Random.SeedOnce(*SeedIn);
                    Current = Mode == EAudioPickMode::Sequential ? 0u : Pick(Mode);
                }

                Cursor.Reset();
                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*NextIn, Frame))
                    {
                        Current = Pick(Mode);
                    }
                }

                if (Mode == EAudioPickMode::ByIndex)
                {
                    Current = Pick(Mode);
                }

                *WaveOut  = WaveCount != 0 ? *Waves[Current] : nullptr;
                *IndexOut = (int32)Current;
            }

        private:

            static constexpr uint32 kFixedInputs = 4;
            static constexpr uint32 kMaxWaves    = 64;

            uint32 Pick(EAudioPickMode Mode) { return PickNextOption(Mode, WaveCount, Current, *IndexIn, Random); }

            const FAudioGraphTriggerBuffer* NextIn;
            const int32*                    ModeIn;
            const int32*                    IndexIn;
            const int32*                    SeedIn;
            const FAudioGraphWaveResource** WaveOut;
            int32*                          IndexOut;

            const FAudioGraphWaveResource** Waves[kMaxWaves] = {};
            uint32                          WaveCount = 0;

            FAudioGraphTriggerCursor Cursor;
            FAudioGraphRandom        Random;
            uint32                   Current = 0;
            bool                     bPicked = false;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSine, FSineOperator, "Sine", { PinFloat, PinAudio, PinTrigger }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSaw, FSawOperator, "Saw", { PinFloat, PinAudio, PinTrigger }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSquare, FSquareOperator, "Square", { PinFloat, PinAudio, PinTrigger, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriangle, FTriangleOperator, "Triangle", { PinFloat, PinAudio, PinTrigger }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeNoise, FNoiseOperator, "Noise", { PinInt32 }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeWavePlayer, FWavePlayerOperator, "WavePlayer", { PinWave, PinTrigger, PinTrigger, PinBool, PinFloat, PinFloat, PinFloat, PinFloat }, { PinAudio, PinAudio, PinTrigger, PinTrigger, PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeRandomWave, FRandomWaveOperator, "RandomWave", { PinTrigger, PinInt32, PinInt32, PinInt32 }, { PinWave, PinInt32 }, PinWave)
}
