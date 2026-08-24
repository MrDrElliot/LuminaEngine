#include "RuntimePCH.h"
#include "AssetLoadTracker.h"

#if USING(WITH_EDITOR)

namespace Lumina
{
    const char* LexAssetLoadOutcome(EAssetLoadOutcome Outcome)
    {
        switch (Outcome)
        {
        case EAssetLoadOutcome::Loaded:          return "loaded";
        case EAssetLoadOutcome::AlreadyResident: return "resident";
        case EAssetLoadOutcome::Joined:          return "joined";
        case EAssetLoadOutcome::Failed:          return "failed";
        }
        return "?";
    }

    namespace
    {
        void Accumulate(FAssetLoadStat& Stat, const FAssetLoadRecord& Record)
        {
            if (Stat.Count == 0)
            {
                Stat.Name  = Record.Name;
                Stat.MinMs = Record.DurationMs;
                Stat.MaxMs = Record.DurationMs;
            }
            else
            {
                Stat.MinMs = Math::Min(Stat.MinMs, Record.DurationMs);
                Stat.MaxMs = Math::Max(Stat.MaxMs, Record.DurationMs);
            }

            ++Stat.Count;
            Stat.TotalMs += Record.DurationMs;
            Stat.LastMs   = Record.DurationMs;

            // A resident hit or a join reads nothing, so keeping the loader's figures avoids reporting zero.
            if (Record.Bytes > 0)
            {
                Stat.Bytes   = Record.Bytes;
                Stat.Exports = Record.Exports;
                Stat.Imports = Record.Imports;
            }

            switch (Record.Outcome)
            {
            case EAssetLoadOutcome::Loaded:          ++Stat.LoadedCount;  break;
            case EAssetLoadOutcome::AlreadyResident: ++Stat.ResidentHits; break;
            case EAssetLoadOutcome::Joined:          ++Stat.JoinedCount;  break;
            case EAssetLoadOutcome::Failed:          ++Stat.Failures;     break;
            }
        }
    }

    FAssetLoadTracker& FAssetLoadTracker::Get()
    {
        static FAssetLoadTracker Instance;
        return Instance;
    }

    void FAssetLoadTracker::Record(const FAssetLoadRecord& Record)
    {
        FScopeLock Lock(Mutex);

        // Oldest out first, so the list stays the most recent MaxRecentRecords entries.
        if (Recent.size() >= MaxRecentRecords)
        {
            Recent.erase(Recent.begin());
        }
        Recent.push_back(Record);

        const bool bRequest = Record.Source == EAssetLoadSource::Request;
        Accumulate(bRequest ? ByRequest[Record.Name] : ByPackage[Record.Name], Record);

        // Only the request layer counts toward the session totals, or every load would be counted twice.
        if (bRequest)
        {
            ++TotalRequests;
            TotalRequestMs += Record.DurationMs;
            ResidentHits += (Record.Outcome == EAssetLoadOutcome::AlreadyResident) ? 1 : 0;
        }
    }

    void FAssetLoadTracker::Clear()
    {
        FScopeLock Lock(Mutex);

        Recent.clear();
        ByRequest.clear();
        ByPackage.clear();
        TotalRequests  = 0;
        ResidentHits   = 0;
        TotalRequestMs = 0.0;
    }

    void FAssetLoadTracker::Snapshot(TVector<FAssetLoadRecord>& OutRecent,
                                     TVector<FAssetLoadStat>& OutRequests,
                                     TVector<FAssetLoadStat>& OutPackages) const
    {
        FScopeLock Lock(Mutex);

        OutRecent.assign(Recent.begin(), Recent.end());

        auto Flatten = [](const THashMap<FName, FAssetLoadStat>& In, TVector<FAssetLoadStat>& Out)
        {
            Out.clear();
            Out.reserve(In.size());
            for (const auto& Pair : In)
            {
                Out.push_back(Pair.second);
            }
        };

        Flatten(ByRequest, OutRequests);
        Flatten(ByPackage, OutPackages);
    }

    uint32 FAssetLoadTracker::GetTotalRequests() const
    {
        FScopeLock Lock(Mutex);
        return TotalRequests;
    }

    uint32 FAssetLoadTracker::GetResidentHits() const
    {
        FScopeLock Lock(Mutex);
        return ResidentHits;
    }

    double FAssetLoadTracker::GetTotalRequestMs() const
    {
        FScopeLock Lock(Mutex);
        return TotalRequestMs;
    }
}

#endif
