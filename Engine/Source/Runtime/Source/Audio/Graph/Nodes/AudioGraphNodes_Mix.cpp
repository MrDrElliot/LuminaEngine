#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        constexpr float kMixHalfPi = 1.57079632679489661923f;

        class FGainOperator final : public IAudioGraphOperator
        {
        public:

            explicit FGainOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , GainIn(Params.FloatIn(1))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override { Gain.Reset(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Gain.Begin(*GainIn, Context.NumFrames);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    Out[Frame] = In[Frame] * Gain.Next();
                }

                Gain.End();
            }

        private:

            const float*             In;
            const float*             GainIn;
            float*                   Out;
            FAudioGraphSmoothedFloat Gain;
        };

        class FMixerOperator final : public IAudioGraphOperator
        {
        public:

            explicit FMixerOperator(const FAudioGraphOperatorBuildParams& Params)
                : Out(Params.AudioOut(0))
            {
                for (uint32 Channel = 0; Channel < NumChannels; ++Channel)
                {
                    In[Channel]     = Params.AudioIn(Channel * 2);
                    GainIn[Channel] = Params.FloatIn(Channel * 2 + 1);
                }
            }

            void Reset() override
            {
                for (FAudioGraphSmoothedFloat& Value : Gain)
                {
                    Value.Reset();
                }
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                for (uint32 Channel = 0; Channel < NumChannels; ++Channel)
                {
                    Gain[Channel].Begin(*GainIn[Channel], Context.NumFrames);
                }

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    float Sum = 0.0f;
                    for (uint32 Channel = 0; Channel < NumChannels; ++Channel)
                    {
                        Sum += In[Channel][Frame] * Gain[Channel].Next();
                    }
                    Out[Frame] = Sum;
                }

                for (FAudioGraphSmoothedFloat& Value : Gain)
                {
                    Value.End();
                }
            }

        private:

            static constexpr uint32 NumChannels = 4;

            const float*             In[NumChannels] = {};
            const float*             GainIn[NumChannels] = {};
            float*                   Out;
            FAudioGraphSmoothedFloat Gain[NumChannels];
        };

        class FPannerOperator final : public IAudioGraphOperator
        {
        public:

            explicit FPannerOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.AudioIn(0))
                , PanIn(Params.FloatIn(1))
                , OutLeft(Params.AudioOut(0))
                , OutRight(Params.AudioOut(1))
            {}

            void Reset() override { Pan.Reset(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Pan.Begin(Math::Clamp(*PanIn, -1.0f, 1.0f), Context.NumFrames);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float Angle = (Pan.Next() * 0.5f + 0.5f) * kMixHalfPi;
                    OutLeft[Frame]  = In[Frame] * Math::Cos(Angle);
                    OutRight[Frame] = In[Frame] * Math::Sin(Angle);
                }

                Pan.End();
            }

        private:

            const float*             In;
            const float*             PanIn;
            float*                   OutLeft;
            float*                   OutRight;
            FAudioGraphSmoothedFloat Pan;
        };

        class FCrossfadeOperator final : public IAudioGraphOperator
        {
        public:

            explicit FCrossfadeOperator(const FAudioGraphOperatorBuildParams& Params)
                : A(Params.AudioIn(0))
                , B(Params.AudioIn(1))
                , AlphaIn(Params.FloatIn(2))
                , Out(Params.AudioOut(0))
            {}

            void Reset() override { Alpha.Reset(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Alpha.Begin(Math::Clamp(*AlphaIn, 0.0f, 1.0f), Context.NumFrames);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float Blend = Alpha.Next();
                    Out[Frame] = A[Frame] * (1.0f - Blend) + B[Frame] * Blend;
                }

                Alpha.End();
            }

        private:

            const float*             A;
            const float*             B;
            const float*             AlphaIn;
            float*                   Out;
            FAudioGraphSmoothedFloat Alpha;
        };

        class FStereoWidthOperator final : public IAudioGraphOperator
        {
        public:

            explicit FStereoWidthOperator(const FAudioGraphOperatorBuildParams& Params)
                : InLeft(Params.AudioIn(0))
                , InRight(Params.AudioIn(1))
                , WidthIn(Params.FloatIn(2))
                , OutLeft(Params.AudioOut(0))
                , OutRight(Params.AudioOut(1))
            {}

            void Reset() override { Width.Reset(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Width.Begin(Math::Max(*WidthIn, 0.0f), Context.NumFrames);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    const float Amount = Width.Next();
                    const float Mid  = (InLeft[Frame] + InRight[Frame]) * 0.5f;
                    const float Side = (InLeft[Frame] - InRight[Frame]) * 0.5f * Amount;

                    OutLeft[Frame]  = Mid + Side;
                    OutRight[Frame] = Mid - Side;
                }

                Width.End();
            }

        private:

            const float*             InLeft;
            const float*             InRight;
            const float*             WidthIn;
            float*                   OutLeft;
            float*                   OutRight;
            FAudioGraphSmoothedFloat Width;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeGain, FGainOperator, "Gain", { PinAudio, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeMixer, FMixerOperator, "Mixer", { PinAudio, PinFloat, PinAudio, PinFloat, PinAudio, PinFloat, PinAudio, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodePanner, FPannerOperator, "Panner", { PinAudio, PinFloat }, { PinAudio, PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeCrossfade, FCrossfadeOperator, "Crossfade", { PinAudio, PinAudio, PinFloat }, { PinAudio })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeStereoWidth, FStereoWidthOperator, "StereoWidth", { PinAudio, PinAudio, PinFloat }, { PinAudio, PinAudio })
}
