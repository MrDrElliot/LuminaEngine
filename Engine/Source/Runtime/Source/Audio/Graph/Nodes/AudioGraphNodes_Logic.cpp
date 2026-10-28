#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kSelectInputs = 4;

        FORCEINLINE uint32 ClampSelectIndex(int32 Index) { return (uint32)Math::Clamp(Index, 0, (int32)kSelectInputs - 1); }

        // Fades between inputs rather than cutting, since a hard switch between two signals clicks.
        class FSelectAudioOperator final : public IAudioGraphOperator
        {
        public:

            explicit FSelectAudioOperator(const FAudioGraphOperatorBuildParams& Params)
                : IndexIn(Params.IntIn(0))
                , FadeIn(Params.FloatIn(kSelectInputs + 1))
                , Out(Params.AudioOut(0))
            {
                for (uint32 Index = 0; Index < kSelectInputs; ++Index)
                {
                    Ins[Index] = Params.AudioIn(Index + 1);
                }
            }

            void Reset() override { bPrimed = false; }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                const uint32 Selected = ClampSelectIndex(*IndexIn);
                if (!bPrimed)
                {
                    bPrimed = true;
                    for (uint32 Index = 0; Index < kSelectInputs; ++Index)
                    {
                        Gains[Index] = Index == Selected ? 1.0f : 0.0f;
                    }
                }

                const float FadeFrames = Math::Max(*FadeIn, 0.0f) * (float)Context.SampleRate;
                const float Step       = FadeFrames >= 1.0f ? 1.0f / FadeFrames : 1.0f;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    float Sum = 0.0f;
                    for (uint32 Index = 0; Index < kSelectInputs; ++Index)
                    {
                        const float Target = Index == Selected ? 1.0f : 0.0f;
                        Gains[Index] = Gains[Index] < Target ? Math::Min(Gains[Index] + Step, Target) : Math::Max(Gains[Index] - Step, Target);
                        Sum += Ins[Index][Frame] * Gains[Index];
                    }
                    Out[Frame] = Sum;
                }
            }

        private:

            const int32* IndexIn;
            const float* Ins[kSelectInputs] = {};
            const float* FadeIn;
            float*       Out;

            float Gains[kSelectInputs] = {};
            bool  bPrimed = false;
        };

        class FSelectFloatOperator final : public IAudioGraphOperator
        {
        public:

            explicit FSelectFloatOperator(const FAudioGraphOperatorBuildParams& Params)
                : IndexIn(Params.IntIn(0))
                , Out(Params.FloatOut(0))
            {
                for (uint32 Index = 0; Index < kSelectInputs; ++Index)
                {
                    Ins[Index] = Params.FloatIn(Index + 1);
                }
            }

            void Execute(const FAudioGraphBlockContext&) override
            {
                *Out = *Ins[ClampSelectIndex(*IndexIn)];
            }

        private:

            const int32* IndexIn;
            const float* Ins[kSelectInputs] = {};
            float*       Out;
        };

        class FCompareFloatOperator final : public IAudioGraphOperator
        {
        public:

            explicit FCompareFloatOperator(const FAudioGraphOperatorBuildParams& Params)
                : A(Params.FloatIn(0))
                , B(Params.FloatIn(1))
                , OpIn(Params.IntIn(2))
                , Out(Params.BoolOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                bool bResult = false;
                switch ((EAudioCompareOp)*OpIn)
                {
                case EAudioCompareOp::Equal:        bResult = *A == *B; break;
                case EAudioCompareOp::NotEqual:     bResult = *A != *B; break;
                case EAudioCompareOp::Less:         bResult = *A < *B;  break;
                case EAudioCompareOp::LessEqual:    bResult = *A <= *B; break;
                case EAudioCompareOp::Greater:      bResult = *A > *B;  break;
                case EAudioCompareOp::GreaterEqual: bResult = *A >= *B; break;
                }
                *Out = bResult ? 1 : 0;
            }

        private:

            const float* A;
            const float* B;
            const int32* OpIn;
            uint8*       Out;
        };

        class FLogicBoolOperator final : public IAudioGraphOperator
        {
        public:

            explicit FLogicBoolOperator(const FAudioGraphOperatorBuildParams& Params)
                : A(Params.BoolIn(0))
                , B(Params.BoolIn(1))
                , OpIn(Params.IntIn(2))
                , Out(Params.BoolOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override
            {
                const bool bA = *A != 0;
                const bool bB = *B != 0;

                bool bResult = false;
                switch ((EAudioLogicOp)*OpIn)
                {
                case EAudioLogicOp::And:  bResult = bA && bB;    break;
                case EAudioLogicOp::Or:   bResult = bA || bB;    break;
                case EAudioLogicOp::Xor:  bResult = bA != bB;    break;
                case EAudioLogicOp::NotA: bResult = !bA;         break;
                case EAudioLogicOp::Nand: bResult = !(bA && bB); break;
                case EAudioLogicOp::Nor:  bResult = !(bA || bB); break;
                }
                *Out = bResult ? 1 : 0;
            }

        private:

            const uint8* A;
            const uint8* B;
            const int32* OpIn;
            uint8*       Out;
        };

        class FFloatToIntOperator final : public IAudioGraphOperator
        {
        public:

            explicit FFloatToIntOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.FloatIn(0))
                , Out(Params.IntOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override { *Out = (int32)Math::Round(*In); }

        private:

            const float* In;
            int32*       Out;
        };

        class FIntToFloatOperator final : public IAudioGraphOperator
        {
        public:

            explicit FIntToFloatOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.IntIn(0))
                , Out(Params.FloatOut(0))
            {}

            void Execute(const FAudioGraphBlockContext&) override { *Out = (float)*In; }

        private:

            const int32* In;
            float*       Out;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSelectAudio, FSelectAudioOperator, "Select.Audio", { PinInt32, PinAudio, PinAudio, PinAudio, PinAudio, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSelectFloat, FSelectFloatOperator, "Select.Float", { PinInt32, PinFloat, PinFloat, PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeCompareFloat, FCompareFloatOperator, "Compare.Float", { PinFloat, PinFloat, PinInt32 }, { PinBool })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeLogicBool, FLogicBoolOperator, "Logic.Bool", { PinBool, PinBool, PinInt32 }, { PinBool })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeFloatToInt, FFloatToIntOperator, "FloatToInt", { PinFloat }, { PinInt32 })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeIntToFloat, FIntToFloatOperator, "IntToFloat", { PinInt32 }, { PinFloat })
}
