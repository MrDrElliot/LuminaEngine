#include "MCPProfilerTools.h"

#include "Agent/AgentGameThread.h"
#include "Agent/AgentToolRegistry.h"
#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Math.h"
#include "Log/Log.h"
#include "MCPTextMatch.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"
#include "Core/Profiler/Profile.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"

#include <cstdlib>

namespace Lumina::MCP
{
    namespace MCPProfilerToolsInternal
    {
        constexpr float MaxProfilerCaptureSeconds = 60.0f;
        constexpr char TracySeparator = ';';
        constexpr const char* TracyFrameZone = "Lumina::FEngine::Update";
        constexpr const char* TracyLimiterZone = "Frame-Rate-Limiter";
        constexpr const char* TracyGpuWaitZone = "Frame Fence (GPU)";

        struct FProfilerZoneTotals
        {
            double TotalNs = 0.0;
            double MaxNs = 0.0;
            int64 Count = 0;
        };

        TVector<FStringView> SplitProfilerFields(FStringView Line)
        {
            TVector<FStringView> Fields;
            int32 Start = 0;
            while (Start <= Line.Len())
            {
                const int32 End = Line.Find(TracySeparator, Start);
                Fields.Add(Line.Mid(Start, End == Constants::kIndexNone ? Constants::kIndexNone : End - Start));
                if (End == Constants::kIndexNone)
                {
                    break;
                }
                Start = End + 1;
            }
            return Fields;
        }

        double ProfilerNumber(FStringView Field)
        {
            const FString Text(Field.GetData(), Field.Len());
            return std::strtod(*Text, nullptr);
        }

        // Tracy's CLI tools ship beside the engine; off Windows a system install on PATH works too.
        FString TracyToolPath(const char* Name)
        {
            const FString Bundled = Paths::GetEngineInstallDirectory() + "/External/Tracy/" + Name;
#ifdef LE_PLATFORM_WINDOWS
            return Bundled + ".exe";
#else
            if (Paths::Exists(Bundled))
            {
                return Bundled;
            }

            const char* PathValue = std::getenv("PATH");
            const FStringView Search(PathValue != nullptr ? PathValue : "");
            int32 Start = 0;
            while (Start < Search.Len())
            {
                int32 End = Search.Find(':', Start);
                End = End == Constants::kIndexNone ? Search.Len() : End;
                const FString Candidate = FString(Search.Mid(Start, End - Start)) + "/" + Name;
                if (End > Start && Paths::Exists(Candidate))
                {
                    return Candidate;
                }
                Start = End + 1;
            }
            return Bundled;
#endif
        }

        // Bounded, since a tracy-capture left connected by an earlier run would otherwise hold the tool call forever.
        constexpr uint32 TracyExportTimeoutMs = 30000;
        // Every event of a long capture, which the hitch report reads, takes longer to write out.
        constexpr uint32 TracyUnwrapTimeoutMs = 120000;

        bool RunTracyTool(const FString& Executable, const FString& Arguments, const TFunction<void(FStringView)>& OnLine,
                     uint32 TimeoutMilliseconds = TracyExportTimeoutMs)
        {
            const int ExitCode = Platform::RunProcessAndWaitCapture(UTF8_TO_TCHAR(*Executable), UTF8_TO_TCHAR(*Arguments),
                                                                    nullptr, OnLine, TimeoutMilliseconds);
            if (ExitCode == Platform::ProcessTimedOutExitCode)
            {
                OnLine(FStringView("error: timed out, so it was stopped"));
            }
            return ExitCode == 0;
        }

        Agent::FToolResult TracyExportFailed(const FString& CaptureFile, const FString& LastLine)
        {
            return Agent::FToolResult::Error(Lumina::Format("tracy-csvexport failed on the trace at {} ({}); record again.",
                CaptureFile, LastLine.IsEmpty() ? FString("no output") : LastLine));
        }

        // Rows repeat a name when several source sites share it, so totals are summed by name.
        bool ReadTracyZones(const FString& Executable, const FString& Arguments, bool bGpuRows, THashMap<FString, FProfilerZoneTotals>& Out,
                            FString& OutLastLine)
        {
            bool bHeader = true;
            return RunTracyTool(Executable, Arguments, [&](FStringView Line)
            {
                OutLastLine = FString(Line.GetData(), Line.Len());
                if (bHeader)
                {
                    bHeader = false;
                    return;
                }

                const TVector<FStringView> Fields = SplitProfilerFields(Line);
                if (Fields.Num() < (bGpuRows ? 4 : 9))
                {
                    return;
                }

                FProfilerZoneTotals& Totals = Out.FindOrAdd(FString(Fields[0].GetData(), Fields[0].Len()));
                if (bGpuRows)
                {
                    const double Ns = ProfilerNumber(Fields[3]);
                    Totals.TotalNs += Ns;
                    Totals.MaxNs = Math::Max(Totals.MaxNs, Ns);
                    Totals.Count += 1;
                }
                else
                {
                    Totals.TotalNs += ProfilerNumber(Fields[3]);
                    Totals.Count += (int64)ProfilerNumber(Fields[5]);
                    Totals.MaxNs = Math::Max(Totals.MaxNs, ProfilerNumber(Fields[8]));
                }
            });
        }

        TVector<SProfilerZone> RankProfilerZones(const THashMap<FString, FProfilerZoneTotals>& Zones, int32 Frames, int32 Top, const FString& Filter)
        {
            TVector<SProfilerZone> Ranked;
            for (const auto& [Name, Totals] : Zones)
            {
                if (Totals.Count == 0 || !ContainsTextFold(Name, Filter))
                {
                    continue;
                }

                SProfilerZone& Zone = Ranked.EmplaceGetRef();
                Zone.Name = Name;
                Zone.Count = (int32)Totals.Count;
                Zone.MsPerFrame = (float)(Totals.TotalNs / 1.0e6 / Math::Max(Frames, 1));
                Zone.MeanUs = (float)(Totals.TotalNs / 1.0e3 / (double)Totals.Count);
                Zone.MaxUs = (float)(Totals.MaxNs / 1.0e3);
            }

            Algo::Sort(Ranked, [](const SProfilerZone& A, const SProfilerZone& B) { return A.MsPerFrame > B.MsPerFrame; });
            if (Ranked.Num() > Top)
            {
                Ranked.SetNum(Top);
            }
            return Ranked;
        }

        float ZoneMsPerFrame(const THashMap<FString, FProfilerZoneTotals>& Zones, const char* Name, int32 Frames)
        {
            const auto Found = Zones.Find(FString(Name));
            return Found == nullptr ? 0.0f : (float)(Found->TotalNs / 1.0e6 / Math::Max(Frames, 1));
        }

        // One capture at a time runs on its own thread, so the scene it measures can still be driven through other tools.
        struct FProfilerCaptureJob
        {
            FString File;
            FString Failure;
            int32 Seconds = 0;
            bool bCaptured = false;
            FThread Worker;
        };

        FMutex ProfilerJobMutex;
        TUniquePtr<FProfilerCaptureJob> ProfilerJob;
        FString LastProfilerCaptureFile;

        bool StartProfilerCapture(float Seconds, bool bUncapFrameRate, FString& OutFile, FString& OutError)
        {
            FScopeLock Lock(ProfilerJobMutex);
            if (ProfilerJob)
            {
                OutError = Lumina::Format("A capture to {} has not been reported yet; call profiler.report first.", ProfilerJob->File);
                return false;
            }

#if defined(TRACY_ENABLE)
            const uint32 ListenPort = tracy::GetProfiler().GetListenPort();
#else
            const uint32 ListenPort = 0;
#endif
            if (ListenPort == 0)
            {
                OutError = "This editor's Tracy client is not listening, so there is nothing to capture.";
                return false;
            }

            FString Directory = Paths::GetEngineDirectory() + "/Saved/Profiling";
            Paths::CreateDirectories(FStringView(*Directory, Directory.Len()));

            ProfilerJob = MakeUnique<FProfilerCaptureJob>();
            ProfilerJob->File = Lumina::Format("{}/Capture_{}.tracy", Directory, (uint64)(PlatformTime::Seconds() * 1000.0));
            OutFile = ProfilerJob->File;

            FProfilerCaptureJob* Running = ProfilerJob.get();
            const int32 CaptureSeconds = (int32)Math::Ceil(Math::Clamp(Seconds, 1.0f, MaxProfilerCaptureSeconds));
            Running->Seconds = CaptureSeconds;
            Running->Worker = FThread([Running, CaptureSeconds, bUncapFrameRate, ListenPort]()
            {
                const int32 GateTimeout = Agent::FGameThreadGate::GetDefaultTimeoutMilliseconds();
                bool bCapLifted = false;
                int32 PreviousCap = -1;
                if (bUncapFrameRate)
                {
                    (void)Agent::FGameThreadGate::Run([&PreviousCap, &bCapLifted]()
                        {
                            if (GEngine != nullptr)
                            {
                                PreviousCap = GEngine->GetFrameRateCapOverride();
                                GEngine->SetFrameRateCapOverride(0);
                                bCapLifted = true;
                            }
                        }, GateTimeout);
                }

                const bool bRan = RunTracyTool(TracyToolPath("tracy-capture"),
                    Lumina::Format("-o \"{}\" -a 127.0.0.1 -p {} -f -s {}", Running->File, ListenPort, CaptureSeconds),
                    [Running](FStringView Line)
                    {
                        if (ContainsTextFold(Line, "error") || ContainsTextFold(Line, "cannot"))
                        {
                            Running->Failure.Append(Line.GetData(), Line.Len());
                        }
                    },
                    (uint32)CaptureSeconds * 1000u + 30000u);
                Running->bCaptured = bRan && Paths::Exists(Running->File);

                if (bCapLifted)
                {
                    (void)Agent::FGameThreadGate::Run([PreviousCap]()
                        {
                            if (GEngine != nullptr)
                            {
                                GEngine->SetFrameRateCapOverride(PreviousCap);
                            }
                        }, GateTimeout);
                }
            });
            return true;
        }

        Agent::FToolResult FinishProfilerCapture(int32 TopZones, const FString& Filter, bool bSelfTime, SProfilerCaptureResult& Out)
        {
            TUniquePtr<FProfilerCaptureJob> Finished;
            {
                FScopeLock Lock(ProfilerJobMutex);
                Finished = Move(ProfilerJob);
            }
            if (!Finished)
            {
                return Agent::FToolResult::Error("No capture is pending; call profiler.capture with bWait false first.");
            }

            if (Finished->Worker.Joinable())
            {
                Finished->Worker.Join();
            }

            Out.CaptureFile = Finished->File;
            if (Finished->bCaptured)
            {
                FScopeLock Lock(ProfilerJobMutex);
                LastProfilerCaptureFile = Finished->File;
            }
            if (!Finished->bCaptured)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "tracy-capture did not produce a trace ({}). Another Tracy client may already be connected.",
                    Finished->Failure.IsEmpty() ? FString("no output") : Finished->Failure));
            }

            const int32 Top = Math::Clamp(TopZones, 1, 200);
            const FString Export = TracyToolPath("tracy-csvexport");
            THashMap<FString, FProfilerZoneTotals> Cpu;
            THashMap<FString, FProfilerZoneTotals> Gpu;
            FString ExportLine;
            if (!ReadTracyZones(Export, Lumina::Format("-s \"{}\" {}\"{}\"", TracySeparator, bSelfTime ? "-e " : "", Out.CaptureFile), false, Cpu, ExportLine))
            {
                return TracyExportFailed(Out.CaptureFile, ExportLine);
            }
            if (Cpu.IsEmpty())
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "Tracy could not read the trace at {}; record again.", Out.CaptureFile));
            }
            FString GpuExportLine;
            const bool bGpuExported = ReadTracyZones(Export, Lumina::Format("-s \"{}\" -u -g \"{}\"", TracySeparator, Out.CaptureFile), true, Gpu, GpuExportLine);

            // A frame's self time is only what its children leave over, so frame totals always come from inclusive times.
            THashMap<FString, FProfilerZoneTotals> Inclusive;
            if (bSelfTime && !ReadTracyZones(Export, Lumina::Format("-s \"{}\" \"{}\"", TracySeparator, Out.CaptureFile), false, Inclusive, ExportLine))
            {
                return TracyExportFailed(Out.CaptureFile, ExportLine);
            }
            const THashMap<FString, FProfilerZoneTotals>& Totals = bSelfTime ? Inclusive : Cpu;

            const auto Frame = Totals.Find(FString(TracyFrameZone));
            if (Frame == nullptr || Frame->Count == 0)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "The trace at {} has no {} zones to count frames by, which usually means Tracy's export failed; record again.",
                    Out.CaptureFile, TracyFrameZone));
            }

            Out.Frames = (int32)Frame->Count;
            Out.FrameMs = (float)(Frame->TotalNs / 1.0e6 / Out.Frames);
            Out.MaxFrameMs = (float)(Frame->MaxNs / 1.0e6);
            Out.LimiterMs = ZoneMsPerFrame(Totals, TracyLimiterZone, Out.Frames);
            Out.GpuWaitMs = ZoneMsPerFrame(Totals, TracyGpuWaitZone, Out.Frames);
            Out.PeriodMs = (float)(Finished->Seconds * 1000.0 / Out.Frames);
            Out.Cpu = RankProfilerZones(Cpu, Out.Frames, Top, Filter);
            Out.Gpu = RankProfilerZones(Gpu, Out.Frames, Top, Filter);

            FString Message = Lumina::Format(
                "{} frames, {:.2f} ms apart ({:.0f} fps). The frame zone took {:.2f} ms ({:.2f} ms max), with {:.2f} ms more waiting "
                "on the GPU and {:.2f} ms in the frame limiter per frame. Trace saved to {}.",
                Out.Frames, Out.PeriodMs, 1000.0f / Math::Max(Out.PeriodMs, 0.001f), Out.FrameMs, Out.MaxFrameMs, Out.GpuWaitMs,
                Out.LimiterMs, Out.CaptureFile);
            if (!bGpuExported)
            {
                Message += Lumina::Format(" The GPU export failed ({}), so GPU pass timings are unavailable.",
                    GpuExportLine.IsEmpty() ? FString("no output") : GpuExportLine);
            }
            return Agent::FToolResult::Ok(Message);
        }

        // One zone event from an unwrapped export, with its name interned.
        struct FProfilerZoneEvent
        {
            int64 Start = 0;
            int64 Duration = 0;
            int64 Self = 0;
            int32 Name = 0;
            int32 Thread = 0;
        };

        void RankHitchZones(const THashMap<int32, SProfilerHitchZone>& Totals, int32 Top, TVector<SProfilerHitchZone>& Out)
        {
            for (const auto& [Id, Zone] : Totals)
            {
                Out.Add(Zone);
            }
            Algo::Sort(Out, [](const SProfilerHitchZone& A, const SProfilerHitchZone& B) { return A.SelfMs > B.SelfMs; });
            if (Out.Num() > Top)
            {
                Out.SetNum(Top);
            }
        }

        Agent::FToolResult FindProfilerHitches(const SProfilerReportParams& In, SProfilerHitchResult& Out)
        {
            Out.CaptureFile = In.CaptureFile;
            if (Out.CaptureFile.IsEmpty())
            {
                FScopeLock Lock(ProfilerJobMutex);
                Out.CaptureFile = LastProfilerCaptureFile;
            }
            if (Out.CaptureFile.IsEmpty() || !Paths::Exists(Out.CaptureFile))
            {
                return Agent::FToolResult::Error("No trace to read; record one with profiler.capture or pass CaptureFile.");
            }

            THashMap<FString, int32> NameIds;
            TVector<FString> Names;
            TVector<FProfilerZoneEvent> Events;
            bool bHeader = true;
            FString ExportLine;
            const bool bExported = RunTracyTool(TracyToolPath("tracy-csvexport"), Lumina::Format("-u -s \"{}\" \"{}\"", TracySeparator, Out.CaptureFile), [&](FStringView Line)
            {
                ExportLine = FString(Line.GetData(), Line.Len());
                if (bHeader)
                {
                    bHeader = false;
                    return;
                }

                const TVector<FStringView> Fields = SplitProfilerFields(Line);
                if (Fields.Num() < 6)
                {
                    return;
                }

                const FString Name(Fields[0].GetData(), Fields[0].Len());
                auto Found = NameIds.Find(Name);
                if (Found == nullptr)
                {
                    Found = &NameIds.FindOrAdd(Name, Names.Num());
                    Names.Add(Name);
                }

                FProfilerZoneEvent& Event = Events.EmplaceGetRef();
                Event.Name = (*Found);
                Event.Start = (int64)ProfilerNumber(Fields[3]);
                Event.Duration = (int64)ProfilerNumber(Fields[4]);
                Event.Thread = (int32)ProfilerNumber(Fields[5]);
            }, TracyUnwrapTimeoutMs);
            if (!bExported)
            {
                return TracyExportFailed(Out.CaptureFile, ExportLine);
            }

            const auto FrameName = NameIds.Find(FString(TracyFrameZone));
            if (FrameName == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("The trace at {} has no {} zones to find frames by.", Out.CaptureFile, TracyFrameZone));
            }

            // Parents sort before the children they contain, which is what the self-time pass below walks.
            Algo::Sort(Events, [](const FProfilerZoneEvent& A, const FProfilerZoneEvent& B)
            {
                if (A.Thread != B.Thread) { return A.Thread < B.Thread; }
                if (A.Start != B.Start) { return A.Start < B.Start; }
                return A.Duration > B.Duration;
            });

            TVector<int32> Open;
            for (int32 Index = 0; Index < Events.Num(); ++Index)
            {
                FProfilerZoneEvent& Event = Events[Index];
                Event.Self = Event.Duration;
                while (!Open.IsEmpty())
                {
                    const FProfilerZoneEvent& Parent = Events[Open.Last()];
                    if (Parent.Thread == Event.Thread && Event.Start < Parent.Start + Parent.Duration)
                    {
                        break;
                    }
                    Open.Pop();
                }
                if (!Open.IsEmpty())
                {
                    Events[Open.Last()].Self -= Event.Duration;
                }
                Open.Add(Index);
            }

            TVector<int32> Frames;
            int64 FirstStart = INT64_MAX;
            for (int32 Index = 0; Index < Events.Num(); ++Index)
            {
                FirstStart = Math::Min(FirstStart, Events[Index].Start);
                if (Events[Index].Name == (*FrameName))
                {
                    Frames.Add(Index);
                }
            }
            if (Frames.IsEmpty())
            {
                return Agent::FToolResult::Error("The trace recorded no whole frames.");
            }

            // A frame lasts until the next one starts, so a stall on the GPU fence outside the frame zone still counts.
            for (int32 Slot = 0; Slot + 1 < Frames.Num(); ++Slot)
            {
                Events[Frames[Slot]].Duration = Events[Frames[Slot + 1]].Start - Events[Frames[Slot]].Start;
            }

            TVector<float> Sorted;
            for (int32 Index : Frames)
            {
                Sorted.Add((float)(Events[Index].Duration / 1.0e6));
            }
            Algo::Sort(Sorted, [](float A, float B) { return A < B; });
            Out.Frames = Sorted.Num();
            Out.MedianFrameMs = Sorted[Sorted.Num() / 2];
            Out.P99FrameMs = Sorted[Math::Min(Sorted.Num() - 1, static_cast<int32>((float)Sorted.Num() * 0.99f))];
            Out.ThresholdMs = In.ThresholdMs > 0.0f ? In.ThresholdMs : Math::Max(Out.MedianFrameMs * 2.0f, Out.MedianFrameMs + 8.0f);

            TVector<int32> Slow;
            for (int32 Index : Frames)
            {
                if ((float)(Events[Index].Duration / 1.0e6) > Out.ThresholdMs)
                {
                    Slow.Add(Index);
                }
            }
            Out.HitchCount = Slow.Num();
            Algo::Sort(Slow, [&Events](int32 A, int32 B) { return Events[A].Duration > Events[B].Duration; });
            if (Slow.Num() > Math::Max(In.MaxHitches, 0))
            {
                Slow.SetNum(Math::Max(In.MaxHitches, 0));
            }

            const int32 Top = Math::Clamp(In.TopZones, 1, 50);
            for (int32 FrameIndex : Slow)
            {
                const FProfilerZoneEvent& Frame = Events[FrameIndex];
                const int64 End = Frame.Start + Frame.Duration;
                THashMap<int32, SProfilerHitchZone> Game;
                THashMap<int32, SProfilerHitchZone> Workers;
                for (const FProfilerZoneEvent& Event : Events)
                {
                    if (Event.Start < Frame.Start || Event.Start >= End || Event.Self <= 0)
                    {
                        continue;
                    }
                    THashMap<int32, SProfilerHitchZone>& Into = Event.Thread == Frame.Thread ? Game : Workers;
                    SProfilerHitchZone& Zone = Into.FindOrAdd(Event.Name);
                    Zone.Name = Names[Event.Name];
                    Zone.SelfMs += (float)(Event.Self / 1.0e6);
                    Zone.Count += 1;
                }

                SProfilerHitch& Hitch = Out.Hitches.EmplaceGetRef();
                Hitch.AtSeconds = (float)((Frame.Start - FirstStart) / 1.0e9);
                Hitch.FrameMs = (float)(Frame.Duration / 1.0e6);
                RankHitchZones(Game, Top, Hitch.GameThread);
                RankHitchZones(Workers, Top, Hitch.Workers);
            }

            return Agent::FToolResult::Ok(Lumina::Format(
                "{} frames, median {:.2f} ms, p99 {:.2f} ms. {} over {:.1f} ms{}.",
                Out.Frames, Out.MedianFrameMs, Out.P99FrameMs, Out.HitchCount, Out.ThresholdMs,
                Out.Hitches.IsEmpty() ? FString() : Lumina::Format(", the worst {:.1f} ms", Out.Hitches[0].FrameMs)));
        }

        Agent::FToolResult CaptureProfile(const SProfilerCaptureParams& In, SProfilerCaptureResult& Out)
        {
            if (!In.bWait)
            {
                FString Error;
                if (!StartProfilerCapture(In.Seconds, In.bUncapFrameRate, Out.CaptureFile, Error))
                {
                    return Agent::FToolResult::Error(Error);
                }
                return Agent::FToolResult::Ok(Lumina::Format("Recording to {}.", Out.CaptureFile));
            }

            // Tracy occasionally writes a trace its own tools cannot read, so record again.
            constexpr int32 MaxAttempts = 3;
            for (int32 Attempt = 1; ; ++Attempt)
            {
                Out = SProfilerCaptureResult{};
                FString Error;
                if (!StartProfilerCapture(In.Seconds, In.bUncapFrameRate, Out.CaptureFile, Error))
                {
                    return Agent::FToolResult::Error(Error);
                }

                Agent::FToolResult Result = FinishProfilerCapture(In.Top, In.Filter, In.bSelfTime, Out);
                if (Out.Frames > 0 || Attempt == MaxAttempts)
                {
                    return Result;
                }
                LOG_WARN("profiler.capture: {} could not be read back, recording again.", Out.CaptureFile);
            }
        }

        void RegisterProfilerCapture(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SProfilerCaptureParams, SProfilerCaptureResult>(
                Owner, "profiler.capture",
                "Record a Tracy capture of the running editor and report frame time and the costliest CPU zones and GPU passes "
                "per frame. Other calls wait while it records unless bWait is false.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any, &CaptureProfile);
        }

        void RegisterProfilerReport(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SProfilerReportParams, SProfilerReportResult>(
                Owner, "profiler.report",
                "Zones waits for the capture profiler.capture started with bWait false and ranks its zones. Hitches breaks the "
                "long frames of a trace down by zone, on the game thread and the workers.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
                [](const SProfilerReportParams& In, SProfilerReportResult& Out)
                {
                    return In.Mode == EProfilerReportMode::Hitches
                        ? FindProfilerHitches(In, Out.Hitches)
                        : FinishProfilerCapture(In.Top, In.Filter, In.bSelfTime, Out.Zones);
                });
        }
    }

    using namespace MCPProfilerToolsInternal;

    void RegisterProfilerTools(FStringView Owner)
    {
        RegisterProfilerCapture(Owner);
        RegisterProfilerReport(Owner);
    }
}
