#include "MCPProfilerTools.h"

#include "Agent/AgentGameThread.h"
#include "Agent/AgentToolRegistry.h"
#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Math.h"
#include "MCPTextMatch.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"

#include <cstdlib>

namespace Lumina::MCP
{
    namespace
    {
        constexpr float MaxCaptureSeconds = 60.0f;
        constexpr char Separator = ';';
        constexpr const char* FrameZone = "Lumina::FEngine::Update";
        constexpr const char* LimiterZone = "Frame-Rate-Limiter";
        constexpr const char* GpuWaitZone = "Frame Fence (GPU)";

        struct FZoneTotals
        {
            double TotalNs = 0.0;
            double MaxNs = 0.0;
            int64 Count = 0;
        };

        TVector<FStringView> SplitFields(FStringView Line)
        {
            TVector<FStringView> Fields;
            size_t Start = 0;
            while (Start <= Line.size())
            {
                const size_t End = Line.find(Separator, Start);
                Fields.push_back(Line.substr(Start, End == FStringView::npos ? FStringView::npos : End - Start));
                if (End == FStringView::npos)
                {
                    break;
                }
                Start = End + 1;
            }
            return Fields;
        }

        double ToNumber(FStringView Field)
        {
            const FString Text(Field.data(), Field.size());
            return std::strtod(Text.c_str(), nullptr);
        }

        // Tracy's CLI tools ship beside the engine rather than inside it.
        FString TracyTool(const char* Name)
        {
            return Paths::GetEngineInstallDirectory() + "/External/Tracy/" + Name + ".exe";
        }

        // Bounded, since a tracy-capture left connected by an earlier run would otherwise hold the tool call forever.
        constexpr uint32 ExportTimeoutMilliseconds = 120000;

        bool RunTool(const FString& Executable, const FString& Arguments, const TFunction<void(FStringView)>& OnLine,
                     uint32 TimeoutMilliseconds = ExportTimeoutMilliseconds)
        {
            const int ExitCode = Platform::RunProcessAndWaitCapture(UTF8_TO_TCHAR(Executable.c_str()), UTF8_TO_TCHAR(Arguments.c_str()),
                                                                    nullptr, OnLine, TimeoutMilliseconds);
            if (ExitCode == Platform::ProcessTimedOutExitCode)
            {
                OnLine(FStringView("error: timed out, so it was stopped"));
            }
            return ExitCode == 0;
        }

        // Rows repeat a name when several source sites share it, so totals are summed by name.
        void ReadZones(const FString& Executable, const FString& Arguments, bool bGpuRows, THashMap<FString, FZoneTotals>& Out)
        {
            bool bHeader = true;
            RunTool(Executable, Arguments, [&](FStringView Line)
            {
                if (bHeader)
                {
                    bHeader = false;
                    return;
                }

                const TVector<FStringView> Fields = SplitFields(Line);
                FZoneTotals& Totals = Out[FString(Fields[0].data(), Fields[0].size())];
                if (bGpuRows && Fields.size() >= 4)
                {
                    const double Ns = ToNumber(Fields[3]);
                    Totals.TotalNs += Ns;
                    Totals.MaxNs = Math::Max(Totals.MaxNs, Ns);
                    Totals.Count += 1;
                }
                else if (!bGpuRows && Fields.size() >= 9)
                {
                    Totals.TotalNs += ToNumber(Fields[3]);
                    Totals.Count += (int64)ToNumber(Fields[5]);
                    Totals.MaxNs = Math::Max(Totals.MaxNs, ToNumber(Fields[8]));
                }
            });
        }

        TVector<SProfilerZone> Rank(const THashMap<FString, FZoneTotals>& Zones, int32 Frames, int32 Top, const FString& Filter)
        {
            TVector<SProfilerZone> Ranked;
            for (const auto& [Name, Totals] : Zones)
            {
                if (Totals.Count == 0 || !ContainsTextFold(Name, Filter))
                {
                    continue;
                }

                SProfilerZone& Zone = Ranked.emplace_back();
                Zone.Name = Name;
                Zone.Count = (int32)Totals.Count;
                Zone.MsPerFrame = (float)(Totals.TotalNs / 1.0e6 / Math::Max(Frames, 1));
                Zone.MeanUs = (float)(Totals.TotalNs / 1.0e3 / (double)Totals.Count);
                Zone.MaxUs = (float)(Totals.MaxNs / 1.0e3);
            }

            Algo::Sort(Ranked, [](const SProfilerZone& A, const SProfilerZone& B) { return A.MsPerFrame > B.MsPerFrame; });
            if ((int32)Ranked.size() > Top)
            {
                Ranked.resize(Top);
            }
            return Ranked;
        }

        float MsPerFrame(const THashMap<FString, FZoneTotals>& Zones, const char* Name, int32 Frames)
        {
            const auto Found = Zones.find(FString(Name));
            return Found == Zones.end() ? 0.0f : (float)(Found->second.TotalNs / 1.0e6 / Math::Max(Frames, 1));
        }
            // One capture at a time runs on its own thread, so the scene it measures can still be driven through other tools.
        struct FCaptureJob
        {
            FString File;
            FString Failure;
            int32 Seconds = 0;
            bool bCaptured = false;
            FThread Worker;
        };

        FMutex JobMutex;
        TUniquePtr<FCaptureJob> Job;
        FString LastCaptureFile;

        bool StartCapture(float Seconds, bool bUncapFrameRate, FString& OutFile, FString& OutError)
        {
            FScopeLock Lock(JobMutex);
            if (Job)
            {
                OutError = Lumina::Format("A capture to {} has not been reported yet; call profiler.report first.", Job->File);
                return false;
            }

            FString Directory = Paths::GetEngineDirectory() + "/Saved/Profiling";
            Paths::CreateDirectories(FStringView(Directory.c_str(), Directory.size()));

            Job = MakeUnique<FCaptureJob>();
            Job->File = Lumina::Format("{}/Capture_{}.tracy", Directory, (uint64)(PlatformTime::Seconds() * 1000.0));
            OutFile = Job->File;

            FCaptureJob* Running = Job.get();
            const int32 CaptureSeconds = (int32)Math::Ceil(Math::Clamp(Seconds, 1.0f, MaxCaptureSeconds));
            Running->Seconds = CaptureSeconds;
            Running->Worker = FThread([Running, CaptureSeconds, bUncapFrameRate]()
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

                const bool bRan = RunTool(TracyTool("tracy-capture"),
                    Lumina::Format("-o \"{}\" -a 127.0.0.1 -f -s {}", Running->File, CaptureSeconds),
                    [Running](FStringView Line)
                    {
                        if (ContainsTextFold(Line, "error") || ContainsTextFold(Line, "cannot"))
                        {
                            Running->Failure.append(Line.data(), Line.size());
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

        Agent::FToolResult FinishCapture(const SProfilerReportParams& In, SProfilerCaptureResult& Out)
        {
            TUniquePtr<FCaptureJob> Finished;
            {
                FScopeLock Lock(JobMutex);
                Finished = Move(Job);
            }
            if (!Finished)
            {
                return Agent::FToolResult::Error("No capture was started; call profiler.start or profiler.capture.");
            }

            if (Finished->Worker.Joinable())
            {
                Finished->Worker.Join();
            }

            Out.CaptureFile = Finished->File;
            if (Finished->bCaptured)
            {
                FScopeLock Lock(JobMutex);
                LastCaptureFile = Finished->File;
            }
            if (!Finished->bCaptured)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "tracy-capture did not produce a trace ({}). Another Tracy client may already be connected.",
                    Finished->Failure.empty() ? FString("no output") : Finished->Failure));
            }

            const int32 Top = Math::Clamp(In.Top, 1, 200);
            const FString Export = TracyTool("tracy-csvexport");
            THashMap<FString, FZoneTotals> Cpu;
            THashMap<FString, FZoneTotals> Gpu;
            ReadZones(Export, Lumina::Format("-s \"{}\" {}\"{}\"", Separator, In.bSelfTime ? "-e " : "", Out.CaptureFile), false, Cpu);
            ReadZones(Export, Lumina::Format("-s \"{}\" -u -g \"{}\"", Separator, Out.CaptureFile), true, Gpu);

            // A frame's self time is only what its children leave over, so frame totals always come from inclusive times.
            THashMap<FString, FZoneTotals> Inclusive;
            if (In.bSelfTime)
            {
                ReadZones(Export, Lumina::Format("-s \"{}\" \"{}\"", Separator, Out.CaptureFile), false, Inclusive);
            }
            const THashMap<FString, FZoneTotals>& Totals = In.bSelfTime ? Inclusive : Cpu;

            const auto Frame = Totals.find(FString(FrameZone));
            if (Frame == Totals.end() || Frame->second.Count == 0)
            {
                return Agent::FToolResult::Error(Lumina::Format("The trace at {} has no {} zones to count frames by.", Out.CaptureFile, FrameZone));
            }

            Out.Frames = (int32)Frame->second.Count;
            Out.FrameMs = (float)(Frame->second.TotalNs / 1.0e6 / Out.Frames);
            Out.MaxFrameMs = (float)(Frame->second.MaxNs / 1.0e6);
            Out.LimiterMs = MsPerFrame(Totals, LimiterZone, Out.Frames);
            Out.GpuWaitMs = MsPerFrame(Totals, GpuWaitZone, Out.Frames);
            Out.PeriodMs = (float)(Finished->Seconds * 1000.0 / Out.Frames);
            Out.Cpu = Rank(Cpu, Out.Frames, Top, In.Filter);
            Out.Gpu = Rank(Gpu, Out.Frames, Top, In.Filter);

            return Agent::FToolResult::Ok(Lumina::Format(
                "{} frames, {:.2f} ms apart ({:.0f} fps). The frame zone took {:.2f} ms ({:.2f} ms max), with {:.2f} ms more waiting "
                "on the GPU and {:.2f} ms in the frame limiter per frame. Trace saved to {}.",
                Out.Frames, Out.PeriodMs, 1000.0f / Math::Max(Out.PeriodMs, 0.001f), Out.FrameMs, Out.MaxFrameMs, Out.GpuWaitMs,
                Out.LimiterMs, Out.CaptureFile));
        }
    }

    namespace
    {
        // One zone event from an unwrapped export, with its name interned.
        struct FZoneEvent
        {
            int64 Start = 0;
            int64 Duration = 0;
            int64 Self = 0;
            int32 Name = 0;
            int32 Thread = 0;
        };

        void RankInto(const THashMap<int32, SProfilerHitchZone>& Totals, int32 Top, TVector<SProfilerHitchZone>& Out)
        {
            for (const auto& [Id, Zone] : Totals)
            {
                Out.push_back(Zone);
            }
            Algo::Sort(Out, [](const SProfilerHitchZone& A, const SProfilerHitchZone& B) { return A.SelfMs > B.SelfMs; });
            if ((int32)Out.size() > Top)
            {
                Out.resize(Top);
            }
        }

        Agent::FToolResult FindHitches(const SProfilerHitchParams& In, SProfilerHitchResult& Out)
        {
            Out.CaptureFile = In.CaptureFile;
            if (Out.CaptureFile.empty())
            {
                FScopeLock Lock(JobMutex);
                Out.CaptureFile = LastCaptureFile;
            }
            if (Out.CaptureFile.empty() || !Paths::Exists(Out.CaptureFile))
            {
                return Agent::FToolResult::Error("No trace to read; record one with profiler.capture or pass CaptureFile.");
            }

            THashMap<FString, int32> NameIds;
            TVector<FString> Names;
            TVector<FZoneEvent> Events;
            bool bHeader = true;
            RunTool(TracyTool("tracy-csvexport"), Lumina::Format("-u -s \"{}\" \"{}\"", Separator, Out.CaptureFile), [&](FStringView Line)
            {
                if (bHeader)
                {
                    bHeader = false;
                    return;
                }

                const TVector<FStringView> Fields = SplitFields(Line);
                if (Fields.size() < 6)
                {
                    return;
                }

                const FString Name(Fields[0].data(), Fields[0].size());
                auto Found = NameIds.find(Name);
                if (Found == NameIds.end())
                {
                    Found = NameIds.emplace(Name, (int32)Names.size()).first;
                    Names.push_back(Name);
                }

                FZoneEvent& Event = Events.emplace_back();
                Event.Name = Found->second;
                Event.Start = (int64)ToNumber(Fields[3]);
                Event.Duration = (int64)ToNumber(Fields[4]);
                Event.Thread = (int32)ToNumber(Fields[5]);
            });

            const auto FrameName = NameIds.find(FString(FrameZone));
            if (FrameName == NameIds.end())
            {
                return Agent::FToolResult::Error(Lumina::Format("The trace at {} has no {} zones to find frames by.", Out.CaptureFile, FrameZone));
            }

            // Parents sort before the children they contain, which is what the self-time pass below walks.
            Algo::Sort(Events, [](const FZoneEvent& A, const FZoneEvent& B)
            {
                if (A.Thread != B.Thread) { return A.Thread < B.Thread; }
                if (A.Start != B.Start) { return A.Start < B.Start; }
                return A.Duration > B.Duration;
            });

            TVector<size_t> Open;
            for (size_t Index = 0; Index < Events.size(); ++Index)
            {
                FZoneEvent& Event = Events[Index];
                Event.Self = Event.Duration;
                while (!Open.empty())
                {
                    const FZoneEvent& Parent = Events[Open.back()];
                    if (Parent.Thread == Event.Thread && Event.Start < Parent.Start + Parent.Duration)
                    {
                        break;
                    }
                    Open.pop_back();
                }
                if (!Open.empty())
                {
                    Events[Open.back()].Self -= Event.Duration;
                }
                Open.push_back(Index);
            }

            TVector<size_t> Frames;
            int64 FirstStart = INT64_MAX;
            for (size_t Index = 0; Index < Events.size(); ++Index)
            {
                FirstStart = Math::Min(FirstStart, Events[Index].Start);
                if (Events[Index].Name == FrameName->second)
                {
                    Frames.push_back(Index);
                }
            }
            if (Frames.empty())
            {
                return Agent::FToolResult::Error("The trace recorded no whole frames.");
            }

            // A frame lasts until the next one starts, so a stall on the GPU fence outside the frame zone still counts.
            for (size_t Slot = 0; Slot + 1 < Frames.size(); ++Slot)
            {
                Events[Frames[Slot]].Duration = Events[Frames[Slot + 1]].Start - Events[Frames[Slot]].Start;
            }

            TVector<float> Sorted;
            for (size_t Index : Frames)
            {
                Sorted.push_back((float)(Events[Index].Duration / 1.0e6));
            }
            Algo::Sort(Sorted, [](float A, float B) { return A < B; });
            Out.Frames = (int32)Sorted.size();
            Out.MedianFrameMs = Sorted[Sorted.size() / 2];
            Out.P99FrameMs = Sorted[Math::Min(Sorted.size() - 1, (size_t)((float)Sorted.size() * 0.99f))];
            Out.ThresholdMs = In.ThresholdMs > 0.0f ? In.ThresholdMs : Math::Max(Out.MedianFrameMs * 2.0f, Out.MedianFrameMs + 8.0f);

            TVector<size_t> Slow;
            for (size_t Index : Frames)
            {
                if ((float)(Events[Index].Duration / 1.0e6) > Out.ThresholdMs)
                {
                    Slow.push_back(Index);
                }
            }
            Out.HitchCount = (int32)Slow.size();
            Algo::Sort(Slow, [&Events](size_t A, size_t B) { return Events[A].Duration > Events[B].Duration; });
            if ((int32)Slow.size() > Math::Max(In.MaxHitches, 0))
            {
                Slow.resize(Math::Max(In.MaxHitches, 0));
            }

            const int32 Top = Math::Clamp(In.TopZones, 1, 50);
            for (size_t FrameIndex : Slow)
            {
                const FZoneEvent& Frame = Events[FrameIndex];
                const int64 End = Frame.Start + Frame.Duration;
                THashMap<int32, SProfilerHitchZone> Game;
                THashMap<int32, SProfilerHitchZone> Workers;
                for (const FZoneEvent& Event : Events)
                {
                    if (Event.Start < Frame.Start || Event.Start >= End || Event.Self <= 0)
                    {
                        continue;
                    }
                    THashMap<int32, SProfilerHitchZone>& Into = Event.Thread == Frame.Thread ? Game : Workers;
                    SProfilerHitchZone& Zone = Into[Event.Name];
                    Zone.Name = Names[Event.Name];
                    Zone.SelfMs += (float)(Event.Self / 1.0e6);
                    Zone.Count += 1;
                }

                SProfilerHitch& Hitch = Out.Hitches.emplace_back();
                Hitch.AtSeconds = (float)((Frame.Start - FirstStart) / 1.0e9);
                Hitch.FrameMs = (float)(Frame.Duration / 1.0e6);
                RankInto(Game, Top, Hitch.GameThread);
                RankInto(Workers, Top, Hitch.Workers);
            }

            return Agent::FToolResult::Ok(Lumina::Format(
                "{} frames, median {:.2f} ms, p99 {:.2f} ms. {} over {:.1f} ms{}.",
                Out.Frames, Out.MedianFrameMs, Out.P99FrameMs, Out.HitchCount, Out.ThresholdMs,
                Out.Hitches.empty() ? FString() : Lumina::Format(", the worst {:.1f} ms", Out.Hitches[0].FrameMs)));
        }
    }

    void RegisterProfilerTools(FStringView Owner)
    {
        Agent::FToolRegistry& Registry = Agent::FToolRegistry::Get();

        Registry.Register<SProfilerStartParams, SProfilerStartResult>(
            Owner, "profiler.start",
            "Start recording a Tracy capture of the running editor in the background and return at once, so the scene can be "
            "driven through other tools while it records. Call profiler.report to wait for it and read the results.",
            Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
            [](const SProfilerStartParams& In, SProfilerStartResult& Out)
            {
                FString Error;
                if (!StartCapture(In.Seconds, In.bUncapFrameRate, Out.CaptureFile, Error))
                {
                    return Agent::FToolResult::Error(Error);
                }
                return Agent::FToolResult::Ok(Lumina::Format("Recording to {}.", Out.CaptureFile));
            });

        Registry.Register<SProfilerReportParams, SProfilerCaptureResult>(
            Owner, "profiler.report",
            "Wait for the capture profiler.start began, then report the frame time, the costliest CPU zones and the costliest GPU passes, per frame.",
            Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
            [](const SProfilerReportParams& In, SProfilerCaptureResult& Out)
            {
                return FinishCapture(In, Out);
            });

        Registry.Register<SProfilerHitchParams, SProfilerHitchResult>(
            Owner, "profiler.hitches",
            "Find the frames in a Tracy capture that ran long and break each one down by the zones that spent the time, "
            "on the game thread and on the workers. Reads the last capture unless CaptureFile names another.",
            Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
            [](const SProfilerHitchParams& In, SProfilerHitchResult& Out)
            {
                return FindHitches(In, Out);
            });

        Registry.Register<SProfilerCaptureParams, SProfilerCaptureResult>(
            Owner, "profiler.capture",
            "Record a Tracy capture of the running editor for a few seconds and report the frame time, the costliest CPU zones "
            "and the costliest GPU passes, per frame. Other tool calls wait while it records; use profiler.start and "
            "profiler.report to drive the scene during the capture.",
            Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
            [](const SProfilerCaptureParams& In, SProfilerCaptureResult& Out)
            {
                FString Error;
                if (!StartCapture(In.Seconds, In.bUncapFrameRate, Out.CaptureFile, Error))
                {
                    return Agent::FToolResult::Error(Error);
                }

                SProfilerReportParams Report;
                Report.Top = In.Top;
                Report.Filter = In.Filter;
                Report.bSelfTime = In.bSelfTime;
                return FinishCapture(Report, Out);
            });
    }
}
