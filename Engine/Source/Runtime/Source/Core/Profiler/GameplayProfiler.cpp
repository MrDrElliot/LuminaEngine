#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "GameplayProfiler.h"

#include "Platform/Process/PlatformProcess.h"
#include "tracy/TracyC.h"

namespace Lumina
{
    namespace
    {
        double GameplayProfilerNowMs()
        {
            return PlatformTime::Seconds() * 1000.0;
        }

        uint64 HashName(FStringView S)
        {
            uint64 H = 1469598103934665603ull;
            for (char C : S)
            {
                H ^= static_cast<uint8>(C);
                H *= 1099511628211ull;
            }
            return H;
        }

        // A scope opens and closes on one thread, so only the shared per-frame aggregation is guarded.
        struct FOpenScope
        {
            FFixedString Name;
            uint64       Hash    = 0;
            double       StartMs = 0.0;
            double       ChildMs = 0.0;
        };

        thread_local TVector<FOpenScope> GOpenStack;
    }

    FGameplayProfiler& FGameplayProfiler::Get()
    {
        static FGameplayProfiler Instance;
        return Instance;
    }

    void FGameplayProfiler::SetEnabled(bool bInEnabled)
    {
        const bool bWas = bEnabled.exchange(bInEnabled, std::memory_order_relaxed);
        if (bInEnabled)
        {
            bScriptScopesActive.store(true, std::memory_order_relaxed);
        }
        if (bInEnabled && !bWas)
        {
            // Fresh start so a stale partial frame can't leak in.
            FScopeLock Lock(Mutex);
            GOpenStack.Reset();
            IndexOf.Reset();
            Current = FGameplayProfileFrame{};
        }
    }

    // Fills to HistorySize, then overwrites the oldest sample and advances Offset onto the next oldest.
    static void PushHistory(FProfilerHistory& History, float Value)
    {
        if (static_cast<uint32>(History.Values.Num()) < FGameplayProfiler::HistorySize)
        {
            History.Values.Add(Value);
            return;
        }

        History.Values[History.Offset] = Value;
        History.Offset = (History.Offset + 1u) % FGameplayProfiler::HistorySize;
    }

    void FGameplayProfiler::BeginFrame()
    {
        bScriptScopesActive.store(IsEnabled() || TracyCIsConnected, std::memory_order_relaxed);

        if (!IsEnabled())
        {
            return;
        }
        FScopeLock Lock(Mutex);
        Current.Entries.Reset();
        IndexOf.Reset();
        GOpenStack.Reset();
        Current.TotalMs = 0.0;
        Current.FrameNumber = ++FrameCounter;
    }

    void FGameplayProfiler::EndFrame()
    {
        if (!IsEnabled())
        {
            return;
        }

        FScopeLock Lock(Mutex);
        double Total = 0.0;
        for (const FGameplayProfileEntry& E : Current.Entries)
        {
            Total += E.ExclusiveMs;
        }
        Current.TotalMs = Total;

        // Frame-total history ring (for the header graph).
        PushHistory(FrameTotalHistory, static_cast<float>(Total));

        // Every known scope advances each frame, so the rings stay aligned for a stable sparkline.
        for (auto& [Hash, History] : EntryHistory)
        {
            const auto It = IndexOf.Find(Hash);
            const float Value = (It != nullptr) ? static_cast<float>(Current.Entries[*It].InclusiveMs) : 0.0f;
            PushHistory(History, Value);
        }
        for (const FGameplayProfileEntry& E : Current.Entries)
        {
            if (!EntryHistory.Contains(E.Hash))
            {
                PushHistory(EntryHistory.Add(E.Hash), static_cast<float>(E.InclusiveMs));
            }
        }

        // Swapped rather than copied; BeginFrame clears whatever Latest hands back.
        Latest.TotalMs     = Current.TotalMs;
        Latest.FrameNumber = Current.FrameNumber;
        swap(Latest.Entries, Current.Entries);
    }

    void FGameplayProfiler::BeginScope(FStringView Name)
    {
        if (!IsEnabled())
        {
            return;
        }
        FOpenScope Open;
        Open.Name    = FFixedString(Name.GetData(), Name.Len());
        Open.Hash    = HashName(Name);
        Open.StartMs = GameplayProfilerNowMs();
        Open.ChildMs = 0.0;
        GOpenStack.Add(std::move(Open));   // thread-local; no lock
    }

    void FGameplayProfiler::EndScope()
    {
        if (!IsEnabled() || GOpenStack.IsEmpty())
        {
            return;
        }

        FOpenScope Open = std::move(GOpenStack.Last());
        GOpenStack.Pop();

        const double Duration = GameplayProfilerNowMs() - Open.StartMs;
        const double Self     = Duration - Open.ChildMs;

        // Aggregate into the shared per-frame table (parallel systems may close scopes concurrently).
        {
            FScopeLock Lock(Mutex);

            int32 Index;
            const auto It = IndexOf.Find(Open.Hash);
            if (It != nullptr)
            {
                Index = (*It);
            }
            else
            {
                Index = Current.Entries.Num();
                FGameplayProfileEntry Entry;
                Entry.Name = Open.Name;
                Entry.Hash = Open.Hash;
                Current.Entries.Add(std::move(Entry));
                IndexOf[Open.Hash] = Index;
            }

            FGameplayProfileEntry& Entry = Current.Entries[Index];
            Entry.Calls++;
            Entry.InclusiveMs += Duration;
            Entry.ExclusiveMs += Self;
        }

        // Charge this scope's time to its parent (thread-local nesting; no lock).
        if (!GOpenStack.IsEmpty())
        {
            GOpenStack.Last().ChildMs += Duration;
        }
    }

    const FProfilerHistory* FGameplayProfiler::GetEntryHistory(uint64 Hash) const
    {
        const auto It = EntryHistory.Find(Hash);
        return It != nullptr ? &(*It) : nullptr;
    }

    FGameplayProfileScopeRAII::FGameplayProfileScopeRAII(FStringView Name)
    {
        bActive = FGameplayProfiler::Get().IsEnabled();
        if (bActive)
        {
            FGameplayProfiler::Get().BeginScope(Name);
        }
    }

    FGameplayProfileScopeRAII::~FGameplayProfileScopeRAII()
    {
        if (bActive)
        {
            FGameplayProfiler::Get().EndScope();
        }
    }
}
