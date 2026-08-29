#pragma once

#include "ModuleAPI.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FCommandLine;
}

// A deterministic timed run, measuring the frame the engine actually renders rather than a microbenchmark.
namespace Lumina::Benchmark
{
    /** Reads -benchmark and its options. Everything below is inert unless that switch was passed. */
    RUNTIME_API void ParseCommandLine(const FCommandLine& CommandLine);

    RUNTIME_API NODISCARD bool IsActive();

    /** The map the run asked for, empty when it profiles whatever the engine already loaded. */
    RUNTIME_API NODISCARD FStringView GetRequestedMap();

    /** Opens the requested map. Called once the engine is up and able to travel. */
    RUNTIME_API void Start();

    /** Records one frame. Returns false when the run is over, which is what ends the loop. */
    RUNTIME_API NODISCARD bool Tick(double DeltaSeconds);

    /** Writes the per-frame CSV and logs the summary. Safe to call when no run is active. */
    RUNTIME_API void Finish();
}
