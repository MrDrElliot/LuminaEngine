#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPRuntimeTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SEngineStatsParams
    {
        GENERATED_BODY()

        // Starts a fresh frame window after reporting, so the next call covers only what happens in between.
        PROPERTY()
        bool bResetFrames = false;

        // How many of the longest frames to list.
        PROPERTY()
        int32 Worst = 10;
    };

    REFLECT()
    struct MCPEDITOR_API SEngineFrame
    {
        GENERATED_BODY()

        // Seconds on the engine clock, comparable across calls.
        PROPERTY()
        double AtSeconds = 0.0;

        PROPERTY()
        float FrameMs = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SEngineWorldStats
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        FString Type;

        PROPERTY()
        int32 Entities = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SEngineStats
    {
        GENERATED_BODY()

        // Frames recorded since the last reset, and over how long.
        PROPERTY()
        int32 Frames = 0;

        PROPERTY()
        float WindowSeconds = 0.0f;

        PROPERTY()
        float MeanMs = 0.0f;

        PROPERTY()
        float P50Ms = 0.0f;

        PROPERTY()
        float P95Ms = 0.0f;

        PROPERTY()
        float P99Ms = 0.0f;

        PROPERTY()
        float MaxMs = 0.0f;

        PROPERTY()
        int32 Over33Ms = 0;

        PROPERTY()
        int32 Over50Ms = 0;

        PROPERTY()
        int32 Over100Ms = 0;

        PROPERTY()
        int32 Over250Ms = 0;

        PROPERTY()
        TVector<SEngineFrame> Worst;

        PROPERTY()
        int32 ProcessMemoryMB = 0;

        PROPERTY()
        int32 GpuUsageMB = 0;

        PROPERTY()
        int32 GpuBudgetMB = 0;

        PROPERTY()
        int32 GpuAllocations = 0;

        // Bytes the engine allocator ledger holds, which excludes the managed heap and driver memory.
        PROPERTY()
        int32 TrackedNativeMB = 0;

        PROPERTY()
        int32 ManagedHeapMB = 0;

        PROPERTY()
        int32 LiveObjects = 0;

        // Managed listeners bound to native delegates, which should fall back after a world goes away.
        PROPERTY()
        int32 ManagedBindings = 0;

        PROPERTY()
        TVector<SEngineWorldStats> Worlds;
    };

    REFLECT()
    struct MCPEDITOR_API SMemoryReportParams
    {
        GENERATED_BODY()

        // On or Off switches per-allocation stack capture, which call sites need; empty leaves it as it is.
        PROPERTY()
        FString Callstacks;

        // Zeroes the category and call-site counters after reporting, so the next report shows growth since now.
        PROPERTY()
        bool bReset = false;

        PROPERTY()
        int32 Top = 12;

        // Only call sites in this category. Empty ranks every category together.
        PROPERTY()
        FString Category;

        // Forces a managed collection first, so garbage waiting on the collector does not read as a leak.
        PROPERTY()
        bool bCollectManaged = true;

        // Writes the allocator's per-thread, per-size-class statistics to this file when set.
        PROPERTY()
        FString StatsFile;

        // Walks every OS heap to size foreign DLL and driver allocations, holding a process-wide lock while it runs.
        PROPERTY()
        bool bIncludeHeaps = false;
    };

    REFLECT()
    struct MCPEDITOR_API SMemoryCategory
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        float LiveMB = 0.0f;

        PROPERTY()
        int32 LiveCount = 0;

        PROPERTY()
        float PeakMB = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SMemoryCallSite
    {
        GENERATED_BODY()

        PROPERTY()
        FString Category;

        PROPERTY()
        float LiveKB = 0.0f;

        PROPERTY()
        int32 LiveCount = 0;

        // Innermost frames first, with the allocator's own frames dropped.
        PROPERTY()
        TVector<FString> Frames;
    };

    REFLECT()
    struct MCPEDITOR_API SMemoryGpuHeap
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Index = 0;

        PROPERTY()
        bool bDeviceLocal = false;

        PROPERTY()
        bool bHostVisible = false;

        // What the OS charges this process on the heap, which includes the driver's own allocations.
        PROPERTY()
        float UsageMB = 0.0f;

        // What the engine's allocator holds there, so usage well above it is the driver's.
        PROPERTY()
        float BlockMB = 0.0f;

        PROPERTY()
        float BudgetMB = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SMemoryGpuCategory
    {
        GENERATED_BODY()

        // The debug name's segment before the first dot, the same grouping the Memory tool uses.
        PROPERTY()
        FString Name;

        // CPUWrite, CPURead or GPUOnly for buffers, Texture for images.
        PROPERTY()
        FString Memory;

        PROPERTY()
        float MB = 0.0f;

        PROPERTY()
        int32 Count = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SMemoryReport
    {
        GENERATED_BODY()

        PROPERTY()
        int32 ProcessMB = 0;

        PROPERTY()
        float TrackedNativeMB = 0.0f;

        PROPERTY()
        int32 TrackedAllocations = 0;

        // Allocations the ledger could not record because a table was full, so nonzero means tracked undercounts.
        PROPERTY()
        int32 TrackingOverflows = 0;

        PROPERTY()
        float ManagedHeapMB = 0.0f;

        PROPERTY()
        float ManagedCommittedMB = 0.0f;

        // Private commit split by owner, where whatever neither allocator explains lands in Unattributed.
        PROPERTY()
        float PrivateCommittedMB = 0.0f;

        PROPERTY()
        float RpmallocMappedMB = 0.0f;

        PROPERTY()
        float RpmallocCachedMB = 0.0f;

        PROPERTY()
        float RpmallocCommittedMB = 0.0f;

        PROPERTY()
        bool bHugePages = false;

        PROPERTY()
        float HeapCommittedMB = 0.0f;

        PROPERTY()
        float UnattributedMB = 0.0f;

        PROPERTY()
        float ReservedMB = 0.0f;

        // Collectible script load contexts still resident, where more than one after a reload is a leak.
        PROPERTY()
        int32 ScriptContexts = 0;

        PROPERTY()
        bool bCallstacks = false;

        PROPERTY()
        TVector<SMemoryCategory> Categories;

        PROPERTY()
        TVector<SMemoryCallSite> CallSites;

        // Host-visible GPU allocations are mapped into the process, so they count toward its private commit.
        PROPERTY()
        float GpuHostVisibleMB = 0.0f;

        PROPERTY()
        float GpuDeviceLocalMB = 0.0f;

        PROPERTY()
        TVector<SMemoryGpuCategory> GpuCategories;

        PROPERTY()
        TVector<SMemoryGpuHeap> GpuHeaps;
    };

    REFLECT()
    enum class EEngineSettingsAction : uint8
    {
        Get,
        Set,
    };

    REFLECT()
    struct MCPEDITOR_API SEngineSettingsParams
    {
        GENERATED_BODY()

        PROPERTY()
        EEngineSettingsAction Action = EEngineSettingsAction::Get;

        // A settings class such as RendererSettings, by class or display name. Get with none lists every class.
        PROPERTY()
        FString Class;

        // Set only. The field to change, dotted for nested structs.
        PROPERTY()
        FString Path;

        // Set only. The new value as JSON, so strings and enum names need quotes.
        PROPERTY()
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SSettingsClassInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        FString ConfigFile;

        // Every field as JSON, only filled when a single class was asked for.
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SEngineSettingsResult
    {
        GENERATED_BODY()

        // Get only.
        PROPERTY()
        TVector<SSettingsClassInfo> Classes;

        // Set only.
        PROPERTY()
        FString Previous;

        PROPERTY()
        FString Current;
    };

    REFLECT()
    struct MCPEDITOR_API SGpuProfileParams
    {
        GENERATED_BODY()

        // How many of the most expensive passes to list.
        PROPERTY()
        int32 Top = 25;
    };

    REFLECT()
    struct MCPEDITOR_API SGpuPassTime
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        float Ms = 0.0f;

        // Nesting depth, so a parent's time includes its children's.
        PROPERTY()
        int32 Depth = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SGpuProfile
    {
        GENERATED_BODY()

        PROPERTY()
        bool bReady = false;

        PROPERTY()
        float TotalMs = 0.0f;

        PROPERTY()
        TVector<SGpuPassTime> Passes;
    };

    namespace MCP
    {
        MCPEDITOR_API void RegisterRuntimeTools(FStringView Owner);
    }
}
