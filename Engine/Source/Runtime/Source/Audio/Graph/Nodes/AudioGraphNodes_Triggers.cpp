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
                Random = FAudioGraphRandom();
                Value  = 0.0f;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Random.SeedOnce(*SeedIn);

                Cursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*TriggerIn, Frame))
                    {
                        Value = *MinIn + (*MaxIn - *MinIn) * Random.NextUnit();
                        NextOut->Add(Frame);
                    }
                }

                *Out = Value;
            }

        private:

            const FAudioGraphTriggerBuffer* TriggerIn;
            const float*                    MinIn;
            const float*                    MaxIn;
            const int32*                    SeedIn;
            float*                          Out;
            FAudioGraphTriggerBuffer*       NextOut;

            FAudioGraphTriggerCursor Cursor;
            FAudioGraphRandom        Random;

            float Value = 0.0f;
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

        class FTriggerSelectOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerSelectOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.TriggerIn(0))
                , ResetIn(Params.TriggerIn(1))
                , ModeIn(Params.IntIn(2))
                , IndexIn(Params.IntIn(3))
                , CountIn(Params.IntIn(4))
                , SeedIn(Params.IntIn(5))
                , SelectedOut(Params.IntOut(NumOutputs))
            {
                for (uint32 Index = 0; Index < NumOutputs; ++Index)
                {
                    Outs[Index] = Params.TriggerOut(Index);
                }
            }

            void Reset() override
            {
                Random  = FAudioGraphRandom();
                Current = kNoneSelected;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Random.SeedOnce(*SeedIn);
                InCursor.Reset();
                ResetCursor.Reset();

                const EAudioPickMode Mode = (EAudioPickMode)*ModeIn;
                const uint32 Count = *CountIn > 0 ? Math::Min((uint32)*CountIn, NumOutputs) : NumOutputs;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (ResetCursor.Consume(*ResetIn, Frame))
                    {
                        Current = kNoneSelected;
                    }

                    if (InCursor.Consume(*In, Frame))
                    {
                        // A fresh sequence starts on the first output.
                        const uint32 Previous = Current < Count ? Current : Count - 1;
                        Current = PickNextOption(Mode, Count, Previous, *IndexIn, Random);
                        Outs[Current]->Add(Frame);
                    }
                }

                *SelectedOut = Current < Count ? (int32)Current : -1;
            }

        private:

            static constexpr uint32 NumOutputs    = 4;
            static constexpr uint32 kNoneSelected = ~0u;

            const FAudioGraphTriggerBuffer* In;
            const FAudioGraphTriggerBuffer* ResetIn;
            const int32*                    ModeIn;
            const int32*                    IndexIn;
            const int32*                    CountIn;
            const int32*                    SeedIn;
            FAudioGraphTriggerBuffer*       Outs[NumOutputs] = {};
            int32*                          SelectedOut;

            FAudioGraphTriggerCursor InCursor;
            FAudioGraphTriggerCursor ResetCursor;
            FAudioGraphRandom        Random;
            uint32                   Current = kNoneSelected;
        };

        class FTriggerProbabilityOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerProbabilityOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.TriggerIn(0))
                , ProbabilityIn(Params.FloatIn(1))
                , SeedIn(Params.IntIn(2))
                , PassOut(Params.TriggerOut(0))
                , FailOut(Params.TriggerOut(1))
            {}

            void Reset() override { Random = FAudioGraphRandom(); }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Random.SeedOnce(*SeedIn);
                Cursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*In, Frame))
                    {
                        (Random.NextUnit() < *ProbabilityIn ? PassOut : FailOut)->Add(Frame);
                    }
                }
            }

        private:

            const FAudioGraphTriggerBuffer* In;
            const float*                    ProbabilityIn;
            const int32*                    SeedIn;
            FAudioGraphTriggerBuffer*       PassOut;
            FAudioGraphTriggerBuffer*       FailOut;

            FAudioGraphTriggerCursor Cursor;
            FAudioGraphRandom        Random;
        };

        class FTriggerGateOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerGateOperator(const FAudioGraphOperatorBuildParams& Params)
                : In(Params.TriggerIn(0))
                , OpenIn(Params.TriggerIn(1))
                , CloseIn(Params.TriggerIn(2))
                , StartOpenIn(Params.BoolIn(3))
                , Out(Params.TriggerOut(0))
            {}

            void Reset() override { bPrimed = false; }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                if (!bPrimed)
                {
                    bPrimed = true;
                    bOpen   = *StartOpenIn != 0;
                }

                InCursor.Reset();
                OpenCursor.Reset();
                CloseCursor.Reset();

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    bOpen = OpenCursor.Consume(*OpenIn, Frame) ? true : bOpen;
                    bOpen = CloseCursor.Consume(*CloseIn, Frame) ? false : bOpen;

                    if (InCursor.Consume(*In, Frame) && bOpen)
                    {
                        Out->Add(Frame);
                    }
                }
            }

        private:

            const FAudioGraphTriggerBuffer* In;
            const FAudioGraphTriggerBuffer* OpenIn;
            const FAudioGraphTriggerBuffer* CloseIn;
            const uint8*                    StartOpenIn;
            FAudioGraphTriggerBuffer*       Out;

            FAudioGraphTriggerCursor InCursor;
            FAudioGraphTriggerCursor OpenCursor;
            FAudioGraphTriggerCursor CloseCursor;

            bool bOpen   = false;
            bool bPrimed = false;
        };

        class FTriggerOnBoolOperator final : public IAudioGraphOperator
        {
        public:

            explicit FTriggerOnBoolOperator(const FAudioGraphOperatorBuildParams& Params)
                : ValueIn(Params.BoolIn(0))
                , TrueOut(Params.TriggerOut(0))
                , FalseOut(Params.TriggerOut(1))
            {}

            void Reset() override { bPrimed = false; }

            void Execute(const FAudioGraphBlockContext&) override
            {
                const bool bNow = *ValueIn != 0;

                // The starting value is a state, not a change, so the first block only records it.
                if (bPrimed && bNow != bLast)
                {
                    (bNow ? TrueOut : FalseOut)->Add(0);
                }

                bPrimed = true;
                bLast   = bNow;
            }

        private:

            const uint8*              ValueIn;
            FAudioGraphTriggerBuffer* TrueOut;
            FAudioGraphTriggerBuffer* FalseOut;

            bool bLast   = false;
            bool bPrimed = false;
        };

        class FRandomIntOperator final : public IAudioGraphOperator
        {
        public:

            explicit FRandomIntOperator(const FAudioGraphOperatorBuildParams& Params)
                : NextIn(Params.TriggerIn(0))
                , MinIn(Params.IntIn(1))
                , MaxIn(Params.IntIn(2))
                , SeedIn(Params.IntIn(3))
                , NoRepeatIn(Params.BoolIn(4))
                , Out(Params.IntOut(0))
                , NextOut(Params.TriggerOut(1))
            {}

            void Reset() override
            {
                Random = FAudioGraphRandom();
                Offset = 0;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                Random.SeedOnce(*SeedIn);
                Cursor.Reset();

                const int32  Low   = Math::Min(*MinIn, *MaxIn);
                const uint32 Count = (uint32)(Math::Max(*MinIn, *MaxIn) - Low) + 1u;
                const EAudioPickMode Mode = *NoRepeatIn != 0 ? EAudioPickMode::RandomNoRepeat : EAudioPickMode::Random;

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (Cursor.Consume(*NextIn, Frame))
                    {
                        Offset = PickNextOption(Mode, Count, Offset, 0, Random);
                        NextOut->Add(Frame);
                    }
                }

                *Out = Low + (int32)Offset;
            }

        private:

            const FAudioGraphTriggerBuffer* NextIn;
            const int32*                    MinIn;
            const int32*                    MaxIn;
            const int32*                    SeedIn;
            const uint8*                    NoRepeatIn;
            int32*                          Out;
            FAudioGraphTriggerBuffer*       NextOut;

            FAudioGraphTriggerCursor Cursor;
            FAudioGraphRandom        Random;
            uint32                   Offset = 0;
        };

        // Musical timing for a graph, with every tick sample accurate however the BPM moves.
        class FBeatClockOperator final : public IAudioGraphOperator
        {
        public:

            explicit FBeatClockOperator(const FAudioGraphOperatorBuildParams& Params)
                : StartIn(Params.TriggerIn(0))
                , StopIn(Params.TriggerIn(1))
                , TempoIn(Params.FloatIn(2))
                , BeatsPerBarIn(Params.IntIn(3))
                , SubdivisionIn(Params.IntIn(4))
                , TickOut(Params.TriggerOut(0))
                , BeatOut(Params.TriggerOut(1))
                , BarOut(Params.TriggerOut(2))
                , BeatIndexOut(Params.IntOut(3))
            {}

            void Reset() override
            {
                bRunning  = false;
                Countdown = 0.0f;
                Tick      = 0;
            }

            void Execute(const FAudioGraphBlockContext& Context) override
            {
                StartCursor.Reset();
                StopCursor.Reset();

                const uint32 Subdivision = (uint32)Math::Max(*SubdivisionIn, 1);
                const uint32 BeatsPerBar = (uint32)Math::Max(*BeatsPerBarIn, 1);
                const float  TickFrames  = 60.0f * (float)Context.SampleRate / (Math::Max(*TempoIn, 1.0f) * (float)Subdivision);

                for (uint32 Frame = 0; Frame < Context.NumFrames; ++Frame)
                {
                    if (StartCursor.Consume(*StartIn, Frame))
                    {
                        bRunning  = true;
                        Countdown = 0.0f;
                        Tick      = 0;
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
                        TickOut->Add(Frame);
                        if (Tick % Subdivision == 0)
                        {
                            BeatOut->Add(Frame);
                            if ((Tick / Subdivision) % BeatsPerBar == 0)
                            {
                                BarOut->Add(Frame);
                            }
                        }
                        ++Tick;
                        Countdown += TickFrames;
                    }

                    Countdown -= 1.0f;
                }

                *BeatIndexOut = Tick != 0 ? (int32)(((Tick - 1) / Subdivision) % BeatsPerBar) : 0;
            }

        private:

            const FAudioGraphTriggerBuffer* StartIn;
            const FAudioGraphTriggerBuffer* StopIn;
            const float*                    TempoIn;
            const int32*                    BeatsPerBarIn;
            const int32*                    SubdivisionIn;
            FAudioGraphTriggerBuffer*       TickOut;
            FAudioGraphTriggerBuffer*       BeatOut;
            FAudioGraphTriggerBuffer*       BarOut;
            int32*                          BeatIndexOut;

            FAudioGraphTriggerCursor StartCursor;
            FAudioGraphTriggerCursor StopCursor;

            float  Countdown = 0.0f;
            uint64 Tick      = 0;
            bool   bRunning  = false;
        };
    }

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerRepeat, FTriggerRepeatOperator, "TriggerRepeat", { PinTrigger, PinTrigger, PinFloat }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerDelay, FTriggerDelayOperator, "TriggerDelay", { PinTrigger, PinFloat }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerOnce, FTriggerOnceOperator, "TriggerOnce", { PinTrigger, PinTrigger }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerAny, FTriggerAnyOperator, "TriggerAny", { PinTrigger, PinTrigger, PinTrigger, PinTrigger }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerCounter, FTriggerCounterOperator, "TriggerCounter", { PinTrigger, PinTrigger, PinInt32 }, { PinFloat, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeRandomFloat, FRandomFloatOperator, "Random.Float", { PinTrigger, PinFloat, PinFloat, PinInt32 }, { PinFloat, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerOnThreshold, FTriggerOnThresholdOperator, "TriggerOnThreshold", { PinFloat, PinFloat }, { PinTrigger, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerSelect, FTriggerSelectOperator, "TriggerSelect", { PinTrigger, PinTrigger, PinInt32, PinInt32, PinInt32, PinInt32 }, { PinTrigger, PinTrigger, PinTrigger, PinTrigger, PinInt32 })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerProbability, FTriggerProbabilityOperator, "TriggerProbability", { PinTrigger, PinFloat, PinInt32 }, { PinTrigger, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerGate, FTriggerGateOperator, "TriggerGate", { PinTrigger, PinTrigger, PinTrigger, PinBool }, { PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeTriggerOnBool, FTriggerOnBoolOperator, "TriggerOnBool", { PinBool }, { PinTrigger, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeRandomInt, FRandomIntOperator, "Random.Int", { PinTrigger, PinInt32, PinInt32, PinInt32, PinBool }, { PinInt32, PinTrigger })

    LUMINA_AUDIO_GRAPH_OPERATOR(GAudioNodeBeatClock, FBeatClockOperator, "BeatClock", { PinTrigger, PinTrigger, PinFloat, PinInt32, PinInt32 }, { PinTrigger, PinTrigger, PinTrigger, PinInt32 })
}
