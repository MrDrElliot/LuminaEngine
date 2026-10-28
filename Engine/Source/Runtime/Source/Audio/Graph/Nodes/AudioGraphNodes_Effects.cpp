#include "RuntimePCH.h"

#include "Audio/AudioReverb.h"
#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        constexpr float kMinLevel = 1.0e-6f;

        // Twenty over the natural log of ten, so decibels come from Math::Log.
        constexpr float kDecibelsPerNeper = 8.6858896f;

        FORCEINLINE float LinearToDecibels(float Linear) { return kDecibelsPerNeper * Math::Log(Math::Max(Linear, kMinLevel)); }
        FORCEINLINE float DecibelsToLinear(float Decibels) { return Math::Pow(10.0f, Decibels * 0.05f); }

        // One pole smoothing coefficient that settles in roughly Seconds.
        FORCEINLINE float SmoothingCoefficient(float Seconds, uint32 SampleRate)
        {
            return Seconds > 0.0f ? Math::Exp(-1.0f / (Seconds * (float)SampleRate)) : 0.0f;
        }

        // Peak follower with separate rise and fall times.
        struct FEnvelopeDetector
        {
            float Value = 0.0f;

            FORCEINLINE float Step(float Level, float AttackCoefficient, float ReleaseCoefficient)
            {
                const float Coefficient = Level > Value ? AttackCoefficient : ReleaseCoefficient;
                Value = Level + (Value - Level) * Coefficient;
                return Value;
            }
        };

        class FReverbOperator final : public IAudioGraphOperator
        {
        public:

            explicit FReverbOperator(const FAudioGraphOperatorBuildParams& Params)
                : InLeft(Params.AudioIn(0))
                , InRight(Params.AudioIn(1))
                , RoomSizeIn(Params.FloatIn(2))
                , DampingIn(Params.FloatIn(3))
                , WidthIn(Params.FloatIn(4))
                , WetIn(Params.FloatIn(5))
                , DryIn(Params.FloatIn(6))
                , OutLeft(Params.AudioOut(0))
                , OutRight(Params.AudioOut(1))
            {
                Reverb.Initialize(2, Params.SampleRate);
            }

            void Reset() override { Reverb.Clear(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                FAudioReverbParams Settings;
                Settings.RoomSize = Math::Clamp(*RoomSizeIn, 0.0f, 1.0f);
                Settings.Damping  = Math::Clamp(*DampingIn, 0.0f, 1.0f);
                Settings.Width    = Math::Clamp(*WidthIn, 0.0f, 1.0f);
                Settings.WetLevel = Math::Max(*WetIn, 0.0f);
                Reverb.SetParams(Settings);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    Interleaved[Frame * 2 + 0] = InLeft[Frame];
                    Interleaved[Frame * 2 + 1] = InRight[Frame];
                }

                Reverb.Process(Interleaved, Tail, Context.NumFrames);

                const float Dry = *DryIn;
                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    OutLeft[Frame]  = InLeft[Frame] * Dry + Tail[Frame * 2 + 0];
                    OutRight[Frame] = InRight[Frame] * Dry + Tail[Frame * 2 + 1];
                }
            }

        private:

            const float* InLeft;
            const float* InRight;
            const float* RoomSizeIn;
            const float* DampingIn;
            const float* WidthIn;
            const float* WetIn;
            const float* DryIn;
            float*       OutLeft;
            float*       OutRight;

            FAudioReverb Reverb;
            float        Interleaved[kAudioGraphBlockFrames * 2] = {};
            float        Tail[kAudioGraphBlockFrames * 2] = {};
        };

        // Stereo linked, so a centered source keeps its image while the gain moves.
        class FCompressorOperator final : public IAudioGraphOperator
        {
        public:

            explicit FCompressorOperator(const FAudioGraphOperatorBuildParams& Params)
                : InLeft(Params.AudioIn(0))
                , InRight(Params.AudioIn(1))
                , SidechainIn(Params.AudioIn(2))
                , UseSidechainIn(Params.BoolIn(3))
                , ThresholdIn(Params.FloatIn(4))
                , RatioIn(Params.FloatIn(5))
                , AttackIn(Params.FloatIn(6))
                , ReleaseIn(Params.FloatIn(7))
                , MakeupIn(Params.FloatIn(8))
                , KneeIn(Params.FloatIn(9))
                , OutLeft(Params.AudioOut(0))
                , OutRight(Params.AudioOut(1))
                , ReductionOut(Params.FloatOut(2))
            {}

            void Reset() override { Reduction = FEnvelopeDetector(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const float Threshold = *ThresholdIn;
                const float Slope     = 1.0f / Math::Max(*RatioIn, 1.0f) - 1.0f;
                const float Knee      = Math::Max(*KneeIn, 0.0f);
                const float Makeup    = *MakeupIn;
                const bool  bExternal = *UseSidechainIn != 0;

                const float Attack  = SmoothingCoefficient(Math::Max(*AttackIn, 0.0f), Context.SampleRate);
                const float Release = SmoothingCoefficient(Math::Max(*ReleaseIn, 0.0f), Context.SampleRate);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float Level = bExternal
                        ? Math::Abs(SidechainIn[Frame])
                        : Math::Max(Math::Abs(InLeft[Frame]), Math::Abs(InRight[Frame]));

                    const float Over = LinearToDecibels(Level) - Threshold;
                    float Target = 0.0f;
                    if (Knee > 0.0f && Math::Abs(Over) * 2.0f <= Knee)
                    {
                        const float IntoKnee = Over + Knee * 0.5f;
                        Target = Slope * IntoKnee * IntoKnee / (2.0f * Knee);
                    }
                    else if (Over > 0.0f)
                    {
                        Target = Slope * Over;
                    }

                    // Tracked as a positive amount, so deeper reduction is the rise the attack time governs.
                    const float Smoothed = -Reduction.Step(-Target, Attack, Release);
                    const float Gain     = DecibelsToLinear(Smoothed + Makeup);

                    OutLeft[Frame]  = InLeft[Frame] * Gain;
                    OutRight[Frame] = InRight[Frame] * Gain;
                }

                *ReductionOut = Reduction.Value;
            }

        private:

            const float* InLeft;
            const float* InRight;
            const float* SidechainIn;
            const uint8* UseSidechainIn;
            const float* ThresholdIn;
            const float* RatioIn;
            const float* AttackIn;
            const float* ReleaseIn;
            const float* MakeupIn;
            const float* KneeIn;
            float*       OutLeft;
            float*       OutRight;
            float*       ReductionOut;

            FEnvelopeDetector Reduction;
        };

        class FEnvelopeFollowerOperator final : public IAudioGraphOperator
        {
        public:

            explicit FEnvelopeFollowerOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , AttackIn(Params.FloatIn(1))
                , ReleaseIn(Params.FloatIn(2))
                , EnvelopeOut(Params.FloatOut(0))
                , SignalOut(Params.AudioOut(1))
            {}

            void Reset() override { Detector = FEnvelopeDetector(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const float Attack  = SmoothingCoefficient(Math::Max(*AttackIn, 0.0f), Context.SampleRate);
                const float Release = SmoothingCoefficient(Math::Max(*ReleaseIn, 0.0f), Context.SampleRate);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    SignalOut[Frame] = Detector.Step(Math::Abs(In[Frame]), Attack, Release);
                }

                *EnvelopeOut = Detector.Value;
            }

        private:

            const float* In;
            const float* AttackIn;
            const float* ReleaseIn;
            float*       EnvelopeOut;
            float*       SignalOut;

            FEnvelopeDetector Detector;
        };

        class FBitcrusherOperator final : public IAudioGraphOperator
        {
        public:

            explicit FBitcrusherOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , BitsIn(Params.FloatIn(1))
                , DownsampleIn(Params.FloatIn(2))
                , MixIn(Params.FloatIn(3))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override
            {
                Held     = 0.0f;
                HoldLeft = 0.0f;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const float Steps      = Math::Exp2(Math::Clamp(*BitsIn, 1.0f, 24.0f) - 1.0f);
                const float Downsample = Math::Clamp(*DownsampleIn, 1.0f, 64.0f);
                const float Mix        = Math::Clamp(*MixIn, 0.0f, 1.0f);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    HoldLeft -= 1.0f;
                    if (HoldLeft <= 0.0f)
                    {
                        HoldLeft += Downsample;
                        Held = Math::Round(In[Frame] * Steps) / Steps;
                    }

                    Out[Frame] = In[Frame] + (Held - In[Frame]) * Mix;
                }
            }

        private:

            const float* In;
            const float* BitsIn;
            const float* DownsampleIn;
            const float* MixIn;
            float*       Out;

            float Held     = 0.0f;
            float HoldLeft = 0.0f;
        };

        // A delay swept by a sine, which reads as chorus at tens of milliseconds and flanger at a few with feedback.
        class FChorusOperator final : public IAudioGraphOperator
        {
        public:

            explicit FChorusOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , RateIn(Params.FloatIn(1))
                , DepthIn(Params.FloatIn(2))
                , DelayIn(Params.FloatIn(3))
                , FeedbackIn(Params.FloatIn(4))
                , MixIn(Params.FloatIn(5))
                , Out(Params.AudioOut(0))
            {
                Line.Initialize((uint32)(kMaxDelaySeconds * (float)Params.SampleRate));
            }

            void Reset() override
            {
                Line.Clear();
                Phase = 0.0f;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const float SampleRate = (float)Context.SampleRate;
                const float Center     = Math::Max(*DelayIn, 0.0f) * SampleRate;
                const float Depth      = Math::Max(*DepthIn, 0.0f) * SampleRate;
                const float PhaseStep  = Math::Max(*RateIn, 0.0f) * Context.InverseSampleRate;
                const float Feedback   = Math::Clamp(*FeedbackIn, -0.95f, 0.95f);
                const float Mix        = Math::Clamp(*MixIn, 0.0f, 1.0f);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float Sweep = 0.5f + 0.5f * Math::Sin(Phase * Math::TwoPi<float>());
                    Phase = Math::Fract(Phase + PhaseStep);

                    const float Wet = Line.Read(Math::Clamp(Center + Depth * Sweep, 1.0f, Line.GetMaxDelay()));
                    Line.Write(In[Frame] + Wet * Feedback);

                    Out[Frame] = In[Frame] + (Wet - In[Frame]) * Mix;
                }
            }

        private:

            static constexpr float kMaxDelaySeconds = 0.1f;

            const float* In;
            const float* RateIn;
            const float* DepthIn;
            const float* DelayIn;
            const float* FeedbackIn;
            const float* MixIn;
            float*       Out;

            FAudioGraphDelayLine Line;
            float                Phase = 0.0f;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeReverb, FReverbOperator, "Reverb", { PinAudio, PinAudio, PinFloat, PinFloat, PinFloat, PinFloat, PinFloat }, { PinAudio, PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeCompressor, FCompressorOperator, "Compressor", { PinAudio, PinAudio, PinAudio, PinBool, PinFloat, PinFloat, PinFloat, PinFloat, PinFloat, PinFloat }, { PinAudio, PinAudio, PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeEnvelopeFollower, FEnvelopeFollowerOperator, "EnvelopeFollower", { PinAudio, PinFloat, PinFloat }, { PinFloat, PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeBitcrusher, FBitcrusherOperator, "Bitcrusher", { PinAudio, PinFloat, PinFloat, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeChorus, FChorusOperator, "Chorus", { PinAudio, PinFloat, PinFloat, PinFloat, PinFloat, PinFloat }, { PinAudio })
}
