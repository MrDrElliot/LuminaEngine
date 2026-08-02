#pragma once
#include "UI/Tools/EditorTool.h"

#include <atomic>
#include <mutex>
#include <thread>

#include "Memory/SmartPtr.h"

namespace Lumina
{
    // Project packaging tool: cooks the asset graph into per-chunk .paks, optionally builds the
    // Shipping exe + copies it. Cook on the main thread (GC-racy); MSBuild + copy on a worker.
    class FProjectPackagerEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FProjectPackagerEditorTool)

        FProjectPackagerEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Package Project", nullptr)
        {}

        ~FProjectPackagerEditorTool() override;

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_PACKAGE_VARIANT; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        // High-level state. Cook is sub-second so it lives on the main thread
        // for one frame; Build+Copy is the long part and runs on the worker.
        enum class EStage
        {
            Idle,
            Cooking,
            Building,
            Copying,
            Done,
        };

        // Shared between UI and worker threads via shared_ptr so the worker keeps it
        // alive even if the tool closes mid-build.
        struct FBuildSession
        {
            std::atomic<bool>           bDone{false};
            std::atomic<bool>           bSuccess{false};
            std::mutex                  PendingMutex;
            TVector<FString>            PendingLines;       // worker -> UI
            FString                     OutputDirectory;    // populated on completion
            FString                     PakPath;
            FString                     ErrorMessage;
        };

        void DrawWindow(bool bIsFocused);
        void DrawExtrasSection();
        void RunCookOnly();
        void RunFullPackage();

        // Pulls any new lines from Session->PendingLines into LogLines.
        void DrainSession();

        // Adds a line directly (UI thread only, main path for synchronous events).
        void AppendLog(FStringView Line);
        void ClearLog();

        FString             OutputDir;          // editable; defaults set on first draw
        int32               ConfigIndex     = 0; // 0=Shipping, 1=Development, 2=Debug
        bool                bExtractScriptsLoose = false;
        TVector<FString>    ExtraFiles;
        TVector<FString>    ExtraDirectories;
        int32               SelectedExtraFile      = -1;
        int32               SelectedExtraDirectory = -1;
        EStage              Stage           = EStage::Idle;
        bool                bLastSuccess    = false;
        FString             LastError;
        FString             LastPakPath;       // Main chunk PAK; reused for "Open Output".
        FString             LastOutputDir;
        // One entry per chunk PAK written by the last successful cook
        // (Main always present; UI/Script/Primary/etc. only when in-use).
        struct FChunkSummary
        {
            FString Name;
            FString Path;
            size_t  Assets = 0;
            size_t  Bytes  = 0;
        };
        TVector<FChunkSummary> LastChunks;
        TVector<FString>    LogLines;           // ring-bounded; UI thread only
        bool                bAutoScroll     = true;

        // Async pipeline state.
        TSharedPtr<FBuildSession>   Session;
        std::thread                 Worker;

        static constexpr int32 MaxLogLines = 2000;
    };
}
