#include "RuntimePCH.h"
#include "BenchmarkRun.h"

#include "Containers/Algorithm.h"
#include "Renderer/MeshData.h"
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
#include "Renderer/RHI.h"
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
            if (Sorted.IsEmpty())
            {
                return 0.0;
            }

            const int32 Index = static_cast<int32>(Fraction * (Sorted.Num() - 1));
            return Sorted[Index];
        }

        // Compared against the context's own MapPath, since Travel reports a failure and carries on.
        bool IsRequestedMapLoaded()
        {
            if (Global::GWorldManager == nullptr)
            {
                return false;
            }

            for (const TUniquePtr<FWorldContext>& Context : Global::GWorldManager->GetContexts())
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

            if (Name.IsEmpty())
            {
                Name = "CurrentWorld";
            }

            FString Directory = Paths::GetEngineDirectory() + "/Saved/Benchmarks";
            Paths::CreateDirectories(FStringView(*Directory, Directory.Len()));

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
            GState.Map = FStringView(*MapValue.GetValue(), MapValue.GetValue().Len());
        }

        if (TOptional<int> Warmup = CommandLine.GetInt("warmup"))
        {
            GState.WarmupFrames = Math::Max(0, Warmup.GetValue());
        }

        if (TOptional<int> Frames = CommandLine.GetInt("frames"))
        {
            GState.MeasureFrames = Math::Max(1, Frames.GetValue());
        }

        // A capped run measures the cap, so any change to the frame's cost is invisible behind it.
        if (TOptional<int> Cap = CommandLine.GetInt("benchmarkfps"))
        {
            GState.FrameRateCap = Math::Max(0, Cap.GetValue());
        }

        if (TOptional<FFixedString> Output = CommandLine.Get("benchmarkout"))
        {
            GState.OutputPath = FStringView(*Output.GetValue(), Output.GetValue().Len());
        }

        GState.FrameMilliseconds.Reserve((SIZE_T)GState.MeasureFrames);

        LOG_DISPLAY("Benchmark: {} warmup frames then {} measured{}.",
            GState.WarmupFrames,
            GState.MeasureFrames,
            GState.Map.IsEmpty() ? FString(", on the world already loaded") : FString(", on ") + GState.Map);
    }

    bool IsActive()
    {
        return GState.bActive;
    }

    FStringView GetRequestedMap()
    {
        return FStringView(*GState.Map, GState.Map.Len());
    }

    void Start()
    {
        if (!GState.bActive || GState.bStarted)
        {
            return;
        }

        GState.bStarted = true;

        if (Global::GEngine != nullptr)
        {
            Global::GEngine->SetFrameRateCapOverride(GState.FrameRateCap);
            LOG_DISPLAY("Benchmark: frame rate cap set to {}{}", GState.FrameRateCap,
                GState.FrameRateCap == 0 ? FString(" (uncapped)") : FString());
        }
        else
        {
            LOG_WARN("Benchmark: no engine to set the frame rate cap on, so the run measures the saved cap.");
        }

        if (GState.Map.IsEmpty() || Global::GEngine == nullptr)
        {
            return;
        }

        LOG_DISPLAY("Benchmark: opening {}", GState.Map);
        Global::GEngine->OpenLevel(FURL::Parse(FStringView(*GState.Map, GState.Map.Len())));
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

            if (!GState.Map.IsEmpty() && !IsRequestedMapLoaded())
            {
                LOG_ERROR("Benchmark: '{}' is not the world that loaded, so there is nothing to measure. "
                          "Check the path against the project's Content, and note that a shell may rewrite "
                          "a leading slash.", GState.Map);

                GState.bFinished = true;
                return false;
            }

            // A profiler connects on this line, so the capture lands in the measured window and not the load.
            LOG_DISPLAY("Benchmark: warmup complete, measuring {} frames.", GState.MeasureFrames);
            RHI::LogGPUPoolStats("after warmup");
        }

        GState.FrameMilliseconds.Add(DeltaSeconds * 1000.0);

        if (GState.FrameMilliseconds.Num() < GState.MeasureFrames)
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

        if (GState.FrameMilliseconds.IsEmpty())
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

        const double Mean = Total / (double)Sorted.Num();
        const double Median = Percentile(Sorted, 0.50);

        FString Csv = "frame,milliseconds\n";
        for (int32 Index = 0; Index < GState.FrameMilliseconds.Num(); ++Index)
        {
            Csv += Format("{},{:.4f}\n", (uint64)Index, GState.FrameMilliseconds[Index]);
        }

        const FString Path = GState.OutputPath.IsEmpty()
            ? ResolveDefaultOutputPath(GState.Map)
            : GState.OutputPath;

        const bool bWritten = Filesystem::WriteFile(
            FStringView(*Path, Path.Len()),
            TSpan<const uint8>(reinterpret_cast<const uint8*>(Csv.GetData()), Csv.Len()));

        RHI::LogGPUPoolStats("end of run");
        MeshGeometryStats::Log("end of run");

        LOG_DISPLAY("Benchmark: {} frames, mean {:.3f} ms ({:.1f} fps), median {:.3f}, "
                    "p95 {:.3f}, p99 {:.3f}, min {:.3f}, max {:.3f}",
            (uint64)Sorted.Num(), Mean, Mean > 0.0 ? 1000.0 / Mean : 0.0, Median,
            Percentile(Sorted, 0.95), Percentile(Sorted, 0.99), Sorted[0], Sorted.Last());

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
