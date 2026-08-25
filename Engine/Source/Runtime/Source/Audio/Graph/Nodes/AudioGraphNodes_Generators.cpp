#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        constexpr float kTwoPi = 6.28318530717958647692f;

        /** Wraps a normalized phase back into the unit range without a modulo. */
        FORCEINLINE float WrapPhase(float Phase)
        {
            while (Phase >= 1.0f) { Phase -= 1.0f; }
            while (Phase < 0.0f)  { Phase += 1.0f; }
            return Phase;
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
                State = 0x9E3779B9u;
                for (float& Value : PinkState)
                {
                    Value = 0.0f;
                }
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const bool bPink = *TypeIn == 1;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float White = NextWhite();
                    Out[Frame] = bPink ? NextPink(White) : White;
                }
            }

        private:

            FORCEINLINE float NextWhite()
            {
                State ^= State << 13;
                State ^= State >> 17;
                State ^= State << 5;
                return (float)(int32)(State >> 8) * (1.0f / 8388608.0f) - 1.0f;
            }

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

            uint32 State = 0x9E3779B9u;
            float  PinkState[7] = {};
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
                , OutLeft(Params.AudioOut(0))
                , OutRight(Params.AudioOut(1))
                , FinishedOut(Params.TriggerOut(2))
            {}

            void Reset() override
            {
                Position  = 0.0;
                bPlaying  = false;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const FAudioGraphWaveResource* Wave = *WaveIn;

                PlayCursor.Reset();
                StopCursor.Reset();

                const bool bValid = Wave != nullptr && Wave->IsValid();
                const double RateRatio = bValid
                    ? (double)Wave->SampleRate / (double)Context.SampleRate
                    : 1.0;

                const float Pitch = Math::Clamp(*PitchIn, 0.01f, 8.0f);
                const bool  bLoop = *LoopIn != 0;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (PlayCursor.Consume(*PlayIn, Frame))
                    {
                        Position = bValid ? Math::Max(0.0, (double)*StartSecondsIn * (double)Wave->SampleRate) : 0.0;
                        bPlaying = bValid;
                    }

                    if (StopCursor.Consume(*StopIn, Frame))
                    {
                        bPlaying = false;
                        FinishedOut->Add(Frame);
                    }

                    if (!bPlaying || !bValid)
                    {
                        OutLeft[Frame]  = 0.0f;
                        OutRight[Frame] = 0.0f;
                        continue;
                    }

                    float Left = 0.0f;
                    float Right = 0.0f;
                    Sample(*Wave, Left, Right);

                    OutLeft[Frame]  = Left;
                    OutRight[Frame] = Right;

                    Position += RateRatio * (double)Pitch;

                    if (Position >= (double)Wave->NumFrames)
                    {
                        if (bLoop)
                        {
                            Position -= (double)Wave->NumFrames;
                        }
                        else
                        {
                            bPlaying = false;
                            FinishedOut->Add(Frame);
                        }
                    }
                }
            }

        private:

            void Sample(const FAudioGraphWaveResource& Wave, float& OutLeftSample, float& OutRightSample) const
            {
                const uint64 Index = (uint64)Position;
                const uint64 Next  = Index + 1 < Wave.NumFrames ? Index + 1 : Index;
                const float  Alpha = (float)(Position - (double)Index);

                const uint32 Channels = Wave.NumChannels;
                const float* Samples  = Wave.Samples.data();

                const float* A = Samples + Index * Channels;
                const float* B = Samples + Next * Channels;

                OutLeftSample = A[0] + (B[0] - A[0]) * Alpha;

                if (Channels > 1)
                {
                    OutRightSample = A[1] + (B[1] - A[1]) * Alpha;
                }
                else
                {
                    OutRightSample = OutLeftSample;
                }
            }

            const FAudioGraphWaveResource** WaveIn;
            const FAudioGraphTriggerBuffer* PlayIn;
            const FAudioGraphTriggerBuffer* StopIn;
            const uint8*                    LoopIn;
            const float*                    PitchIn;
            const float*                    StartSecondsIn;

            float*                          OutLeft;
            float*                          OutRight;
            FAudioGraphTriggerBuffer*       FinishedOut;

            FAudioGraphTriggerCursor PlayCursor;
            FAudioGraphTriggerCursor StopCursor;

            double Position = 0.0;
            bool   bPlaying = false;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSine, FSineOperator, "Sine", { PinFloat, PinAudio, PinTrigger }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSaw, FSawOperator, "Saw", { PinFloat, PinAudio, PinTrigger }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSquare, FSquareOperator, "Square", { PinFloat, PinAudio, PinTrigger, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriangle, FTriangleOperator, "Triangle", { PinFloat, PinAudio, PinTrigger }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeNoise, FNoiseOperator, "Noise", { PinInt32 }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeWavePlayer, FWavePlayerOperator, "WavePlayer", { PinWave, PinTrigger, PinTrigger, PinBool, PinFloat, PinFloat }, { PinAudio, PinAudio, PinTrigger })
}
