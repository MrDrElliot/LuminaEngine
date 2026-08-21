#include "Core/Threading/Thread.h"
#include "ProjectPackagerEditorTool.h"

#include "Platform/Filesystem/PlatformFilesystem.h"

#include "Assets/AssetRegistry/CookRoot.h"
#include "Config/Config.h"
#include "Cooker/AssetCooker.h"
#include "Cooker/ProjectPackager.h"
#include "Core/Engine/Engine.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Platform/Process/PlatformProcess.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    FProjectPackagerEditorTool::~FProjectPackagerEditorTool()
    {
        if (Worker.joinable())
        {
            Worker.detach();
        }
    }

    void FProjectPackagerEditorTool::OnInitialize()
    {
        CreateToolWindow("Package Project", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FProjectPackagerEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FProjectPackagerEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("What this does",
            "Walks FAssetData dependencies from the cook roots (project + enabled-plugin CookRoots[], "
            "plus every asset flagged EAssetFlags::Primary), strips EditorOnly properties via a cook-mode "
            "FPackageSaver, and writes one .pak per chunk. The Main chunk also carries shared content "
            "(/Engine resources, shader cache, /Config) and a pre-baked /Engine/AssetRegistry.bin so the "
            "runtime skips the filesystem rescan.");
        DrawHelpTextRow("Chunks",
            "FCookGraph assigns each asset a chunk name (inherited along Hard/Owned edges from its root). "
            "The cooker writes <name>.pak for Main and <name>-<chunk>.pak for others, side-by-side. The "
            "runtime mounts every .pak next to the exe at startup.");
        DrawHelpTextRow("DDC",
            "Cooked bytes are cached at <EngineInstall>/Intermediates/DDC/. Repeat cooks of unchanged "
            "assets skip the load+resave entirely. Bump FCookDDC::kCookStamp in code to invalidate the "
            "whole cache after changing cook logic.");
        DrawHelpTextRow("Cook vs Full Package",
            "Cook Only stops after the .pak set, fast iteration when you only changed assets. "
            "Full Package runs cook + build + copy and produces a runnable distribution.");
        DrawHelpTextRow("Configurations",
            "Shipping (default): no editor / no debug overhead, optimized. Development: profiler scopes + "
            "asserts kept. Debug: unoptimized.");
        DrawHelpTextRow("Extras",
            "The Extras section adds files/directories to copy alongside the build (e.g. third-party DLLs, "
            "docs, OpenSSL certs). Paths are relative to project root.");
        DrawHelpTextRow("Build Output",
            "The lower pane streams build output live. If a build fails, the stderr lines containing 'error' "
            "are highlighted; full log is also kept on disk under <OutputDir>/build.log.");
    }

    void FProjectPackagerEditorTool::AppendLog(FStringView Line)
    {
        LogLines.emplace_back(Line.data(), Line.size());
        if ((int32)LogLines.size() > MaxLogLines)
        {
            LogLines.erase(LogLines.begin(), LogLines.begin() + (LogLines.size() - MaxLogLines));
        }
    }

    void FProjectPackagerEditorTool::ClearLog()
    {
        LogLines.clear();
        LastError.clear();
        LastPakPath.clear();
        LastOutputDir.clear();
        LastChunks.clear();
        bLastSuccess = false;
    }

    namespace
    {
        // Mirrored into FChunkSummary so the UI never keeps a pointer into a stale result.
        template<typename ToolT>
        void CaptureChunksFromResult(const FCookResult& Result, ToolT& Out)
        {
            Out.clear();
            Out.reserve(Result.Chunks.size());
            for (const FCookChunkResult& C : Result.Chunks)
            {
                typename ToolT::value_type Entry;
                Entry.Name   = C.Chunk.ToString().c_str();
                Entry.Path   = C.PakPath;
                Entry.Assets = C.NumAssets;
                Entry.Bytes  = C.Bytes;
                Out.push_back(Move(Entry));
            }
        }
    }

    void FProjectPackagerEditorTool::DrainSession()
    {
        if (!Session)
        {
            return;
        }

        // The pending vector is swapped with an empty one to minimize lock hold time.
        TVector<FString> Drained;
        {
            FScopeLock Lock(Session->PendingMutex);
            Drained.swap(Session->PendingLines);
        }
        for (FString& Line : Drained)
        {
            AppendLog(Line);
        }

        // If the worker has finished, capture its result and reset.
        if (Session->bDone.load(std::memory_order_acquire) && Stage == EStage::Building)
        {
            bLastSuccess = Session->bSuccess.load(std::memory_order_acquire);
            LastError = Session->ErrorMessage;
            LastOutputDir = Session->OutputDirectory;
            LastPakPath = Session->PakPath;

            if (bLastSuccess)
            {
                AppendLog(FString("DONE: ") + LastOutputDir);
            }
            else
            {
                AppendLog(FString("FAILED: ") + LastError);
            }

            // Worker is finished, join (instant) before releasing our handle.
            if (Worker.joinable())
            {
                Worker.join();
            }
            Session.reset();
            Stage = EStage::Done;
        }
    }

    void FProjectPackagerEditorTool::RunCookOnly()
    {
        if (Stage != EStage::Idle && Stage != EStage::Done)
        {
            return;
        }
        if (GEngine == nullptr || GEngine->GetProjectName().empty())
        {
            AppendLog("[error] No project loaded.");
            return;
        }

        ClearLog();
        Stage = EStage::Cooking;

        const FString ProjectName(GEngine->GetProjectName().data(), GEngine->GetProjectName().size());

        FString PakDir = OutputDir.empty()
            ? FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size()) + "/Build/" + ProjectName
            : OutputDir;

        Filesystem::MakeDirectoryTree(PakDir);

        const FString PakPath = PakDir + "/" + ProjectName + ".pak";

        FCookOptions CookOpts;
        CookOpts.bExtractScriptsAsLooseFiles = bExtractScriptsLoose;
        CookOpts.ExtraFiles                  = ExtraFiles;
        CookOpts.ExtraDirectories            = ExtraDirectories;

        const FCookResult Result = FAssetCooker::Cook(PakPath, CookOpts, [this](FStringView Line)
        {
            AppendLog(Line);
        });

        if (Result.bSuccess)
        {
            // Runs on the UI thread, since it walks the VFS that other main-thread systems may mutate.
            if (bExtractScriptsLoose)
            {
                AppendLog("Extracting loose scripts...");
                const size_t Extracted = FProjectPackager::ExtractLooseScripts(PakDir,
                    [this](FStringView Line) { AppendLog(Line); });
                AppendLog(Format("Extracted {} loose script files.", Extracted).c_str());
            }

            bLastSuccess = true;
            LastPakPath = PakPath;
            LastOutputDir = PakDir;
            CaptureChunksFromResult(Result, LastChunks);
            AppendLog(Format("DONE: {} assets across {} chunk(s), {} bytes total -> {}",
                Result.NumAssetsCooked, Result.Chunks.size(), Result.TotalBytes, PakDir.c_str()).c_str());
        }
        else
        {
            LastError = Result.ErrorMessage;
            AppendLog(FString("FAILED: ") + Result.ErrorMessage);
        }

        Stage = EStage::Done;
    }

    void FProjectPackagerEditorTool::RunFullPackage()
    {
        if (Stage != EStage::Idle && Stage != EStage::Done)
        {
            return;
        }
        if (GEngine == nullptr || GEngine->GetProjectName().empty())
        {
            AppendLog("[error] No project loaded.");
            return;
        }
        if (Worker.joinable())
        {
            // Previous worker still alive (e.g. user closed and reopened during a build).
            AppendLog("[error] A previous build is still running; wait for it to finish.");
            return;
        }

        ClearLog();

        // Touches engine state, but is fast, typically sub-second for a small project.
        Stage = EStage::Cooking;

        const FString ProjectName(GEngine->GetProjectName().data(), GEngine->GetProjectName().size());

        FString PakDir = OutputDir.empty()
            ? FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size()) + "/Build/" + ProjectName
            : OutputDir;

        Filesystem::MakeDirectoryTree(PakDir);

        const FString PakPath = PakDir + "/" + ProjectName + ".pak";

        FCookOptions CookOpts;
        CookOpts.bExtractScriptsAsLooseFiles = bExtractScriptsLoose;
        CookOpts.ExtraFiles                  = ExtraFiles;
        CookOpts.ExtraDirectories            = ExtraDirectories;

        const FCookResult Cook = FAssetCooker::Cook(PakPath, CookOpts, [this](FStringView Line)
        {
            AppendLog(Line);
        });

        if (!Cook.bSuccess)
        {
            LastError = Cook.ErrorMessage;
            bLastSuccess = false;
            AppendLog(FString("FAILED (cook): ") + Cook.ErrorMessage);
            Stage = EStage::Done;
            return;
        }

        CaptureChunksFromResult(Cook, LastChunks);
        AppendLog(Format("Cook OK: {} assets across {} chunk(s), {} bytes total",
            Cook.NumAssetsCooked, Cook.Chunks.size(), Cook.TotalBytes).c_str());

        // 1.5) Loose-script extraction must run on the main thread (uses VFS).
        if (bExtractScriptsLoose)
        {
            AppendLog("Extracting loose scripts...");
            const size_t Extracted = FProjectPackager::ExtractLooseScripts(PakDir,
                [this](FStringView Line) { AppendLog(Line); });
            AppendLog(Format("Extracted {} loose script files.", Extracted).c_str());
        }

        // Captures everything by value so the worker is independent of any UI state.
        Stage = EStage::Building;

        Session = MakeShared<FBuildSession>();
        Session->PakPath = PakPath;
        Session->OutputDirectory = PakDir;

        FPackageBuildOptions Opts;
        Opts.OutputDirectory                = PakDir;
        Opts.ProjectDirectory               = FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size());
        Opts.bBuildExecutable               = true;
        Opts.bExtractScriptsAsLooseFiles    = bExtractScriptsLoose;
        Opts.ExtraFiles                     = ExtraFiles;
        Opts.ExtraDirectories               = ExtraDirectories;
        Opts.BuildConfiguration             = (ConfigIndex == 0) ? FString("Shipping")
                                            : (ConfigIndex == 1) ? FString("Development")
                                                                 : FString("Debug");

        TSharedPtr<FBuildSession> WorkerSession = Session;

        Worker = FThread([WorkerSession, Opts, ProjectName, PakPath]()
        {
            // Pushes into the mutex-protected pending buffer, drained each frame by the UI thread.
            auto LogFunc = [WorkerSession](FStringView Line)
            {
                FScopeLock Lock(WorkerSession->PendingMutex);
                WorkerSession->PendingLines.emplace_back(Line.data(), Line.size());
            };

            const FPackageBuildResult Result = FProjectPackager::BuildAndCopyOnly(
                Opts,
                FStringView(ProjectName.c_str(), ProjectName.size()),
                FStringView(PakPath.c_str(), PakPath.size()),
                LogFunc);

            WorkerSession->bSuccess.store(Result.bSuccess, std::memory_order_release);
            WorkerSession->ErrorMessage = Result.ErrorMessage;
            WorkerSession->OutputDirectory = Result.OutputDirectory.empty()
                ? WorkerSession->OutputDirectory
                : Result.OutputDirectory;
            WorkerSession->bDone.store(true, std::memory_order_release);
        });
    }

    void FProjectPackagerEditorTool::DrawWindow(bool bIsFocused)
    {
        if (GEngine == nullptr || GEngine->GetProjectName().empty())
        {
            ImGui::TextDisabled("No project loaded.");
            return;
        }

        // Pull worker output into the displayed log every frame.
        DrainSession();

        // Lazy-init defaults so they reflect the current project.
        if (OutputDir.empty())
        {
            const FString ProjectName(GEngine->GetProjectName().data(), GEngine->GetProjectName().size());
            OutputDir = FString(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size())
                + "/Build/" + ProjectName;
        }

        // Project + cook-roots summary.
        ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), LE_ICON_PACKAGE_VARIANT " Package Project");
        ImGui::Separator();

        ImGui::Text("Project:    %.*s", (int)GEngine->GetProjectName().size(), GEngine->GetProjectName().data());

        const TVector<FCookRoot> Roots = GEngine->GetCookRoots();
        if (Roots.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "Cook Roots: <none, open Project Settings > Maps > Cook Roots to add asset paths,");
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "             or declare CookRoots in a .lplugin / flag assets EAssetFlags::Primary>");
        }
        else
        {
            const int32 ShownLimit = 6;
            ImGui::Text("Cook Roots: %zu", Roots.size());
            const int32 Count = (int32)Roots.size();
            for (int32 i = 0; i < Count && i < ShownLimit; ++i)
            {
                const FName ChunkName = Roots[i].Chunk.IsNone() ? FName("Main") : Roots[i].Chunk;
                ImGui::TextDisabled("    [%s]  %s", ChunkName.ToString().c_str(), Roots[i].Asset.c_str());
            }
            if (Count > ShownLimit)
            {
                ImGui::TextDisabled("    ... (+%d more)", Count - ShownLimit);
            }
        }

        // Status badge, visible at a glance while a build is running.
        const bool bIsRunning = (Stage == EStage::Cooking || Stage == EStage::Building || Stage == EStage::Copying);
        if (bIsRunning)
        {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
            const char* StageName = (Stage == EStage::Cooking)  ? "COOKING"
                                  : (Stage == EStage::Building) ? "BUILDING"
                                  : (Stage == EStage::Copying)  ? "COPYING"
                                                                : "WORKING";
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), LE_ICON_PROGRESS_CLOCK " %s...", StageName);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Editable fields.
        char OutBuf[512]; std::snprintf(OutBuf, sizeof(OutBuf), "%s", OutputDir.c_str());
        ImGui::Text("Output Directory");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##out", OutBuf, sizeof(OutBuf)))
        {
            OutputDir = OutBuf;
        }

        ImGui::Text("Build Configuration");
        const char* Configs[] = { "Shipping", "Development", "Debug" };
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("##cfg", &ConfigIndex, Configs, IM_ARRAYSIZE(Configs));

        ImGui::Spacing();

        ImGui::Checkbox("Expose /Game files as loose (modder-friendly)", &bExtractScriptsLoose);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "When enabled, every non-.lasset file under /Game/ (.rcss, .rml,\n"
                "JSON, etc) is mirrored next to the cooked exe instead of being\n"
                "bundled in the PAK. End users (or you, post-ship) can tweak\n"
                "gameplay and UI without re-cooking. The runtime mounts the\n"
                "loose folder over the PAK so loose copies take priority.");
        }

        ImGui::Spacing();
        DrawExtrasSection();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Action buttons.
        ImGui::BeginDisabled(bIsRunning || Roots.empty());
        {
            if (ImGui::Button(LE_ICON_PACKAGE_VARIANT " Cook + Build", ImVec2(180, 32)))
            {
                RunFullPackage();
            }
            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_FILE_OUTLINE " Cook PAK Only", ImVec2(180, 32)))
            {
                RunCookOnly();
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_BROOM " Clear Log", ImVec2(120, 32)))
        {
            ClearLog();
        }

        if (bLastSuccess && !LastOutputDir.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_FOLDER_OPEN " Open Output", ImVec2(140, 32)))
            {
                Platform::LaunchURL(UTF8_TO_TCHAR(LastOutputDir.c_str()));
            }
        }

        // A compact table with one row per .pak written, each click-to-open.
        if (!LastChunks.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                LE_ICON_FILE_OUTLINE " Last cook output (%zu chunk%s):",
                LastChunks.size(), LastChunks.size() == 1 ? "" : "s");

            if (ImGui::BeginTable("##chunks", 4,
                ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
            {
                ImGui::TableSetupColumn("Chunk",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Path",   ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (const FChunkSummary& C : LastChunks)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(C.Name.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%zu", C.Assets);
                    ImGui::TableSetColumnIndex(2);
                    if (C.Bytes >= 1024 * 1024)
                        ImGui::Text("%.2f MiB", (double)C.Bytes / (1024.0 * 1024.0));
                    else if (C.Bytes >= 1024)
                        ImGui::Text("%.1f KiB", (double)C.Bytes / 1024.0);
                    else
                        ImGui::Text("%zu B", C.Bytes);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(C.Path.c_str());
                }
                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        ImGui::Checkbox("Auto-scroll log", &bAutoScroll);

        ImGui::Separator();

        // Near-black with a subtle border and generous padding, so lines are not cramped against the edge.
        ImGui::PushStyleColor(ImGuiCol_ChildBg,    ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border,     ImVec4(0.20f, 0.20f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.04f, 0.04f, 0.05f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));

        if (ImGui::BeginChild("##log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar))
        {
            const ImVec4 ColDefault(0.85f, 0.85f, 0.88f, 1.0f);
            const ImVec4 ColBuildTool(0.55f, 0.65f, 0.80f, 1.0f);
            const ImVec4 ColCookEntry(0.65f, 0.82f, 0.55f, 1.0f);
            const ImVec4 ColError  (1.00f, 0.40f, 0.40f, 1.0f);
            const ImVec4 ColWarn   (1.00f, 0.78f, 0.35f, 1.0f);
            const ImVec4 ColSuccess(0.45f, 1.00f, 0.55f, 1.0f);

            for (const FString& Line : LogLines)
            {
                ImVec4 Color = ColDefault;
                if (Line.find("[error]") != FString::npos || Line.find("FAILED") != FString::npos
                    || Line.find(" error ") != FString::npos || Line.find("error C") != FString::npos
                    || Line.find("error LNK") != FString::npos)
                {
                    Color = ColError;
                }
                else if (Line.find("[warn]") != FString::npos || Line.find(" warning ") != FString::npos)
                {
                    Color = ColWarn;
                }
                else if (Line.find("DONE") != FString::npos)
                {
                    Color = ColSuccess;
                }
                else if (Line.size() >= 4 && Line[0] == ' ' && Line[1] == ' ' && Line[2] == '+')
                {
                    Color = ColCookEntry;
                }
                else if (Line.size() >= 4 && Line[0] == ' ' && Line[1] == ' ' && Line[2] == '|')
                {
                    Color = ColBuildTool;
                }

                ImGui::PushStyleColor(ImGuiCol_Text, Color);
                ImGui::TextUnformatted(Line.c_str());
                ImGui::PopStyleColor();
            }
            if (bAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
            {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(3);
    }

    void FProjectPackagerEditorTool::DrawExtrasSection()
    {
        const FString ProjectPath(GEngine->GetProjectPath().data(), GEngine->GetProjectPath().size());

        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.55f, 1.0f), LE_ICON_PACKAGE_VARIANT_CLOSED " Extras (manually included in PAK)");
        ImGui::Spacing();

        // Extra files
        ImGui::TextDisabled("Extra Files");
        if (ImGui::BeginListBox("##extra_files", ImVec2(-1, 80)))
        {
            for (int32 i = 0; i < (int32)ExtraFiles.size(); ++i)
            {
                const bool bSelected = (SelectedExtraFile == i);
                if (ImGui::Selectable(ExtraFiles[i].c_str(), bSelected))
                {
                    SelectedExtraFile = i;
                }
            }
            ImGui::EndListBox();
        }
        if (ImGui::Button(LE_ICON_FILE_PLUS " Add File...", ImVec2(140, 0)))
        {
            FFixedString Picked;
            // Filter is "Display\0Pattern\0Display\0Pattern\0\0"; pass a generic All Files match.
            static const char Filter[] = "All Files\0*.*\0";
            if (Platform::OpenFileDialogue(Picked, "Add Extra File", Filter, ProjectPath.c_str()))
            {
                ExtraFiles.emplace_back(Picked.c_str(), Picked.size());
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(SelectedExtraFile < 0 || SelectedExtraFile >= (int32)ExtraFiles.size());
        if (ImGui::Button(LE_ICON_MINUS " Remove File", ImVec2(140, 0)))
        {
            ExtraFiles.erase(ExtraFiles.begin() + SelectedExtraFile);
            SelectedExtraFile = -1;
        }
        ImGui::EndDisabled();

        ImGui::Spacing();

        // Extra directories
        ImGui::TextDisabled("Extra Directories");
        if (ImGui::BeginListBox("##extra_dirs", ImVec2(-1, 80)))
        {
            for (int32 i = 0; i < (int32)ExtraDirectories.size(); ++i)
            {
                const bool bSelected = (SelectedExtraDirectory == i);
                if (ImGui::Selectable(ExtraDirectories[i].c_str(), bSelected))
                {
                    SelectedExtraDirectory = i;
                }
            }
            ImGui::EndListBox();
        }
        if (ImGui::Button(LE_ICON_FOLDER_PLUS " Add Directory...", ImVec2(180, 0)))
        {
            FFixedString Picked;
            // Null filter = folder picker (see WindowsPlatformProcess::OpenFileDialogue).
            if (Platform::OpenFileDialogue(Picked, "Add Extra Directory", nullptr, ProjectPath.c_str()))
            {
                ExtraDirectories.emplace_back(Picked.c_str(), Picked.size());
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(SelectedExtraDirectory < 0 || SelectedExtraDirectory >= (int32)ExtraDirectories.size());
        if (ImGui::Button(LE_ICON_MINUS " Remove Directory", ImVec2(180, 0)))
        {
            ExtraDirectories.erase(ExtraDirectories.begin() + SelectedExtraDirectory);
            SelectedExtraDirectory = -1;
        }
        ImGui::EndDisabled();
    }
}
