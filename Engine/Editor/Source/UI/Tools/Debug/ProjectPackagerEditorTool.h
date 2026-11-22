#pragma once
#include "Core/Threading/Thread.h"
#include "UI/Tools/EditorTool.h"

#include <atomic>

#include "Memory/SmartPtr.h"

namespace Lumina
{
    // Cooks the asset graph into per-chunk paks on the main thread since it races the GC, then builds and copies on a worker.
    class FProjectPackagerEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FProjectPackagerEditorTool)

        FProjectPackagerEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Package Project", nullptr)
        {}

        ~FProjectPackagerEditorTool() override;
        FProjectPackagerEditorTool(const FProjectPackagerEditorTool&) = delete;
        FProjectPackagerEditorTool& operator=(const FProjectPackagerEditorTool&) = delete;
        FProjectPackagerEditorTool(FProjectPackagerEditorTool&&) = delete;
        FProjectPackagerEditorTool& operator=(FProjectPackagerEditorTool&&) = delete;

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_PACKAGE_VARIANT; }

        void OnInitialize() override;
        void DrawHelpMenu() override;

    private:

        // Cook is sub-second and stays on the main thread, while build and copy is the long part on the worker.
        enum class EStage
        {
            Idle,
            Cooking,
            Building,
            Copying,
            Done,
        };

        struct FBuildSession
        {
            std::atomic<bool>           bDone{false};
            std::atomic<bool>           bSuccess{false};
            FMutex                  PendingMutex;
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

        FString DefaultOutputDir(bool bForServer) const;
        FString PackageName() const;

        FString             OutputDir;          // editable; defaults set on first draw
        int32               ConfigIndex     = 0; // 0=Shipping, 1=Development, 2=Debug
        int32               PlatformIndex   = 0; // EPackagePlatform
        bool                bExtractScriptsLoose = false;
        bool                bServer              = false;
        TVector<FString>    ExtraFiles;
        TVector<FString>    ExtraDirectories;
        int32               SelectedExtraFile      = -1;
        int32               SelectedExtraDirectory = -1;
        EStage              Stage           = EStage::Idle;
        bool                bLastSuccess    = false;
        FString             LastError;
        FString             LastPakPath;       // Main chunk PAK; reused for "Open Output".
        FString             LastOutputDir;
        // One entry per chunk pak from the last successful cook, with Main always present.
        struct FChunkSummary
        {
            FString Name;
            FString Path;
            int32   Assets = 0;
            int64   Bytes  = 0;
        };
        TVector<FChunkSummary> LastChunks;
        TVector<FString>    LogLines;           // ring-bounded; UI thread only
        bool                bAutoScroll     = true;

        // Async pipeline state.
        TSharedPtr<FBuildSession>   Session;
        FThread                 Worker;

        static constexpr int32 MaxLogLines = 2000;
    };
}
