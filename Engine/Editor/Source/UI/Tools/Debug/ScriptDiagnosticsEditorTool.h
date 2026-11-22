#pragma once

#include "UI/Tools/EditorTool.h"
#include "Containers/RingBuffer.h"
#include "Containers/Vector.h"
#include "Scripting/DotNet/DotNetHost.h"

namespace Lumina
{
    // Live managed runtime view that polls only while open, where a climbing Resident generations count reveals a hot-reload unload leak.
    class FScriptDiagnosticsEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FScriptDiagnosticsEditorTool)

        FScriptDiagnosticsEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "C# Diagnostics", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_LANGUAGE_CSHARP; }

        void OnInitialize() override;

    private:

        void DrawWindow(bool bIsFocused);
        void Refresh(bool bForceCollect);

        DotNet::FScriptDiagnostics Snapshot;     // last sampled values (held stable while frozen)

        // Minted C# classes counted under the native class each one extends, refreshed with the snapshot.
        FString MintedClassSummary;
        bool   bAvailable   = false;             // false until the first successful sample
        bool   bFrozen      = false;
        uint32 DrawTicks    = 0;
        float  RefreshTimer = 0.0f;

        // Derived allocation-churn rate (MB/s) from the lifetime TotalAllocatedBytes delta between refreshes.
        int64  PrevAllocBytes = 0;
        bool   bHavePrevAlloc = false;
        float  AllocRateMBs   = 0.0f;

        // Rolling history for the timelines (newest at the back).
        TRingBuffer<float> HistHeapMB;
        TRingBuffer<float> HistAllocRateMB;
        TRingBuffer<float> HistWorkingSetMB;
    };
}
