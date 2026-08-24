#pragma once

#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    /** Which layer measured the entry, since one request can also produce a package read beneath it. */
    enum class EAssetLoadSource : uint8
    {
        Request,   // FAssetManager, what gameplay actually asked for
        Package,   // CPackage::LoadPackage, what the disk actually did
    };

    /** How a request was satisfied. Only Loaded did real work. */
    enum class EAssetLoadOutcome : uint8
    {
        Loaded,             // read and deserialized by this caller
        AlreadyResident,    // already in memory, so the request cost a lookup
        Joined,             // attached to a load another thread had in flight
        Failed,
    };
}

#if USING(WITH_EDITOR)

#include "Containers/Name.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "ModuleAPI.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    RUNTIME_API const char* LexAssetLoadOutcome(EAssetLoadOutcome Outcome);

    /** One load request and what it cost the caller. */
    struct FAssetLoadRecord
    {
        FName             Name;
        EAssetLoadSource  Source      = EAssetLoadSource::Request;
        EAssetLoadOutcome Outcome     = EAssetLoadOutcome::Loaded;
        double            DurationMs  = 0.0;
        double            CompletedAt = 0.0;   // seconds since process start, for ordering the recent list
        uint64            Bytes       = 0;
        uint32            Exports     = 0;
        uint32            Imports     = 0;
        uint32            ThreadId    = 0;

        NODISCARD bool DidWork() const { return Outcome == EAssetLoadOutcome::Loaded; }
    };

    /** Rolled-up cost for one name across every request for it this session. */
    struct FAssetLoadStat
    {
        FName  Name;
        double TotalMs      = 0.0;
        double LastMs       = 0.0;
        double MinMs        = 0.0;
        double MaxMs        = 0.0;
        uint64 Bytes        = 0;
        uint32 Exports      = 0;
        uint32 Imports      = 0;
        uint32 Count        = 0;
        uint32 LoadedCount  = 0;
        uint32 ResidentHits = 0;
        uint32 JoinedCount  = 0;
        uint32 Failures     = 0;

        NODISCARD double AverageMs() const { return Count > 0 ? TotalMs / (double)Count : 0.0; }
    };

    /** Editor-only history of asset load requests and what each one cost. */
    class RUNTIME_API FAssetLoadTracker
    {
    public:

        static constexpr uint32 MaxRecentRecords = 512;

        static FAssetLoadTracker& Get();

        FAssetLoadTracker() = default;
        FAssetLoadTracker(const FAssetLoadTracker&) = delete;
        FAssetLoadTracker& operator = (const FAssetLoadTracker&) = delete;

        /** Loads run on whichever thread asked, so this is callable from any of them. */
        void Record(const FAssetLoadRecord& Record);

        void Clear();

        /** Copies every view under one lock, so the tables always describe the same moment. */
        void Snapshot(TVector<FAssetLoadRecord>& OutRecent,
                      TVector<FAssetLoadStat>& OutRequests,
                      TVector<FAssetLoadStat>& OutPackages) const;

        NODISCARD uint32 GetTotalRequests() const;
        NODISCARD uint32 GetResidentHits() const;
        NODISCARD double GetTotalRequestMs() const;

    private:

        mutable FMutex                  Mutex;
        TVector<FAssetLoadRecord>       Recent;
        // Kept apart rather than keyed together, so a request and the package under it never sum into one row.
        THashMap<FName, FAssetLoadStat> ByRequest;
        THashMap<FName, FAssetLoadStat> ByPackage;
        uint32                          TotalRequests = 0;
        uint32                          ResidentHits  = 0;
        double                          TotalRequestMs = 0.0;
    };
}

#endif
