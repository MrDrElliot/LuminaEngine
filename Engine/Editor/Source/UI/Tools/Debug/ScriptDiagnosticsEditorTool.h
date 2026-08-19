#pragma once

#include "UI/Tools/EditorTool.h"
#include "Containers/Vector.h"
#include "Scripting/DotNet/DotNetHost.h"

namespace Lumina
{
    // "C# Diagnostics": live view of the managed (LuminaSharp) runtime, managed heap + GC counters, the
    // allocation-churn rate, and the collectible-ALC / script-generation health that reveals a hot-reload
    // unload leak ("Resident generations": 1 is healthy, a climbing count is a leak). Polls the managed host
    // only while this window is open, so there is zero runtime cost otherwise (and none at all without the
    // editor). Editor-only.
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
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;

    private:

        void DrawWindow(bool bIsFocused);
        void Refresh(bool bForceCollect);

        DotNet::FScriptDiagnostics Snapshot;     // last sampled values (held stable while frozen)
        bool   bAvailable   = false;             // false until the first successful sample
        bool   bFrozen      = false;
        uint32 DrawTicks    = 0;
        float  RefreshTimer = 0.0f;

        // Derived allocation-churn rate (MB/s) from the lifetime TotalAllocatedBytes delta between refreshes.
        int64  PrevAllocBytes = 0;
        bool   bHavePrevAlloc = false;
        float  AllocRateMBs   = 0.0f;

        // Rolling history for the timelines (newest at the back).
        TVector<float> HistHeapMB;
        TVector<float> HistAllocRateMB;
        TVector<float> HistWorkingSetMB;
    };
}
