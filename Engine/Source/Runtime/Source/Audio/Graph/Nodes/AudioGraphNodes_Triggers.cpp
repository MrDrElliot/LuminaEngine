#include "RuntimePCH.h"

#include "Audio/Graph/AudioGraphNodeBuilder.h"
#include "Core/Math/Scalar.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    namespace
    {
        class FTriggerRepeatOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerRepeatOperator(const FAudioGraphOperatorBuildParams& Params)
                : StartIn(Params.TriggerIn(0))
                , StopIn(Params.TriggerIn(1))
                , PeriodIn(Params.FloatIn(2))
                , Out(Params.TriggerOut(0))
            {}

            void Reset() override
            {
                Countdown = 0.0f;
                bRunning  = false;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                StartCursor.Reset();
                StopCursor.Reset();

                const float PeriodFrames = Math::Max(*PeriodIn, 0.001f) * (float)Context.SampleRate;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (StartCursor.Consume(*StartIn, Frame))
                    {
                        bRunning  = true;
                        Countdown = 0.0f;
                    }

                    if (StopCursor.Consume(*StopIn, Frame))
                    {
                        bRunning = false;
                    }

                    if (!bRunning)
                    {
                        continue;
                    }

                    if (Countdown <= 0.0f)
                    {
                        Out->Add(Frame);
                        Countdown += PeriodFrames;
                    }

                    Countdown -= 1.0f;
                }
            }

        private:

            const FAudioGraphTriggerBuffer* StartIn;
            const FAudioGraphTriggerBuffer* StopIn;
            const float*                    PeriodIn;
            FAudioGraphTriggerBuffer*       Out;

            FAudioGraphTriggerCursor StartCursor;
            FAudioGraphTriggerCursor StopCursor;

            float Countdown = 0.0f;
            bool  bRunning  = false;
        };

        class FTriggerDelayOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerDelayOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.TriggerIn(0))
                , DelayIn(Params.FloatIn(1))
                , Out(Params.TriggerOut(0))
            {}

            void Reset() override { PendingCount = 0; }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Cursor.Reset();

                const float DelayFrames = Math::Max(*DelayIn, 0.0f) * (float)Context.SampleRate;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*In, Frame) && PendingCount < MaxPending)
                    {
                        Pending[PendingCount++] = DelayFrames;
                    }

                    uint32 Write = 0;
                    for (uint32 Index = 0; Index < PendingCount; ++Index)
                    {
                        if (Pending[Index] <= 0.0f)
                        {
                            Out->Add(Frame);
                            continue;
                        }

                        Pending[Write++] = Pending[Index] - 1.0f;
                    }
                    PendingCount = Write;
                }
            }

        private:

            static constexpr uint32 MaxPending = 16;

            const FAudioGraphTriggerBuffer* In;
            const float*                    DelayIn;
            FAudioGraphTriggerBuffer*       Out;

            FAudioGraphTriggerCursor Cursor;

            float  Pending[MaxPending] = {};
            uint32 PendingCount = 0;
        };

        class FTriggerOnceOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerOnceOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.TriggerIn(0))
                , ResetIn(Params.TriggerIn(1))
                , Out(Params.TriggerOut(0))
            {}

            void Reset() override { bFired = false; }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                InCursor.Reset();
                ResetCursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (ResetCursor.Consume(*ResetIn, Frame))
                    {
                        bFired = false;
                    }

                    if (InCursor.Consume(*In, Frame) && !bFired)
                    {
                        bFired = true;
                        Out->Add(Frame);
                    }
                }
            }

        private:

            const FAudioGraphTriggerBuffer* In;
            const FAudioGraphTriggerBuffer* ResetIn;
            FAudioGraphTriggerBuffer*       Out;

            FAudioGraphTriggerCursor InCursor;
            FAudioGraphTriggerCursor ResetCursor;

            bool bFired = false;
        };

        class FTriggerAnyOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerAnyOperator(const FAudioGraphOperatorBuildParams& Params)
                : Out(Params.TriggerOut(0))
            {
                for (uint32 Index = 0; Index < NumInputs; ++Index)
                {
                    In[Index] = Params.TriggerIn(Index);
                }
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                for (uint32 Index = 0; Index < NumInputs; ++Index)
                {
                    Cursors[Index].Reset();
                }

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    bool bFired = false;
                    for (uint32 Index = 0; Index < NumInputs; ++Index)
                    {
                        bFired |= Cursors[Index].Consume(*In[Index], Frame);
                    }

                    if (bFired)
                    {
                        Out->Add(Frame);
                    }
                }
            }

        private:

            static constexpr uint32 NumInputs = 4;

            const FAudioGraphTriggerBuffer* In[NumInputs] = {};
            FAudioGraphTriggerBuffer*       Out;
            FAudioGraphTriggerCursor        Cursors[NumInputs];
        };

        class FTriggerCounterOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerCounterOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.TriggerIn(0))
                , ResetIn(Params.TriggerIn(1))
                , ResetCountIn(Params.IntIn(2))
                , CountOut(Params.FloatOut(0))
                , WrappedOut(Params.TriggerOut(1))
            {}

            void Reset() override { Count = 0; }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                InCursor.Reset();
                ResetCursor.Reset();

                const int32 ResetCount = Math::Max(*ResetCountIn, 0);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (ResetCursor.Consume(*ResetIn, Frame))
                    {
                        Count = 0;
                    }

                    if (InCursor.Consume(*In, Frame))
                    {
                        ++Count;

                        if (ResetCount > 0 && Count >= ResetCount)
                        {
                            Count = 0;
                            WrappedOut->Add(Frame);
                        }
                    }
                }

                *CountOut = (float)Count;
            }

        private:

            const FAudioGraphTriggerBuffer* In;
            const FAudioGraphTriggerBuffer* ResetIn;
            const int32*                    ResetCountIn;
            float*                          CountOut;
            FAudioGraphTriggerBuffer*       WrappedOut;

            FAudioGraphTriggerCursor InCursor;
            FAudioGraphTriggerCursor ResetCursor;

            int32 Count = 0;
        };

        class FRandomFloatOperator final : public IAudioGraphOperator
        {
        public:

            explicit FRandomFloatOperator(const FAudioGraphOperatorBuildParams& Params)
                : TriggerIn(Params.TriggerIn(0))
                , MinIn(Params.FloatIn(1))
                , MaxIn(Params.FloatIn(2))
                , SeedIn(Params.IntIn(3))
                , Out(Params.FloatOut(0))
                , NextOut(Params.TriggerOut(1))
            {}

            void Reset() override
            {
                State  = 0;
                bSeeded = false;
                Value  = 0.0f;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                if (!bSeeded)
                {
                    bSeeded = true;
                    State   = (uint32)*SeedIn * 2654435761u + 1u;
                }

                Cursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*TriggerIn, Frame))
                    {
                        Value = *MinIn + (*MaxIn - *MinIn) * NextUnit();
                        NextOut->Add(Frame);
                    }
                }

                *Out = Value;
            }

        private:

            FORCEINLINE float NextUnit()
            {
                State ^= State << 13;
                State ^= State >> 17;
                State ^= State << 5;
                return (float)(State >> 8) * (1.0f / 16777216.0f);
            }

            const FAudioGraphTriggerBuffer* TriggerIn;
            const float*                    MinIn;
            const float*                    MaxIn;
            const int32*                    SeedIn;
            float*                          Out;
            FAudioGraphTriggerBuffer*       NextOut;

            FAudioGraphTriggerCursor Cursor;

            uint32 State   = 0;
            float  Value   = 0.0f;
            bool   bSeeded = false;
        };

        class FTriggerOnThresholdOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerOnThresholdOperator(const FAudioGraphOperatorBuildParams& Params)
                : ValueIn(Params.FloatIn(0))
                , ThresholdIn(Params.FloatIn(1))
                , RisingOut(Params.TriggerOut(0))
                , FallingOut(Params.TriggerOut(1))
            {}

            void Reset() override
            {
                bAbove  = false;
                bPrimed = false;
            }

            void Execute(const FAudioGraphBlockContext&) override
            {
                const bool bNowAbove = *ValueIn >= *ThresholdIn;

                if (!bPrimed)
                {
                    bPrimed = true;
                    bAbove  = bNowAbove;
                    return;
                }

                if (bNowAbove != bAbove)
                {
                    bAbove = bNowAbove;
                    (bNowAbove ? RisingOut : FallingOut)->Add(0);
                }
            }

        private:

            const float*              ValueIn;
            const float*              ThresholdIn;
            FAudioGraphTriggerBuffer* RisingOut;
            FAudioGraphTriggerBuffer* FallingOut;

            bool bAbove  = false;
            bool bPrimed = false;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerRepeat, FTriggerRepeatOperator, "TriggerRepeat", { PinTrigger, PinTrigger, PinFloat }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerDelay, FTriggerDelayOperator, "TriggerDelay", { PinTrigger, PinFloat }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerOnce, FTriggerOnceOperator, "TriggerOnce", { PinTrigger, PinTrigger }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerAny, FTriggerAnyOperator, "TriggerAny", { PinTrigger, PinTrigger, PinTrigger, PinTrigger }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerCounter, FTriggerCounterOperator, "TriggerCounter", { PinTrigger, PinTrigger, PinInt32 }, { PinFloat, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeRandomFloat, FRandomFloatOperator, "Random.Float", { PinTrigger, PinFloat, PinFloat, PinInt32 }, { PinFloat, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerOnThreshold, FTriggerOnThresholdOperator, "TriggerOnThreshold", { PinFloat, PinFloat }, { PinTrigger, PinTrigger })
}
