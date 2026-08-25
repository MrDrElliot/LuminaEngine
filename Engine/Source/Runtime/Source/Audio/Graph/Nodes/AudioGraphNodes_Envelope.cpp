#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        constexpr float kEnvelopeTwoPi = 6.28318530717958647692f;

        /** Frames a time in seconds covers, floored to one so a zero time still advances. */
        FORCEINLINE float StageRate(float Seconds, uint32 SampleRate)
        {
            const float Frames = Math::Max(Seconds, 0.0f) * (float)SampleRate;
            return Frames > 1.0f ? 1.0f / Frames : 1.0f;
        }

        class FADSROperator final : public IAudioGraphOperator
        {
        public:

            explicit FADSROperator(const FAudioGraphOperatorBuildParams& Params)
                : AttackTriggerIn(Params.TriggerIn(0))
                , ReleaseTriggerIn(Params.TriggerIn(1))
                , AttackTimeIn(Params.FloatIn(2))
                , DecayTimeIn(Params.FloatIn(3))
                , SustainLevelIn(Params.FloatIn(4))
                , ReleaseTimeIn(Params.FloatIn(5))
                , Out(Params.AudioOut(0))
                , LevelOut(Params.FloatOut(1))
                , FinishedOut(Params.TriggerOut(2))
            {}

            void Reset() override
            {
                Stage = EStage::Idle;
                Level = 0.0f;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                AttackCursor.Reset();
                ReleaseCursor.Reset();

                const float AttackRate  = StageRate(*AttackTimeIn, Context.SampleRate);
                const float DecayRate   = StageRate(*DecayTimeIn, Context.SampleRate);
                const float ReleaseRate = StageRate(*ReleaseTimeIn, Context.SampleRate);
                const float Sustain     = Math::Clamp(*SustainLevelIn, 0.0f, 1.0f);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (AttackCursor.Consume(*AttackTriggerIn, Frame))
                    {
                        Stage = EStage::Attack;
                    }

                    if (ReleaseCursor.Consume(*ReleaseTriggerIn, Frame))
                    {
                        Stage = EStage::Release;
                    }

                    switch (Stage)
                    {
                    case EStage::Attack:
                        Level += AttackRate;
                        if (Level >= 1.0f)
                        {
                            Level = 1.0f;
                            Stage = EStage::Decay;
                        }
                        break;

                    case EStage::Decay:
                        Level -= DecayRate * (1.0f - Sustain);
                        if (Level <= Sustain)
                        {
                            Level = Sustain;
                            Stage = EStage::Sustain;
                        }
                        break;

                    case EStage::Sustain:
                        Level = Sustain;
                        break;

                    case EStage::Release:
                        Level -= ReleaseRate;
                        if (Level <= 0.0f)
                        {
                            Level = 0.0f;
                            Stage = EStage::Idle;
                            FinishedOut->Add(Frame);
                        }
                        break;

                    default:
                        Level = 0.0f;
                        break;
                    }

                    Out[Frame] = Level;
                }

                *LevelOut = Level;
            }

        private:

            enum class EStage : uint8
            {
                Idle,
                Attack,
                Decay,
                Sustain,
                Release,
            };

            const FAudioGraphTriggerBuffer* AttackTriggerIn;
            const FAudioGraphTriggerBuffer* ReleaseTriggerIn;
            const float*                    AttackTimeIn;
            const float*                    DecayTimeIn;
            const float*                    SustainLevelIn;
            const float*                    ReleaseTimeIn;

            float*                          Out;
            float*                          LevelOut;
            FAudioGraphTriggerBuffer*       FinishedOut;

            FAudioGraphTriggerCursor AttackCursor;
            FAudioGraphTriggerCursor ReleaseCursor;

            EStage Stage = EStage::Idle;
            float  Level = 0.0f;
        };

        class FAttackDecayOperator final : public IAudioGraphOperator
        {
        public:

            explicit FAttackDecayOperator(const FAudioGraphOperatorBuildParams& Params)
                : TriggerIn(Params.TriggerIn(0))
                , AttackTimeIn(Params.FloatIn(1))
                , DecayTimeIn(Params.FloatIn(2))
                , CurveIn(Params.FloatIn(3))
                , Out(Params.AudioOut(0))
                , FinishedOut(Params.TriggerOut(1))
            {}

            void Reset() override
            {
                Phase    = 0.0f;
                bActive  = false;
                bDecaying = false;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Cursor.Reset();

                const float AttackRate = StageRate(*AttackTimeIn, Context.SampleRate);
                const float DecayRate  = StageRate(*DecayTimeIn, Context.SampleRate);
                const float Curve      = Math::Max(*CurveIn, 0.01f);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*TriggerIn, Frame))
                    {
                        Phase     = 0.0f;
                        bActive   = true;
                        bDecaying = false;
                    }

                    if (!bActive)
                    {
                        Out[Frame] = 0.0f;
                        continue;
                    }

                    if (!bDecaying)
                    {
                        Phase += AttackRate;
                        if (Phase >= 1.0f)
                        {
                            Phase     = 1.0f;
                            bDecaying = true;
                        }
                        Out[Frame] = Phase;
                    }
                    else
                    {
                        Phase -= DecayRate;
                        if (Phase <= 0.0f)
                        {
                            Phase   = 0.0f;
                            bActive = false;
                            FinishedOut->Add(Frame);
                        }
                        Out[Frame] = Math::Pow(Phase, Curve);
                    }
                }
            }

        private:

            const FAudioGraphTriggerBuffer* TriggerIn;
            const float*                    AttackTimeIn;
            const float*                    DecayTimeIn;
            const float*                    CurveIn;

            float*                          Out;
            FAudioGraphTriggerBuffer*       FinishedOut;

            FAudioGraphTriggerCursor Cursor;

            float Phase     = 0.0f;
            bool  bActive   = false;
            bool  bDecaying = false;
        };

        class FLFOOperator final : public IAudioGraphOperator
        {
        public:

            explicit FLFOOperator(const FAudioGraphOperatorBuildParams& Params)
                : FrequencyIn(Params.FloatIn(0))
                , ShapeIn(Params.IntIn(1))
                , AmplitudeIn(Params.FloatIn(2))
                , OffsetIn(Params.FloatIn(3))
                , SyncIn(Params.TriggerIn(4))
                , AudioOut(Params.AudioOut(0))
                , ValueOut(Params.FloatOut(1))
            {}

            void Reset() override { Phase = 0.0f; }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Cursor.Reset();

                const float Increment = *FrequencyIn * Context.InverseSampleRate;
                const float Amplitude = *AmplitudeIn;
                const float Offset    = *OffsetIn;
                const int32 Shape     = *ShapeIn;

                float Value = 0.0f;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*SyncIn, Frame))
                    {
                        Phase = 0.0f;
                    }

                    Value = Shaped(Shape, Phase) * Amplitude + Offset;
                    AudioOut[Frame] = Value;

                    Phase += Increment;
                    while (Phase >= 1.0f) { Phase -= 1.0f; }
                    while (Phase < 0.0f)  { Phase += 1.0f; }
                }

                *ValueOut = Value;
            }

        private:

            static float Shaped(int32 Shape, float Phase)
            {
                switch (Shape)
                {
                case 1:  return 2.0f * Phase - 1.0f;
                case 2:  return Phase < 0.5f ? 1.0f : -1.0f;
                case 3:  return 4.0f * Math::Abs(Phase - 0.5f) - 1.0f;
                default: return Math::Sin(Phase * kEnvelopeTwoPi);
                }
            }

            const float*                    FrequencyIn;
            const int32*                    ShapeIn;
            const float*                    AmplitudeIn;
            const float*                    OffsetIn;
            const FAudioGraphTriggerBuffer* SyncIn;

            float* AudioOut;
            float* ValueOut;

            FAudioGraphTriggerCursor Cursor;
            float                    Phase = 0.0f;
        };

        class FInterpToOperator final : public IAudioGraphOperator
        {
        public:

            explicit FInterpToOperator(const FAudioGraphOperatorBuildParams& Params)
                : TargetIn(Params.FloatIn(0))
                , TimeIn(Params.FloatIn(1))
                , Out(Params.FloatOut(0))
                , BlockSeconds((float)kAudioGraphBlockFrames / (float)Params.SampleRate)
            {}

            void Reset() override
            {
                Value   = 0.0f;
                bPrimed = false;
            }

            void Execute(const FAudioGraphBlockContext&) override
            {
                const float Target = *TargetIn;

                if (!bPrimed)
                {
                    bPrimed = true;
                    Value   = Target;
                }
                else
                {
                    const float HalfLife = Math::Max(*TimeIn, 0.0f);
                    const float Alpha = HalfLife > 0.0f
                        ? 1.0f - Math::Exp(-BlockSeconds / HalfLife)
                        : 1.0f;
                    Value += (Target - Value) * Alpha;
                }

                *Out = Value;
            }

        private:

            const float* TargetIn;
            const float* TimeIn;
            float*       Out;

            float BlockSeconds = 0.0f;
            float Value        = 0.0f;
            bool  bPrimed      = false;
        };

        class FSampleAndHoldOperator final : public IAudioGraphOperator
        {
        public:

            explicit FSampleAndHoldOperator(const FAudioGraphOperatorBuildParams& Params)
                : TriggerIn(Params.TriggerIn(0))
                , ValueIn(Params.FloatIn(1))
                , Out(Params.FloatOut(0))
            {}

            void Reset() override { Held = 0.0f; }

            void Execute(const FAudioGraphBlockContext&) override
            {
                if (!TriggerIn->IsEmpty())
                {
                    Held = *ValueIn;
                }

                *Out = Held;
            }

        private:

            const FAudioGraphTriggerBuffer* TriggerIn;
            const float*                    ValueIn;
            float*                          Out;

            float Held = 0.0f;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeADSR, FADSROperator, "ADSR", { PinTrigger, PinTrigger, PinFloat, PinFloat, PinFloat, PinFloat }, { PinAudio, PinFloat, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeAttackDecay, FAttackDecayOperator, "AttackDecay", { PinTrigger, PinFloat, PinFloat, PinFloat }, { PinAudio, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeLFO, FLFOOperator, "LFO", { PinFloat, PinInt32, PinFloat, PinFloat, PinTrigger }, { PinAudio, PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeInterpTo, FInterpToOperator, "InterpTo.Float", { PinFloat, PinFloat }, { PinFloat })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeSampleAndHold, FSampleAndHoldOperator, "SampleAndHold.Float", { PinTrigger, PinFloat }, { PinFloat })
}
