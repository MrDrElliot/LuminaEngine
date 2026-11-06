#include "FrameStats.h"

#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"

#include <algorithm>

namespace Lumina::FrameStats
{
    namespace
    {
        struct FSample
        {
            double TimeSeconds;
            float  FrameMs;
        };

        // A quarter million frames is over twenty minutes at 165 Hz, and beyond it the oldest half is dropped.
        constexpr int32 MaxSamples = 1 << 18;

        FMutex          Mutex;
        TVector<FSample> Samples;
        double          LastFrameSeconds = 0.0;
        double          WindowStartSeconds = 0.0;
    }

    void MarkFrame(double NowSeconds)
    {
        FScopeLock Lock(Mutex);
        if (LastFrameSeconds > 0.0)
        {
            if (Samples.Num() >= MaxSamples)
            {
                Samples.RemoveAt(0, MaxSamples / 2);
            }
            Samples.Emplace(NowSeconds, (float)((NowSeconds - LastFrameSeconds) * 1000.0));
        }
        else
        {
            WindowStartSeconds = NowSeconds;
        }
        LastFrameSeconds = NowSeconds;
    }

    void Reset()
    {
        FScopeLock Lock(Mutex);
        Samples.Reset();
        WindowStartSeconds = LastFrameSeconds;
    }

    FSummary Summarize(int32 WorstCount, double SinceSeconds)
    {
        TVector<FSample> Copy;
        FSummary Summary;
        {
            FScopeLock Lock(Mutex);
            Copy = Samples;
            Summary.WindowSeconds = LastFrameSeconds - Math::Max(WindowStartSeconds, SinceSeconds);
        }

        if (SinceSeconds > 0.0)
        {
            Copy.RemoveAll([SinceSeconds](const FSample& Sample) { return Sample.TimeSeconds < SinceSeconds; });
        }

        Summary.Frames = Copy.Num();
        if (Copy.IsEmpty())
        {
            return Summary;
        }

        TVector<float> Sorted;
        Sorted.Reserve(Copy.Num());
        double Total = 0.0;
        for (const FSample& Sample : Copy)
        {
            Sorted.Add(Sample.FrameMs);
            Total += Sample.FrameMs;
            Summary.Over33Ms  += Sample.FrameMs > 33.3f ? 1u : 0u;
            Summary.Over50Ms  += Sample.FrameMs > 50.0f ? 1u : 0u;
            Summary.Over100Ms += Sample.FrameMs > 100.0f ? 1u : 0u;
            Summary.Over250Ms += Sample.FrameMs > 250.0f ? 1u : 0u;
        }
        Algo::Sort(Sorted, [](float A, float B) { return A < B; });

        auto Percentile = [&Sorted](float Fraction)
        {
            const int32 Index = Math::Min(Sorted.Num() - 1, static_cast<int32>(Fraction * static_cast<float>(Sorted.Num() - 1) + 0.5f));
            return Sorted[Index];
        };
        Summary.MeanMs = (float)(Total / (double)Copy.Num());
        Summary.P50Ms = Percentile(0.50f);
        Summary.P95Ms = Percentile(0.95f);
        Summary.P99Ms = Percentile(0.99f);
        Summary.MaxMs = Sorted.Last();

        TVector<int32> Order(Copy.Num());
        for (int32 Index = 0; Index < Copy.Num(); ++Index)
        {
            Order[Index] = Index;
        }
        Algo::Sort(Order, [&Copy](int32 A, int32 B) { return Copy[A].FrameMs > Copy[B].FrameMs; });
        const int32 Count = Math::Min(WorstCount, Order.Num());
        for (int32 Rank = 0; Rank < Count; ++Rank)
        {
            const FSample& Sample = Copy[Order[Rank]];
            Summary.Worst.Emplace(Sample.TimeSeconds, Sample.FrameMs);
        }
        return Summary;
    }
}
