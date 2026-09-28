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
        constexpr size_t MaxSamples = 1u << 18;

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
            if (Samples.size() >= MaxSamples)
            {
                Samples.erase(Samples.begin(), Samples.begin() + MaxSamples / 2);
            }
            Samples.push_back(FSample{ NowSeconds, (float)((NowSeconds - LastFrameSeconds) * 1000.0) });
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
        Samples.clear();
        WindowStartSeconds = LastFrameSeconds;
    }

    FSummary Summarize(uint32 WorstCount, double SinceSeconds)
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
            Copy.erase(std::remove_if(Copy.begin(), Copy.end(), [SinceSeconds](const FSample& Sample) { return Sample.TimeSeconds < SinceSeconds; }), Copy.end());
        }

        Summary.Frames = Copy.size();
        if (Copy.empty())
        {
            return Summary;
        }

        TVector<float> Sorted;
        Sorted.reserve(Copy.size());
        double Total = 0.0;
        for (const FSample& Sample : Copy)
        {
            Sorted.push_back(Sample.FrameMs);
            Total += Sample.FrameMs;
            Summary.Over33Ms  += Sample.FrameMs > 33.3f ? 1u : 0u;
            Summary.Over50Ms  += Sample.FrameMs > 50.0f ? 1u : 0u;
            Summary.Over100Ms += Sample.FrameMs > 100.0f ? 1u : 0u;
            Summary.Over250Ms += Sample.FrameMs > 250.0f ? 1u : 0u;
        }
        Algo::Sort(Sorted, [](float A, float B) { return A < B; });

        auto Percentile = [&Sorted](float Fraction)
        {
            const size_t Index = Math::Min(Sorted.size() - 1, (size_t)(Fraction * (float)(Sorted.size() - 1) + 0.5f));
            return Sorted[Index];
        };
        Summary.MeanMs = (float)(Total / (double)Copy.size());
        Summary.P50Ms = Percentile(0.50f);
        Summary.P95Ms = Percentile(0.95f);
        Summary.P99Ms = Percentile(0.99f);
        Summary.MaxMs = Sorted.back();

        TVector<size_t> Order(Copy.size());
        for (size_t Index = 0; Index < Copy.size(); ++Index)
        {
            Order[Index] = Index;
        }
        Algo::Sort(Order, [&Copy](size_t A, size_t B) { return Copy[A].FrameMs > Copy[B].FrameMs; });
        const size_t Count = Math::Min((size_t)WorstCount, Order.size());
        for (size_t Rank = 0; Rank < Count; ++Rank)
        {
            const FSample& Sample = Copy[Order[Rank]];
            Summary.Worst.push_back(FHitch{ Sample.TimeSeconds, Sample.FrameMs });
        }
        return Summary;
    }
}
