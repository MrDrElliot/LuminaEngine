#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        enum class EAudioMathOp : uint8
        {
            Add,
            Subtract,
            Multiply,
            Divide,
            Min,
            Max,
            Power,
        };

        FORCEINLINE float ApplyMathOp(EAudioMathOp Op, float A, float B)
        {
            switch (Op)
            {
            case EAudioMathOp::Add:      return A + B;
            case EAudioMathOp::Subtract: return A - B;
            case EAudioMathOp::Multiply: return A * B;
            case EAudioMathOp::Divide:   return B != 0.0f ? A / B : 0.0f;
            case EAudioMathOp::Min:      return Math::Min(A, B);
            case EAudioMathOp::Max:      return Math::Max(A, B);
            case EAudioMathOp::Power:    return Math::Pow(A, B);
            default:                     return A;
            }
        }

        template <EAudioMathOp Op>
        class TFloatMathOperator final : public IAudioGraphOperator
        {
        public:

            explicit TFloatMathOperator(const FAudioGraphOperatorBuildParams& Params)
                : A(Params.FloatIn(0))
                , B(Params.FloatIn(1))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                *Out = ApplyMathOp(Op, *A, *B);
            }

        private:

            const float* A;
            const float* B;
            float*       Out;
        };

        template <EAudioMathOp Op>
        class TAudioMathOperator final : public IAudioGraphOperator
        {
        public:

            explicit TAudioMathOperator(const FAudioGraphOperatorBuildParams& Params)
                : A(Params.AudioIn(0))
                , B(Params.AudioIn(1))
                , Out(Params.AudioOut(0))
            {}

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    Out[Frame] = ApplyMathOp(Op, A[Frame], B[Frame]);
                }
            }

        private:

            const float* A;
            const float* B;
            float*       Out;
        };

        class FClampOperator final : public IAudioGraphOperator
        {
        public:

            explicit FClampOperator(const FAudioGraphOperatorBuildParams& Params)
                : Value(Params.FloatIn(0))
                , Low(Params.FloatIn(1))
                , High(Params.FloatIn(2))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                const float Lo = *Low;
                const float Hi = *High;
                *Out = Hi < Lo ? Math::Clamp(*Value, Hi, Lo) : Math::Clamp(*Value, Lo, Hi);
            }

        private:

            const float* Value;
            const float* Low;
            const float* High;
            float*       Out;
        };

        class FMapRangeOperator final : public IAudioGraphOperator
        {
        public:

            explicit FMapRangeOperator(const FAudioGraphOperatorBuildParams& Params)
                : Value(Params.FloatIn(0))
                , InMin(Params.FloatIn(1))
                , InMax(Params.FloatIn(2))
                , OutMin(Params.FloatIn(3))
                , OutMax(Params.FloatIn(4))
                , bClamp(Params.BoolIn(5))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                const float Span = *InMax - *InMin;
                float Alpha = Span != 0.0f ? (*Value - *InMin) / Span : 0.0f;

                if (*bClamp != 0)
                {
                    Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
                }

                *Out = *OutMin + (*OutMax - *OutMin) * Alpha;
            }

        private:

            const float* Value;
            const float* InMin;
            const float* InMax;
            const float* OutMin;
            const float* OutMax;
            const uint8* bClamp;
            float*       Out;
        };

        class FMidiToFrequencyOperator final : public IAudioGraphOperator
        {
        public:

            explicit FMidiToFrequencyOperator(const FAudioGraphOperatorBuildParams& Params)
                : Note(Params.FloatIn(0))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                *Out = 440.0f * Math::Pow(2.0f, (*Note - 69.0f) / 12.0f);
            }

        private:

            const float* Note;
            float*       Out;
        };

        class FDecibelsToLinearOperator final : public IAudioGraphOperator
        {
        public:

            explicit FDecibelsToLinearOperator(const FAudioGraphOperatorBuildParams& Params)
                : Decibels(Params.FloatIn(0))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                *Out = Math::Pow(10.0f, *Decibels * 0.05f);
            }

        private:

            const float* Decibels;
            float*       Out;
        };

        class FLinearToDecibelsOperator final : public IAudioGraphOperator
        {
        public:

            explicit FLinearToDecibelsOperator(const FAudioGraphOperatorBuildParams& Params)
                : Linear(Params.FloatIn(0))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                const float Value = Math::Max(Math::Abs(*Linear), 1.0e-6f);
                *Out = 20.0f * Math::Log(Value) * 0.4342944819f;
            }

        private:

            const float* Linear;
            float*       Out;
        };

        class FSemitonesToRatioOperator final : public IAudioGraphOperator
        {
        public:

            explicit FSemitonesToRatioOperator(const FAudioGraphOperatorBuildParams& Params)
                : Semitones(Params.FloatIn(0))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                *Out = Math::Pow(2.0f, *Semitones / 12.0f);
            }

        private:

            const float* Semitones;
            float*       Out;
        };

        class FFloatToAudioOperator final : public IAudioGraphOperator
        {
        public:

            explicit FFloatToAudioOperator(const FAudioGraphOperatorBuildParams& Params)
                : Value(Params.FloatIn(0))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override { Smoothed.Reset(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Smoothed.Begin(*Value, Context.NumFrames);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    Out[Frame] = Smoothed.Next();
                }

                Smoothed.End();
            }

        private:

            const float*             Value;
            float*                   Out;
            FAudioGraphSmoothedFloat Smoothed;
        };

        class FAudioToFloatOperator final : public IAudioGraphOperator
        {
        public:

            explicit FAudioToFloatOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , ModeIn(Params.IntIn(1))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                if (Context.NumFrames == 0)
                {
                    return;
                }

                switch (*ModeIn)
                {
                case 1:
                    {
                        float Peak = 0.0f;
                        for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                        {
                            Peak = Math::Max(Peak, Math::Abs(In[Frame]));
                        }
                        *Out = Peak;
                    }
                    break;

                case 2:
                    {
                        float SumOfSquares = 0.0f;
                        for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                        {
                            SumOfSquares += In[Frame] * In[Frame];
                        }
                        *Out = Math::Sqrt(SumOfSquares / (float)Context.NumFrames);
                    }
                    break;

                default:
                    *Out = In[Context.NumFrames - 1];
                    break;
                }
            }

        private:

            const float* In;
            const int32* ModeIn;
            float*       Out;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeAddFloat, TFloatMathOperator<EAudioMathOp::Add>, "Add.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSubtractFloat, TFloatMathOperator<EAudioMathOp::Subtract>, "Subtract.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeMultiplyFloat, TFloatMathOperator<EAudioMathOp::Multiply>, "Multiply.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeDivideFloat, TFloatMathOperator<EAudioMathOp::Divide>, "Divide.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeMinFloat, TFloatMathOperator<EAudioMathOp::Min>, "Min.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeMaxFloat, TFloatMathOperator<EAudioMathOp::Max>, "Max.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodePowerFloat, TFloatMathOperator<EAudioMathOp::Power>, "Power.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeAddAudio, TAudioMathOperator<EAudioMathOp::Add>, "Add.Audio", { PinAudio, PinAudio }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSubtractAudio, TAudioMathOperator<EAudioMathOp::Subtract>, "Subtract.Audio", { PinAudio, PinAudio }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeMultiplyAudio, TAudioMathOperator<EAudioMathOp::Multiply>, "Multiply.Audio", { PinAudio, PinAudio }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeClamp, FClampOperator, "Clamp.Float", { PinFloat, PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeMapRange, FMapRangeOperator, "MapRange.Float", { PinFloat, PinFloat, PinFloat, PinFloat, PinFloat, PinBool }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeMidiToFrequency, FMidiToFrequencyOperator, "MidiToFrequency", { PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeDecibelsToLinear, FDecibelsToLinearOperator, "DecibelsToLinear", { PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeLinearToDecibels, FLinearToDecibelsOperator, "LinearToDecibels", { PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSemitonesToRatio, FSemitonesToRatioOperator, "SemitonesToRatio", { PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeFloatToAudio, FFloatToAudioOperator, "FloatToAudio", { PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeAudioToFloat, FAudioToFloatOperator, "AudioToFloat", { PinAudio, PinInt32 }, { PinFloat })
}
