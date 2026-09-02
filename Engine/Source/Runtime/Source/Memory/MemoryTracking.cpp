#include "RuntimePCH.h"
#include "MemoryTracking.h"
#include "Memory/Construct.h"

#if LUMINA_MEMORY_TRACKING

#include <rpmalloc.h>
#include <cstring>
#include <cstdio>
#include <new>
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"

#if defined(LE_PLATFORM_WINDOWS)
    #include <Windows.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
#endif

namespace Lumina::Memory
{
    namespace
    {
        // Interned category names; index 0 is always "Default", cached per call site.
        struct FCategory
        {
            char            Name[64];
            uint64          NameHash;
            TAtomic<uint64> LiveBytes;
            TAtomic<uint64> LiveCount;
            TAtomic<uint64> PeakBytes;
            TAtomic<uint64> TotalAllocs;
            TAtomic<uint64> TotalFrees;
        };

        constexpr uint32 kMaxCategories = 256;
        constexpr uint32 kMaxCatDepth   = 32;

        FCategory       gCategories[kMaxCategories];
        FMutex          gCategoryMutex;
        TAtomic<uint32> gCategoryCount;     // number of registered categories

        // One outstanding allocation. Key == nullptr marks an empty slot.
        struct FEntry
        {
            void*  Key     = nullptr;
            size_t Size    = 0;
            uint32 CatId   = 0;
            uint32 StackId = 0;             // 1-based call-site slot, 0 == none
        };
        
        struct CACHE_ALIGN FShard
        {
            FMutex  Mutex;
            FEntry* Entries  = nullptr;
            uint32  Capacity = 0;           // power of two; 0 == not yet allocated
            uint32  Count    = 0;
        };

        constexpr uint32 kNumShards  = 256; // low 8 bits of the hash select the shard
        constexpr uint32 kInitialCap = 256;

        FShard          gShards[kNumShards];
        TAtomic<uint64> gGlobalLiveBytes;
        TAtomic<uint64> gGlobalLiveCount;
        TAtomic<uint64> gOverflowCount;
        
        TAtomic<bool>   gEnabled{ true };

        struct FCallSite
        {
            uint64 Hash;                    // 0 == empty slot
            uint64 LiveBytes;
            uint64 LiveCount;
            uint64 TotalAllocs;
            uint32 CatId;
            uint16 FrameCount;
            void*  Frames[kMaxStackFrames];
        };

        constexpr uint32 kMaxCallSites = 131072;    // power of two; open-addressed by Hash

        TAtomic<bool>   gCaptureStacks;
        TAtomic<uint64> gCallSiteOverflow;          // allocations that found the table full
        FMutex          gCallSiteMutex;
        FCallSite*      gCallSites   = nullptr;     // lazily allocated [kMaxCallSites]

        // Per-thread attribution scope stack and a re-entrancy guard.
        thread_local uint32 tlCategoryStack[kMaxCatDepth];
        thread_local int    tlCategoryDepth = 0;
        thread_local bool   tlInTracker     = false;

        FORCEINLINE uint64 HashString(const char* S)
        {
            uint64 H = 1469598103934665603ull;      // FNV-1a
            for (; *S; ++S)
            {
                H = (H ^ (uint8)*S) * 1099511628211ull;
            }
            return H;
        }

        FORCEINLINE uint32 CurrentCategory()
        {
            const int Depth = tlCategoryDepth;
            return (Depth > 0 && Depth <= (int)kMaxCatDepth) ? tlCategoryStack[Depth - 1] : 0u;
        }

        // Caller must hold gCategoryMutex. Index 0 is reserved for "Default".
        void EnsureDefaultCategory()
        {
            if (gCategoryCount.load(Atomic::MemoryOrderRelaxed) == 0)
            {
                std::strncpy(gCategories[0].Name, "Default", sizeof(gCategories[0].Name) - 1);
                gCategories[0].NameHash = HashString("Default");
                gCategoryCount.store(1, Atomic::MemoryOrderRelease);
            }
        }

        // Spreads pointer bits so the low byte and the high bits are both well mixed.
        FORCEINLINE uint64 MixPtr(const void* P)
        {
            uint64 X = (uint64)reinterpret_cast<uintptr_t>(P);
            X ^= X >> 33; X *= 0xff51afd7ed558ccdULL;
            X ^= X >> 33; X *= 0xc4ceb9fe1a85ec53ULL;
            X ^= X >> 33;
            return X;
        }

        FORCEINLINE void UpdatePeak(TAtomic<uint64>& Peak, uint64 Candidate)
        {
            uint64 Prev = Peak.load(Atomic::MemoryOrderRelaxed);
            while (Candidate > Prev && !Peak.compare_exchange_weak(Prev, Candidate, Atomic::MemoryOrderRelaxed))
            {
            }
        }

        // Insert into a raw table (no growth). Returns false only when full.
        bool TableInsert(FEntry* Entries, uint32 Cap, void* Key, uint64 Hash, size_t Size, uint32 CatId, uint32 StackId, uint32& InOutCount)
        {
            const uint32 Mask = Cap - 1;
            const uint32 Slot = (uint32)(Hash >> 8) & Mask;
            for (uint32 Probe = 0; Probe < Cap; ++Probe)
            {
                FEntry& E = Entries[(Slot + Probe) & Mask];
                if (E.Key == nullptr)
                {
                    E.Key = Key; E.Size = Size; E.CatId = CatId; E.StackId = StackId;
                    ++InOutCount;
                    return true;
                }
                if (E.Key == Key)
                {
                    E.Size = Size; E.CatId = CatId; E.StackId = StackId;   // realloc-in-place
                    return true;
                }
            }
            return false;
        }

        // Grow/allocate a shard's table so one more insert stays under ~0.75 load.
        bool ShardEnsureCapacity(FShard& Shard)
        {
            if (Shard.Entries != nullptr && (uint64)(Shard.Count + 1) * 4 < (uint64)Shard.Capacity * 3)
            {
                return true;
            }

            const uint32 NewCap = (Shard.Capacity == 0) ? kInitialCap : Shard.Capacity * 2;
            FEntry* NewEntries = (FEntry*)rpmalloc(sizeof(FEntry) * NewCap);
            if (NewEntries == nullptr)
            {
                return false;
            }
            // Value-init rather than memset, since FEntry has member initializers and is not trivially default.
            for (uint32 i = 0; i < NewCap; ++i)
            {
                Memory::ConstructAt(NewEntries + i);
            }

            uint32 NewCount = 0;
            if (Shard.Entries != nullptr)
            {
                for (uint32 i = 0; i < Shard.Capacity; ++i)
                {
                    FEntry& E = Shard.Entries[i];
                    if (E.Key != nullptr)
                    {
                        TableInsert(NewEntries, NewCap, E.Key, MixPtr(E.Key), E.Size, E.CatId, E.StackId, NewCount);
                    }
                }
                rpfree(Shard.Entries);
            }

            Shard.Entries  = NewEntries;
            Shard.Capacity = NewCap;
            Shard.Count    = NewCount;
            return true;
        }

        // Linear-probe lookup + backward-shift deletion (no tombstones).
        bool ShardRemove(FShard& Shard, void* Key, uint64 Hash, size_t& OutSize, uint32& OutCatId, uint32& OutStackId)
        {
            if (Shard.Entries == nullptr || Shard.Count == 0)
            {
                return false;
            }

            const uint32 Mask  = Shard.Capacity - 1;
            const uint32 Start = (uint32)(Hash >> 8) & Mask;

            uint32 Found = UINT32_MAX;
            for (uint32 Probe = 0; Probe < Shard.Capacity; ++Probe)
            {
                const uint32 Idx = (Start + Probe) & Mask;
                FEntry& E = Shard.Entries[Idx];
                if (E.Key == nullptr)
                {
                    return false;          // empty slot ends the probe chain
                }
                if (E.Key == Key)
                {
                    Found = Idx; OutSize = E.Size; OutCatId = E.CatId; OutStackId = E.StackId;
                    break;
                }
            }
            if (Found == UINT32_MAX)
            {
                return false;
            }

            uint32 Hole = Found;
            for (;;)
            {
                uint32 j = Hole;
                for (;;)
                {
                    j = (j + 1) & Mask;
                    if (Shard.Entries[j].Key == nullptr)
                    {
                        Shard.Entries[Hole].Key = nullptr;
                        --Shard.Count;
                        return true;
                    }
                    const uint32 Home = (uint32)(MixPtr(Shard.Entries[j].Key) >> 8) & Mask;
                    const bool bInRange = (Hole <= j) ? (Hole < Home && Home <= j)
                                                      : (Hole < Home || Home <= j);
                    if (!bInRange)
                    {
                        break;
                    }
                }
                Shard.Entries[Hole] = Shard.Entries[j];
                Hole = j;
            }
        }

        // Returns a 1-based slot id or 0, and the caller must NOT hold a shard lock.
        uint32 CaptureCallSite(size_t Size, uint32 CatId)
        {
#if defined(LE_PLATFORM_WINDOWS)
            void* Frames[kMaxStackFrames];
            ULONG BackHash = 0;
            // Skip CaptureCallSite + Hooks::OnAlloc + Memory::Malloc so the top frame is the caller.
            const USHORT N = RtlCaptureStackBackTrace(3, (ULONG)kMaxStackFrames, Frames, &BackHash);
            if (N == 0)
            {
                return 0;
            }
            const uint64 H = BackHash ? (uint64)BackHash : 1ull;

            FScopeLock Lock(gCallSiteMutex);
            if (gCallSites == nullptr)
            {
                gCallSites = (FCallSite*)rpmalloc(sizeof(FCallSite) * kMaxCallSites);
                if (gCallSites == nullptr)
                {
                    return 0;
                }
                std::memset(gCallSites, 0, sizeof(FCallSite) * kMaxCallSites);
            }

            const uint32 Mask = kMaxCallSites - 1;
            const uint32 Slot = (uint32)(H ^ (H >> 17)) & Mask;
            for (uint32 Probe = 0; Probe < kMaxCallSites; ++Probe)
            {
                FCallSite& C = gCallSites[(Slot + Probe) & Mask];
                if (C.Hash == 0)
                {
                    C.Hash = H; C.CatId = CatId; C.FrameCount = N;
                    std::memcpy(C.Frames, Frames, sizeof(void*) * N);
                    C.LiveBytes = Size; C.LiveCount = 1; C.TotalAllocs = 1;
                    return ((Slot + Probe) & Mask) + 1;
                }
                if (C.Hash == H)
                {
                    C.LiveBytes += Size; C.LiveCount += 1; C.TotalAllocs += 1;
                    return ((Slot + Probe) & Mask) + 1;
                }
            }
            gCallSiteOverflow.fetch_add(1, Atomic::MemoryOrderRelaxed);
            return 0;
#else
            (void)Size; (void)CatId;
            return 0;
#endif
        }

        void ReleaseCallSite(uint32 StackId, size_t Size)
        {
            if (StackId == 0)
            {
                return;
            }
            FScopeLock Lock(gCallSiteMutex);
            if (gCallSites == nullptr)
            {
                return;
            }
            FCallSite& C = gCallSites[StackId - 1];
            C.LiveBytes = (C.LiveBytes >= Size) ? (C.LiveBytes - Size) : 0;
            if (C.LiveCount > 0) { --C.LiveCount; }
        }
    }

    uint32 RegisterCategory(const char* Name)
    {
        if (Name == nullptr || Name[0] == '\0')
        {
            return 0;
        }
        const uint64 Hash = HashString(Name);

        FScopeLock Lock(gCategoryMutex);
        EnsureDefaultCategory();

        const uint32 Count = gCategoryCount.load(Atomic::MemoryOrderRelaxed);
        for (uint32 i = 0; i < Count; ++i)
        {
            if (gCategories[i].NameHash == Hash && std::strcmp(gCategories[i].Name, Name) == 0)
            {
                return i;
            }
        }
        if (Count >= kMaxCategories)
        {
            return 0;       // out of category slots; fall back to Default
        }

        std::strncpy(gCategories[Count].Name, Name, sizeof(gCategories[Count].Name) - 1);
        gCategories[Count].Name[sizeof(gCategories[Count].Name) - 1] = '\0';
        gCategories[Count].NameHash = Hash;
        gCategoryCount.store(Count + 1, Atomic::MemoryOrderRelease);
        return Count;
    }

    void SetTrackingEnabled(bool bEnabled) { gEnabled.store(bEnabled, Atomic::MemoryOrderRelease); }
    bool IsTrackingEnabled()               { return gEnabled.load(Atomic::MemoryOrderAcquire); }

    void ResetTracking()
    {
        for (FShard& Shard : gShards)
        {
            FScopeLock Lock(Shard.Mutex);
            if (Shard.Entries != nullptr)
            {
                rpfree(Shard.Entries);
                Shard.Entries = nullptr;
            }
            Shard.Capacity = 0;
            Shard.Count    = 0;
        }

        // Keep category registrations (their ids are cached at call sites); zero counters only.
        const uint32 Count = gCategoryCount.load(Atomic::MemoryOrderAcquire);
        for (uint32 i = 0; i < Count; ++i)
        {
            gCategories[i].LiveBytes.store(0, Atomic::MemoryOrderRelaxed);
            gCategories[i].LiveCount.store(0, Atomic::MemoryOrderRelaxed);
            gCategories[i].PeakBytes.store(0, Atomic::MemoryOrderRelaxed);
            gCategories[i].TotalAllocs.store(0, Atomic::MemoryOrderRelaxed);
            gCategories[i].TotalFrees.store(0, Atomic::MemoryOrderRelaxed);
        }

        gGlobalLiveBytes.store(0, Atomic::MemoryOrderRelaxed);
        gGlobalLiveCount.store(0, Atomic::MemoryOrderRelaxed);
        gOverflowCount.store(0, Atomic::MemoryOrderRelaxed);

        {
            FScopeLock Lock(gCallSiteMutex);
            if (gCallSites != nullptr)
            {
                rpfree(gCallSites);
                gCallSites = nullptr;
            }
        }
    }

    uint32 GetCategoryStats(FMemoryCategoryStats* Out, uint32 MaxOut)
    {
        {
            FScopeLock Lock(gCategoryMutex);
            EnsureDefaultCategory();
        }
        const uint32 Registered = gCategoryCount.load(Atomic::MemoryOrderAcquire);
        const uint32 Count = (Registered < MaxOut) ? Registered : MaxOut;
        for (uint32 i = 0; i < Count; ++i)
        {
            Out[i].Name        = gCategories[i].Name;
            Out[i].LiveBytes   = gCategories[i].LiveBytes.load(Atomic::MemoryOrderRelaxed);
            Out[i].LiveCount   = gCategories[i].LiveCount.load(Atomic::MemoryOrderRelaxed);
            Out[i].PeakBytes   = gCategories[i].PeakBytes.load(Atomic::MemoryOrderRelaxed);
            Out[i].TotalAllocs = gCategories[i].TotalAllocs.load(Atomic::MemoryOrderRelaxed);
            Out[i].TotalFrees  = gCategories[i].TotalFrees.load(Atomic::MemoryOrderRelaxed);
        }
        return Count;
    }

    uint64 GetTrackedLiveBytes()      { return gGlobalLiveBytes.load(Atomic::MemoryOrderRelaxed); }
    uint64 GetTrackedLiveCount()      { return gGlobalLiveCount.load(Atomic::MemoryOrderRelaxed); }
    uint64 GetTrackingOverflowCount() { return gOverflowCount.load(Atomic::MemoryOrderRelaxed); }

    void Hooks::PushCategory(uint32 CategoryId)
    {
        if (tlCategoryDepth < (int)kMaxCatDepth)
        {
            tlCategoryStack[tlCategoryDepth] = CategoryId;
        }
        ++tlCategoryDepth;
    }

    void Hooks::PopCategory()
    {
        if (tlCategoryDepth > 0)
        {
            --tlCategoryDepth;
        }
    }

    void Hooks::OnAlloc(void* Ptr, size_t Size)
    {
        if (!gEnabled.load(Atomic::MemoryOrderRelaxed) || Ptr == nullptr || tlInTracker)
        {
            return;
        }
        tlInTracker = true;

        const uint32 Cat  = CurrentCategory();
        const uint64 Hash = MixPtr(Ptr);
        FShard& Shard = gShards[Hash & (kNumShards - 1)];

        const uint32 StackId = gCaptureStacks.load(Atomic::MemoryOrderRelaxed) ? CaptureCallSite(Size, Cat) : 0;

        bool bInserted;
        {
            FScopeLock Lock(Shard.Mutex);
            bInserted = ShardEnsureCapacity(Shard) && TableInsert(Shard.Entries, Shard.Capacity, Ptr, Hash, Size, Cat, StackId, Shard.Count);
        }

        if (!bInserted)
        {
            gOverflowCount.fetch_add(1, Atomic::MemoryOrderRelaxed);
            ReleaseCallSite(StackId, Size);
            tlInTracker = false;
            return;
        }

        FCategory& C = gCategories[Cat];
        const uint64 NewLive = C.LiveBytes.fetch_add(Size, Atomic::MemoryOrderRelaxed) + Size;
        C.LiveCount.fetch_add(1, Atomic::MemoryOrderRelaxed);
        C.TotalAllocs.fetch_add(1, Atomic::MemoryOrderRelaxed);
        UpdatePeak(C.PeakBytes, NewLive);

        gGlobalLiveBytes.fetch_add(Size, Atomic::MemoryOrderRelaxed);
        gGlobalLiveCount.fetch_add(1, Atomic::MemoryOrderRelaxed);

        tlInTracker = false;
    }

    void Hooks::OnFree(void* Ptr)
    {
        if (!gEnabled.load(Atomic::MemoryOrderRelaxed) || Ptr == nullptr || tlInTracker)
        {
            return;
        }
        tlInTracker = true;

        const uint64 Hash = MixPtr(Ptr);
        FShard& Shard = gShards[Hash & (kNumShards - 1)];

        size_t Size    = 0;
        uint32 Cat     = 0;
        uint32 StackId = 0;
        bool   bFound;
        {
            FScopeLock Lock(Shard.Mutex);
            bFound = ShardRemove(Shard, Ptr, Hash, Size, Cat, StackId);
        }

        if (bFound)
        {
            FCategory& C = gCategories[Cat];
            C.LiveBytes.fetch_sub(Size, Atomic::MemoryOrderRelaxed);
            C.LiveCount.fetch_sub(1, Atomic::MemoryOrderRelaxed);
            C.TotalFrees.fetch_add(1, Atomic::MemoryOrderRelaxed);
            gGlobalLiveBytes.fetch_sub(Size, Atomic::MemoryOrderRelaxed);
            gGlobalLiveCount.fetch_sub(1, Atomic::MemoryOrderRelaxed);
            ReleaseCallSite(StackId, Size);
        }

        tlInTracker = false;
    }

    void Hooks::OnRealloc(void* OldPtr, void* NewPtr, size_t NewSize)
    {
        if (!gEnabled.load(Atomic::MemoryOrderRelaxed) || tlInTracker)
        {
            return;
        }
        tlInTracker = true;

        // Preserves the original category so memory stays attributed to whoever first requested it.
        uint32 Cat = CurrentCategory();

        if (OldPtr != nullptr)
        {
            const uint64 Hash = MixPtr(OldPtr);
            FShard& Shard = gShards[Hash & (kNumShards - 1)];
            size_t OldSize    = 0;
            uint32 OldCat     = 0;
            uint32 OldStackId = 0;
            bool   bFound;
            {
                FScopeLock Lock(Shard.Mutex);
                bFound = ShardRemove(Shard, OldPtr, Hash, OldSize, OldCat, OldStackId);
            }
            if (bFound)
            {
                Cat = OldCat;
                FCategory& C = gCategories[OldCat];
                C.LiveBytes.fetch_sub(OldSize, Atomic::MemoryOrderRelaxed);
                C.LiveCount.fetch_sub(1, Atomic::MemoryOrderRelaxed);
                C.TotalFrees.fetch_add(1, Atomic::MemoryOrderRelaxed);
                gGlobalLiveBytes.fetch_sub(OldSize, Atomic::MemoryOrderRelaxed);
                gGlobalLiveCount.fetch_sub(1, Atomic::MemoryOrderRelaxed);
                ReleaseCallSite(OldStackId, OldSize);
            }
        }

        if (NewPtr != nullptr)
        {
            const uint64 Hash = MixPtr(NewPtr);
            FShard& Shard = gShards[Hash & (kNumShards - 1)];
            const uint32 StackId = gCaptureStacks.load(Atomic::MemoryOrderRelaxed)
                ? CaptureCallSite(NewSize, Cat) : 0;
            bool bInserted;
            {
                FScopeLock Lock(Shard.Mutex);
                bInserted = ShardEnsureCapacity(Shard)
                         && TableInsert(Shard.Entries, Shard.Capacity, NewPtr, Hash, NewSize, Cat, StackId, Shard.Count);
            }
            if (bInserted)
            {
                FCategory& C = gCategories[Cat];
                const uint64 NewLive = C.LiveBytes.fetch_add(NewSize, Atomic::MemoryOrderRelaxed) + NewSize;
                C.LiveCount.fetch_add(1, Atomic::MemoryOrderRelaxed);
                C.TotalAllocs.fetch_add(1, Atomic::MemoryOrderRelaxed);
                UpdatePeak(C.PeakBytes, NewLive);
                gGlobalLiveBytes.fetch_add(NewSize, Atomic::MemoryOrderRelaxed);
                gGlobalLiveCount.fetch_add(1, Atomic::MemoryOrderRelaxed);
            }
            else
            {
                gOverflowCount.fetch_add(1, Atomic::MemoryOrderRelaxed);
                ReleaseCallSite(StackId, NewSize);
            }
        }

        tlInTracker = false;
    }

    uint64 GetCallSiteOverflowCount() { return gCallSiteOverflow.load(Atomic::MemoryOrderRelaxed); }

    void SetCaptureCallstacks(bool bEnabled) { gCaptureStacks.store(bEnabled, Atomic::MemoryOrderRelease); }
    bool IsCapturingCallstacks()             { return gCaptureStacks.load(Atomic::MemoryOrderAcquire); }

    uint32 AnalyzeCategory(const char* CategoryName, FCategoryLedger& OutSummary, FLedgerAlloc* OutLargest, uint32 MaxLargest)
    {
        OutSummary = FCategoryLedger{};

        uint32 FilterCat = 0;
        bool   bFilter   = false;
        if (CategoryName != nullptr && CategoryName[0] != '\0')
        {
            FScopeLock Lock(gCategoryMutex);
            EnsureDefaultCategory();
            const uint64 Hash  = HashString(CategoryName);
            const uint32 Count = gCategoryCount.load(Atomic::MemoryOrderAcquire);
            for (uint32 i = 0; i < Count; ++i)
            {
                if (gCategories[i].NameHash == Hash && std::strcmp(gCategories[i].Name, CategoryName) == 0)
                {
                    FilterCat = i;
                    bFilter   = true;
                    break;
                }
            }
            if (!bFilter)
            {
                return 0;
            }
        }

        struct FTopEntry
        {
            uint64 Size    = 0;
            uint32 StackId = 0;
        };

        FTopEntry    Top[kMaxLedgerTop];
        const uint32 Want = (OutLargest == nullptr) ? 0
                          : ((MaxLargest < kMaxLedgerTop) ? MaxLargest : kMaxLedgerTop);
        uint32       TopCount = 0;

        // The ledger has no tombstones (ShardRemove back-shifts), so a non-null Key is a live entry.
        for (FShard& Shard : gShards)
        {
            FScopeLock Lock(Shard.Mutex);
            if (Shard.Entries == nullptr)
            {
                continue;
            }
            for (uint32 i = 0; i < Shard.Capacity; ++i)
            {
                const FEntry& E = Shard.Entries[i];
                if (E.Key == nullptr || (bFilter && E.CatId != FilterCat))
                {
                    continue;
                }

                const uint64 Size = (uint64)E.Size;
                OutSummary.LiveBytes += Size;
                ++OutSummary.LiveCount;
                if (E.StackId == 0)
                {
                    OutSummary.UnattributedBytes += Size;
                    ++OutSummary.UnattributedCount;
                }

                uint32 Bucket = 0;
                while (Bucket < 63 && (Size >> (Bucket + 1)) != 0)
                {
                    ++Bucket;
                }
                OutSummary.BucketBytes[Bucket] += Size;
                OutSummary.BucketCount[Bucket] += 1;

                if (Want > 0 && (TopCount < Want || Size > Top[Want - 1].Size))
                {
                    uint32 Pos = (TopCount < Want) ? TopCount : (Want - 1);
                    while (Pos > 0 && Top[Pos - 1].Size < Size)
                    {
                        Top[Pos] = Top[Pos - 1];
                        --Pos;
                    }
                    Top[Pos] = FTopEntry{ Size, E.StackId };
                    if (TopCount < Want)
                    {
                        ++TopCount;
                    }
                }
            }
        }

        // Frames are copied after the shard locks are dropped; the two mutexes are never nested.
        for (uint32 i = 0; i < TopCount; ++i)
        {
            OutLargest[i]      = FLedgerAlloc{};
            OutLargest[i].Size = Top[i].Size;
        }
        if (TopCount > 0)
        {
            FScopeLock Lock(gCallSiteMutex);
            if (gCallSites != nullptr)
            {
                for (uint32 i = 0; i < TopCount; ++i)
                {
                    if (Top[i].StackId == 0)
                    {
                        continue;
                    }
                    const FCallSite& C = gCallSites[Top[i].StackId - 1];
                    OutLargest[i].FrameCount = C.FrameCount;
                    std::memcpy(OutLargest[i].Frames, C.Frames, sizeof(void*) * C.FrameCount);
                }
            }
        }

        return TopCount;
    }

    uint32 GetTopCallSites(FCallSiteStat* Out, uint32 MaxOut, ECallSiteSort Sort, const char* CategoryName)
    {
        if (Out == nullptr || MaxOut == 0)
        {
            return 0;
        }

        uint32 FilterCat = 0;
        bool   bFilter   = false;
        if (CategoryName != nullptr && CategoryName[0] != '\0')
        {
            FScopeLock Lock(gCategoryMutex);
            EnsureDefaultCategory();
            const uint64 Hash  = HashString(CategoryName);
            const uint32 Count = gCategoryCount.load(Atomic::MemoryOrderAcquire);
            for (uint32 i = 0; i < Count; ++i)
            {
                if (gCategories[i].NameHash == Hash && std::strcmp(gCategories[i].Name, CategoryName) == 0)
                {
                    FilterCat = i;
                    bFilter   = true;
                    break;
                }
            }
            if (!bFilter)
            {
                return 0;
            }
        }

        const bool bByAllocs = (Sort == ECallSiteSort::TotalAllocs);
        auto KeyOf = [bByAllocs](const FCallSiteStat& S) -> uint64
        {
            return bByAllocs ? S.TotalAllocs : S.LiveBytes;
        };

        FScopeLock Lock(gCallSiteMutex);
        if (gCallSites == nullptr)
        {
            return 0;
        }

        uint32 Count = 0;
        for (uint32 i = 0; i < kMaxCallSites; ++i)
        {
            const FCallSite& C = gCallSites[i];
            const uint64 Key = bByAllocs ? C.TotalAllocs : C.LiveBytes;
            if (C.Hash == 0 || Key == 0)
            {
                continue;
            }
            if (bFilter && C.CatId != FilterCat)
            {
                continue;
            }
            if (Count == MaxOut && Key <= KeyOf(Out[MaxOut - 1]))
            {
                continue;
            }

            uint32 Pos = (Count < MaxOut) ? Count : (MaxOut - 1);
            while (Pos > 0 && KeyOf(Out[Pos - 1]) < Key)
            {
                Out[Pos] = Out[Pos - 1];
                --Pos;
            }

            FCallSiteStat& Dst = Out[Pos];
            Dst.CatName     = (C.CatId < kMaxCategories) ? gCategories[C.CatId].Name : "";
            Dst.LiveBytes   = C.LiveBytes;
            Dst.LiveCount   = C.LiveCount;
            Dst.TotalAllocs = C.TotalAllocs;
            Dst.FrameCount  = C.FrameCount;
            std::memcpy(Dst.Frames, C.Frames, sizeof(void*) * C.FrameCount);

            if (Count < MaxOut)
            {
                ++Count;
            }
        }
        return Count;
    }

    bool ResolveSymbol(void* Address, char* OutText, size_t OutTextSize)
    {
        if (OutText == nullptr || OutTextSize == 0)
        {
            return false;
        }
#if defined(LE_PLATFORM_WINDOWS)
        // DbgHelp is single-threaded; serialize and lazy-init once for the process.
        static FMutex SymMutex;
        static bool   bSymInit = false;
        FScopeLock Lock(SymMutex);

        HANDLE Process = GetCurrentProcess();
        if (!bSymInit)
        {
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
            SymInitialize(Process, nullptr, TRUE);
            bSymInit = true;
        }

        alignas(SYMBOL_INFO) char SymBuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* Symbol = reinterpret_cast<SYMBOL_INFO*>(SymBuf);
        std::memset(Symbol, 0, sizeof(SymBuf));
        Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        Symbol->MaxNameLen   = 255;

        auto FormatResolved = [&]()
        {
            IMAGEHLP_LINE64 LineInfo{};
            LineInfo.SizeOfStruct = sizeof(LineInfo);
            DWORD LineDisp = 0;
            if (SymGetLineFromAddr64(Process, (DWORD64)(uintptr_t)Address, &LineDisp, &LineInfo) && LineInfo.FileName)
            {
                const char* Leaf = LineInfo.FileName;
                for (const char* p = LineInfo.FileName; *p; ++p)
                {
                    if (*p == '\\' || *p == '/') { Leaf = p + 1; }
                }
                _snprintf_s(OutText, OutTextSize, _TRUNCATE, "%s  (%s:%lu)",
                    Symbol->Name, Leaf, (unsigned long)LineInfo.LineNumber);
            }
            else
            {
                _snprintf_s(OutText, OutTextSize, _TRUNCATE, "%s", Symbol->Name);
            }
        };

        DWORD64 Disp = 0;
        if (SymFromAddr(Process, (DWORD64)(uintptr_t)Address, &Disp, Symbol))
        {
            FormatResolved();
            return true;
        }

        // SymInitialize only saw the modules loaded then, so anything mapped later has no symbols.
        {
            static uint64 LastRefreshMs = 0;
            const uint64  NowMs = GetTickCount64();
            if (LastRefreshMs == 0 || NowMs - LastRefreshMs > 1000)
            {
                LastRefreshMs = NowMs;
                SymRefreshModuleList(Process);
                if (SymFromAddr(Process, (DWORD64)(uintptr_t)Address, &Disp, Symbol))
                {
                    FormatResolved();
                    return true;
                }
            }
        }

        // Naming the module and offset says WHERE an allocation came from even with no PDB.
        HMODULE Module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(Address), &Module) && Module != nullptr)
        {
            char ModulePath[MAX_PATH] = {};
            if (GetModuleFileNameA(Module, ModulePath, MAX_PATH) != 0)
            {
                const char* Leaf = ModulePath;
                for (const char* p = ModulePath; *p; ++p)
                {
                    if (*p == '\\' || *p == '/') { Leaf = p + 1; }
                }
                const uintptr_t Offset = (uintptr_t)Address - (uintptr_t)Module;
                _snprintf_s(OutText, OutTextSize, _TRUNCATE, "%s+0x%llX", Leaf, (unsigned long long)Offset);
                return false;
            }
        }

        _snprintf_s(OutText, OutTextSize, _TRUNCATE, "0x%016llX", (unsigned long long)(uintptr_t)Address);
        return false;
#else
        std::snprintf(OutText, OutTextSize, "0x%016llX", (unsigned long long)(uintptr_t)Address);
        return false;
#endif
    }
}

#endif // LUMINA_MEMORY_TRACKING
