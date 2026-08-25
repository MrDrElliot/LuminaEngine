#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

#include <cmath>

namespace Lumina
{
    namespace
    {
        constexpr float kFilterPi = 3.14159265358979323846f;

        /** Longest delay a Delay node can address, and therefore the buffer it allocates up front. */
        constexpr float kMaxDelaySeconds = 2.0f;

        class FOnePoleOperator : public IAudioGraphOperator
        {
        public:

            FOnePoleOperator(const FAudioGraphOperatorBuildParams& Params, bool bInHighPass)
                : In(Params.AudioIn(0))
                , CutoffIn(Params.FloatIn(1))
                , Out(Params.AudioOut(0))
                , bHighPass(bInHighPass)
            {}

            void Reset() override { State = 0.0f; }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const float Nyquist = (float)Context.SampleRate * 0.5f;
                const float Cutoff  = Math::Clamp(*CutoffIn, 1.0f, Nyquist * 0.99f);
                const float Coefficient = 1.0f - Math::Exp(-2.0f * kFilterPi * Cutoff * Context.InverseSampleRate);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    State += Coefficient * (In[Frame] - State);
                    Out[Frame] = bHighPass ? In[Frame] - State : State;
                }
            }

        private:

            const float* In;
            const float* CutoffIn;
            float*       Out;

            float State = 0.0f;
            bool  bHighPass = false;
        };

        class FOnePoleLowPassOperator final : public FOnePoleOperator
        {
        public:
            explicit FOnePoleLowPassOperator(const FAudioGraphOperatorBuildParams& Params)
                : FOnePoleOperator(Params, false) {}
        };

        class FOnePoleHighPassOperator final : public FOnePoleOperator
        {
        public:
            explicit FOnePoleHighPassOperator(const FAudioGraphOperatorBuildParams& Params)
                : FOnePoleOperator(Params, true) {}
        };

        class FBiquadOperator final : public IAudioGraphOperator
        {
        public:

            explicit FBiquadOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , TypeIn(Params.IntIn(1))
                , FrequencyIn(Params.FloatIn(2))
                , QIn(Params.FloatIn(3))
                , GainDecibelsIn(Params.FloatIn(4))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override
            {
                X1 = X2 = Y1 = Y2 = 0.0f;
                bCoefficientsValid = false;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                UpdateCoefficients(Context);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float X = In[Frame];
                    const float Y = B0 * X + B1 * X1 + B2 * X2 - A1 * Y1 - A2 * Y2;

                    X2 = X1;
                    X1 = X;
                    Y2 = Y1;
                    Y1 = Y;

                    Out[Frame] = Y;
                }

                // Denormals in the feedback path cost hundreds of cycles a sample once the tail decays.
                FlushDenormal(Y1);
                FlushDenormal(Y2);
            }

        private:

            static void FlushDenormal(float& Value)
            {
                if (Math::Abs(Value) < 1.0e-20f)
                {
                    Value = 0.0f;
                }
            }

            void UpdateCoefficients(const FAudioGraphBlockContext& Context)
            {
                const float Nyquist   = (float)Context.SampleRate * 0.5f;
                const float Frequency = Math::Clamp(*FrequencyIn, 10.0f, Nyquist * 0.99f);
                const float Q         = Math::Max(*QIn, 0.05f);
                const float GainDb    = *GainDecibelsIn;
                const int32 Type      = *TypeIn;

                if (bCoefficientsValid && Type == CachedType && Frequency == CachedFrequency
                    && Q == CachedQ && GainDb == CachedGain)
                {
                    return;
                }

                CachedType      = Type;
                CachedFrequency = Frequency;
                CachedQ         = Q;
                CachedGain      = GainDb;
                bCoefficientsValid = true;

                const float Omega = 2.0f * kFilterPi * Frequency * Context.InverseSampleRate;
                const float SinOmega = Math::Sin(Omega);
                const float CosOmega = Math::Cos(Omega);
                const float Alpha = SinOmega / (2.0f * Q);
                const float A = Math::Pow(10.0f, GainDb * 0.025f);

                float NB0 = 1.0f;
                float NB1 = 0.0f;
                float NB2 = 0.0f;
                float NA0 = 1.0f;
                float NA1 = 0.0f;
                float NA2 = 0.0f;

                switch (Type)
                {
                case 1:
                    NB0 = (1.0f + CosOmega) * 0.5f;
                    NB1 = -(1.0f + CosOmega);
                    NB2 = NB0;
                    NA0 = 1.0f + Alpha;
                    NA1 = -2.0f * CosOmega;
                    NA2 = 1.0f - Alpha;
                    break;

                case 2:
                    NB0 = Alpha;
                    NB1 = 0.0f;
                    NB2 = -Alpha;
                    NA0 = 1.0f + Alpha;
                    NA1 = -2.0f * CosOmega;
                    NA2 = 1.0f - Alpha;
                    break;

                case 3:
                    NB0 = 1.0f;
                    NB1 = -2.0f * CosOmega;
                    NB2 = 1.0f;
                    NA0 = 1.0f + Alpha;
                    NA1 = NB1;
                    NA2 = 1.0f - Alpha;
                    break;

                case 4:
                    NB0 = 1.0f + Alpha * A;
                    NB1 = -2.0f * CosOmega;
                    NB2 = 1.0f - Alpha * A;
                    NA0 = 1.0f + Alpha / A;
                    NA1 = NB1;
                    NA2 = 1.0f - Alpha / A;
                    break;

                case 5:
                    {
                        const float Root = 2.0f * Math::Sqrt(A) * Alpha;
                        NB0 = A * ((A + 1.0f) - (A - 1.0f) * CosOmega + Root);
                        NB1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * CosOmega);
                        NB2 = A * ((A + 1.0f) - (A - 1.0f) * CosOmega - Root);
                        NA0 = (A + 1.0f) + (A - 1.0f) * CosOmega + Root;
                        NA1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * CosOmega);
                        NA2 = (A + 1.0f) + (A - 1.0f) * CosOmega - Root;
                    }
                    break;

                case 6:
                    {
                        const float Root = 2.0f * Math::Sqrt(A) * Alpha;
                        NB0 = A * ((A + 1.0f) + (A - 1.0f) * CosOmega + Root);
                        NB1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * CosOmega);
                        NB2 = A * ((A + 1.0f) + (A - 1.0f) * CosOmega - Root);
                        NA0 = (A + 1.0f) - (A - 1.0f) * CosOmega + Root;
                        NA1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * CosOmega);
                        NA2 = (A + 1.0f) - (A - 1.0f) * CosOmega - Root;
                    }
                    break;

                default:
                    NB0 = (1.0f - CosOmega) * 0.5f;
                    NB1 = 1.0f - CosOmega;
                    NB2 = NB0;
                    NA0 = 1.0f + Alpha;
                    NA1 = -2.0f * CosOmega;
                    NA2 = 1.0f - Alpha;
                    break;
                }

                const float InverseA0 = NA0 != 0.0f ? 1.0f / NA0 : 1.0f;
                B0 = NB0 * InverseA0;
                B1 = NB1 * InverseA0;
                B2 = NB2 * InverseA0;
                A1 = NA1 * InverseA0;
                A2 = NA2 * InverseA0;
            }

            const float* In;
            const int32* TypeIn;
            const float* FrequencyIn;
            const float* QIn;
            const float* GainDecibelsIn;
            float*       Out;

            float B0 = 1.0f, B1 = 0.0f, B2 = 0.0f, A1 = 0.0f, A2 = 0.0f;
            float X1 = 0.0f, X2 = 0.0f, Y1 = 0.0f, Y2 = 0.0f;

            int32 CachedType      = -1;
            float CachedFrequency = 0.0f;
            float CachedQ         = 0.0f;
            float CachedGain      = 0.0f;
            bool  bCoefficientsValid = false;
        };

        class FDelayOperator final : public IAudioGraphOperator
        {
        public:

            explicit FDelayOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , DelayTimeIn(Params.FloatIn(1))
                , FeedbackIn(Params.FloatIn(2))
                , DryLevelIn(Params.FloatIn(3))
                , WetLevelIn(Params.FloatIn(4))
                , Out(Params.AudioOut(0))
                , SampleRate(Params.SampleRate)
            {
                Buffer.resize((size_t)(kMaxDelaySeconds * (float)SampleRate) + 1, 0.0f);
            }

            void Reset() override
            {
                for (float& Sample : Buffer)
                {
                    Sample = 0.0f;
                }
                WriteIndex = 0;
                DelaySamples.Reset();
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const size_t Capacity = Buffer.size();
                if (Capacity == 0)
                {
                    return;
                }

                const float MaxSamples = (float)(Capacity - 2);
                const float TargetDelay = Math::Clamp(*DelayTimeIn * (float)Context.SampleRate, 1.0f, MaxSamples);
                const float Feedback = Math::Clamp(*FeedbackIn, 0.0f, 0.99f);
                const float Dry = *DryLevelIn;
                const float Wet = *WetLevelIn;

                DelaySamples.Begin(TargetDelay, Context.NumFrames);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float Delay = DelaySamples.Next();

                    float ReadPosition = (float)WriteIndex - Delay;
                    while (ReadPosition < 0.0f)
                    {
                        ReadPosition += (float)Capacity;
                    }

                    const size_t Index = (size_t)ReadPosition;
                    const size_t Next  = (Index + 1) % Capacity;
                    const float  Alpha = ReadPosition - (float)Index;

                    const float Delayed = Buffer[Index % Capacity] + (Buffer[Next] - Buffer[Index % Capacity]) * Alpha;

                    Buffer[WriteIndex] = In[Frame] + Delayed * Feedback;
                    WriteIndex = (WriteIndex + 1) % Capacity;

                    Out[Frame] = In[Frame] * Dry + Delayed * Wet;
                }

                DelaySamples.End();
            }

        private:

            const float* In;
            const float* DelayTimeIn;
            const float* FeedbackIn;
            const float* DryLevelIn;
            const float* WetLevelIn;
            float*       Out;

            TVector<float>           Buffer;
            FAudioGraphSmoothedFloat DelaySamples;
            size_t                   WriteIndex = 0;
            uint32                   SampleRate = 48000;
        };

        class FSaturationOperator final : public IAudioGraphOperator
        {
        public:

            explicit FSaturationOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , DriveIn(Params.FloatIn(1))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override { Drive.Reset(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Drive.Begin(Math::Max(*DriveIn, 0.01f), Context.NumFrames);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float Amount = Drive.Next();
                    Out[Frame] = std::tanh(In[Frame] * Amount) / Math::Max(Amount, 1.0f);
                }

                Drive.End();
            }

        private:

            const float*             In;
            const float*             DriveIn;
            float*                   Out;
            FAudioGraphSmoothedFloat Drive;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeOnePoleLowPass, FOnePoleLowPassOperator, "OnePoleLowPass", { PinAudio, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeOnePoleHighPass, FOnePoleHighPassOperator, "OnePoleHighPass", { PinAudio, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeBiquad, FBiquadOperator, "BiquadFilter", { PinAudio, PinInt32, PinFloat, PinFloat, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeDelay, FDelayOperator, "Delay", { PinAudio, PinFloat, PinFloat, PinFloat, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSaturation, FSaturationOperator, "Saturation", { PinAudio, PinFloat }, { PinAudio })
}
