#include "RuntimePCH.h"
#include "BenchmarkRun.h"

#include "Containers/Algorithm.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "Core/Application/Application.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineURL.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "World/WorldManager.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

namespace Lumina::Benchmark
{
    namespace
    {
        struct FState
        {
            bool     bActive       = false;
            bool     bStarted      = false;
            bool     bFinished     = false;
            bool     bVerified     = false;
            int32    FrameRateCap  = 0;

            FString  Map;
            FString  OutputPath;

            int32    WarmupFrames  = 60;
            int32    MeasureFrames = 600;

            int32    FramesSeen    = 0;
            TVector<double> FrameMilliseconds;
        };

        FState GState;

        double Percentile(const TVector<double>& Sorted, double Fraction)
        {
            if (Sorted.empty())
            {
                return 0.0;
            }

            const SIZE_T Index = (SIZE_T)(Fraction * (double)(Sorted.size() - 1));
            return Sorted[Index];
        }

        // Compared against the context's own MapPath, since Travel reports a failure and carries on.
        bool IsRequestedMapLoaded()
        {
            if (GWorldManager == nullptr)
            {
                return false;
            }

            for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
            {
                if (Context && Context->MapPath == GState.Map)
                {
                    return true;
                }
            }

            return false;
        }

        FString ResolveDefaultOutputPath(const FString& Map)
        {
            FString Name = Map;

            // A map path is a virtual one, and its separators cannot go into a file name.
            for (char& Character : Name)
            {
                if (Character == '/' || Character == '\\' || Character == ':')
                {
                    Character = '_';
                }
            }

            if (Name.empty())
            {
                Name = "CurrentWorld";
            }

            FString Directory = Paths::GetEngineDirectory() + "/Saved/Benchmarks";
            Paths::CreateDirectories(FStringView(Directory.c_str(), Directory.size()));

            return Directory + "/" + Name + ".csv";
        }
    }

    void ParseCommandLine(const FCommandLine& CommandLine)
    {
        GState = FState{};

        if (!CommandLine.Has("benchmark"))
        {
            return;
        }

        GState.bActive = true;

        if (TOptional<FFixedString> MapValue = CommandLine.Get("benchmark"))
        {
            GState.Map.assign(MapValue.value().c_str(), MapValue.value().size());
        }

        if (TOptional<int> Warmup = CommandLine.GetInt("warmup"))
        {
            GState.WarmupFrames = Math::Max(0, Warmup.value());
        }

        if (TOptional<int> Frames = CommandLine.GetInt("frames"))
        {
            GState.MeasureFrames = Math::Max(1, Frames.value());
        }

        // A capped run measures the cap, so any change to the frame's cost is invisible behind it.
        if (TOptional<int> Cap = CommandLine.GetInt("benchmarkfps"))
        {
            GState.FrameRateCap = Math::Max(0, Cap.value());
        }

        if (TOptional<FFixedString> Output = CommandLine.Get("benchmarkout"))
        {
            GState.OutputPath.assign(Output.value().c_str(), Output.value().size());
        }

        GState.FrameMilliseconds.reserve((SIZE_T)GState.MeasureFrames);

        LOG_DISPLAY("Benchmark: {} warmup frames then {} measured{}.",
            GState.WarmupFrames,
            GState.MeasureFrames,
            GState.Map.empty() ? FString(", on the world already loaded") : FString(", on ") + GState.Map);
    }

    bool IsActive()
    {
        return GState.bActive;
    }

    FStringView GetRequestedMap()
    {
        return FStringView(GState.Map.c_str(), GState.Map.size());
    }

    void Start()
    {
        if (!GState.bActive || GState.bStarted)
        {
            return;
        }

        GState.bStarted = true;

        const FString Cap = Format("{}", GState.FrameRateCap);
        if (FConsoleRegistry::Get().SetValueFromString(FStringView("Core.MaxFPS"),
                FStringView(Cap.c_str(), Cap.size())))
        {
            LOG_DISPLAY("Benchmark: Core.MaxFPS set to {}{}", GState.FrameRateCap,
                GState.FrameRateCap == 0 ? FString(" (uncapped)") : FString());
        }
        else
        {
            LOG_WARN("Benchmark: could not clear Core.MaxFPS, so the run measures the frame rate cap.");
        }

        if (GState.Map.empty() || GEngine == nullptr)
        {
            return;
        }

        LOG_DISPLAY("Benchmark: opening {}", GState.Map);
        GEngine->OpenLevel(FURL::Parse(FStringView(GState.Map.c_str(), GState.Map.size())));
    }

    bool Tick(double DeltaSeconds)
    {
        if (!GState.bActive || GState.bFinished)
        {
            return !GState.bActive;
        }

        ++GState.FramesSeen;

        // The warmup covers the level load, the first shader compiles and the caches that fill behind them.
        if (GState.FramesSeen <= GState.WarmupFrames)
        {
            return true;
        }

        // A map that failed to load leaves the startup world running, and timing that reports a number.
        if (!GState.bVerified)
        {
            GState.bVerified = true;

            if (!GState.Map.empty() && !IsRequestedMapLoaded())
            {
                LOG_ERROR("Benchmark: '{}' is not the world that loaded, so there is nothing to measure. "
                          "Check the path against the project's Content, and note that a shell may rewrite "
                          "a leading slash.", GState.Map);

                GState.bFinished = true;
                return false;
            }

            // A profiler connects on this line, so the capture lands in the measured window and not the load.
            LOG_DISPLAY("Benchmark: warmup complete, measuring {} frames.", GState.MeasureFrames);
        }

        GState.FrameMilliseconds.push_back(DeltaSeconds * 1000.0);

        if ((int32)GState.FrameMilliseconds.size() < GState.MeasureFrames)
        {
            return true;
        }

        Finish();
        return false;
    }

    void Finish()
    {
        if (!GState.bActive || GState.bFinished)
        {
            return;
        }

        GState.bFinished = true;

        if (GState.FrameMilliseconds.empty())
        {
            LOG_WARN("Benchmark: the run ended before any frame was measured.");
            return;
        }

        TVector<double> Sorted = GState.FrameMilliseconds;
        Algo::Sort(Sorted);

        double Total = 0.0;
        for (double Frame : Sorted)
        {
            Total += Frame;
        }

        const double Mean = Total / (double)Sorted.size();
        const double Median = Percentile(Sorted, 0.50);

        FString Csv = "frame,milliseconds\n";
        for (SIZE_T Index = 0; Index < GState.FrameMilliseconds.size(); ++Index)
        {
            Csv += Format("{},{:.4f}\n", (uint64)Index, GState.FrameMilliseconds[Index]);
        }

        const FString Path = GState.OutputPath.empty()
            ? ResolveDefaultOutputPath(GState.Map)
            : GState.OutputPath;

        const bool bWritten = Filesystem::WriteFile(
            FStringView(Path.c_str(), Path.size()),
            TSpan<const uint8>(reinterpret_cast<const uint8*>(Csv.data()), Csv.size()));

        LOG_DISPLAY("Benchmark: {} frames, mean {:.3f} ms ({:.1f} fps), median {:.3f}, "
                    "p95 {:.3f}, p99 {:.3f}, min {:.3f}, max {:.3f}",
            (uint64)Sorted.size(), Mean, Mean > 0.0 ? 1000.0 / Mean : 0.0, Median,
            Percentile(Sorted, 0.95), Percentile(Sorted, 0.99), Sorted.front(), Sorted.back());

        if (bWritten)
        {
            LOG_DISPLAY("Benchmark: per-frame times written to {}", Path);
        }
        else
        {
            LOG_ERROR("Benchmark: could not write {}", Path);
        }
    }
}
