#include "RuntimePCH.h"
#include "Memory/DebugAllocator.h"

#if LUMINA_DEBUG_ALLOCATOR

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Core/Assertions/Assert.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Threading/Sync.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"

#if defined(LE_PLATFORM_WINDOWS)
    #include <Windows.h>
    #include <malloc.h>
#else
    #include <execinfo.h>
    #include <sys/mman.h>
    #include <unistd.h>
#endif

namespace Lumina::Memory::DebugAllocatorInternal
{
    using EError        = DebugAllocator::EError;
    using FError        = DebugAllocator::FError;
    using FStats        = DebugAllocator::FStats;
    using FErrorHandler = DebugAllocator::FErrorHandler;

    constexpr uint64 kLiveMagic             = 0xA110CA7EDEB06A11ull;
    constexpr uint64 kFreedMagic            = 0xF4EEDDEB06DEAD11ull;
    constexpr uint8  kAllocFill             = 0xCD;
    constexpr uint8  kFreeFill              = 0xDD;
    constexpr uint8  kFrontGuardFill        = 0xFB;
    constexpr uint8  kBackGuardFill         = 0xFD;
    constexpr uint64 kFrontGuardSize        = 32;
    constexpr uint64 kMinBackGuardSize      = 32;
    constexpr uint64 kMinAlignment          = 16;
    constexpr uint64 kMaxAlignment          = 1ull << 20;
    constexpr uint64 kMaxSaneSize           = 1ull << 40;
    constexpr uint32 kStackDepth            = 32;
    constexpr uint32 kShardCount            = 64;
    constexpr uint32 kMaxReportedPerPass    = 64;
    constexpr uint32 kEvictionBatch         = 32;
    constexpr uint64 kPageBlockFlag         = 1;

    struct FStackRecord
    {
        void*   AllocFrames[kStackDepth];
        void*   FreeFrames[kStackDepth];
        uint32  AllocDepth;
        uint32  FreeDepth;
    };

    struct alignas(16) FHeader
    {
        uint64          Magic;
        uint8*          Base;
        uint64          MappedSize;
        uint64          DataBytes;
        uint64          Size;
        uint64          Alignment;
        uint64          Serial;
        uint64          AllocThread;
        uint64          FreeThread;
        FStackRecord*   Stacks;
        uint64          Flags;
        uint64          Checksum;
    };

    static_assert(sizeof(FHeader) % 16 == 0, "the header sits directly before the front guard and must keep the user pointer aligned");

    enum class EState : uint8
    {
        Empty,
        Live,
        Quarantined,
    };

    struct FSlot
    {
        uintptr_t       Key;
        uint64          Size;
        uint64          Serial;
        uint64          Flags;
        FStackRecord*   Stacks;
        EState          State;
    };

    struct FShard
    {
        FMutex          Mutex;
        FSlot*          Slots = nullptr;
        uint64          Capacity = 0;
        uint64          Count = 0;
    };

    struct FQuarantined
    {
        uint8*          User;
        uint8*          Base;
        uint64          MappedSize;
        FStackRecord*   Stacks;
        uint64          Flags;
    };

    struct FSnapshot
    {
        FSlot           Slot{};
        FStackRecord    Stacks{};
        uint64          Alignment = 0;
        uint64          AllocThread = 0;
        uint64          FreeThread = 0;
        bool            bFound = false;
        bool            bHasStacks = false;
    };

    struct FRegion
    {
        uint8*          Data;
        uint64          Count;
        uint8           Fill;
        EError          Kind;
    };

    struct FOptions
    {
        uint64          PageGuardEvery = 0;
        bool            bCaptureStacks = false;
        bool            bFillOnAllocate = true;
        uint64          QuarantineBytes = 256ull << 20;
        uint64          QuarantineCount = 1ull << 20;
        uint64          ValidateEvery = 0;
        uint64          BreakOnSerial = 0;
        uint64          PageSize = 4096;
    };

    uint64 AlignUp(uint64 Value, uint64 Alignment)
    {
        return (Value + Alignment - 1) & ~(Alignment - 1);
    }

    uint64 HashKey(uintptr_t Key)
    {
        uint64 Hash = static_cast<uint64>(Key) >> 4;
        Hash ^= Hash >> 33;
        Hash *= 0xFF51AFD7ED558CCDull;
        Hash ^= Hash >> 33;
        return Hash;
    }

    bool ReadFlag(const char* Name, bool bDefault)
    {
        const char* Value = std::getenv(Name);
        return Value == nullptr ? bDefault : (Value[0] != '\0' && Value[0] != '0');
    }

    uint64 ReadNumber(const char* Name, uint64 Default)
    {
        const char* Value = std::getenv(Name);
        return Value == nullptr || Value[0] == '\0' ? Default : std::strtoull(Value, nullptr, 10);
    }

    uint64 QueryPageSize()
    {
    #if defined(LE_PLATFORM_WINDOWS)
        SYSTEM_INFO Info;
        GetSystemInfo(&Info);
        return Info.dwPageSize;
    #else
        const long Size = sysconf(_SC_PAGESIZE);
        return Size > 0 ? static_cast<uint64>(Size) : 4096;
    #endif
    }

    uint8* SystemAllocate(uint64 Alignment, uint64 Bytes)
    {
    #if defined(LE_PLATFORM_WINDOWS)
        return static_cast<uint8*>(_aligned_malloc(Bytes, Alignment));
    #else
        void* Memory = nullptr;
        return posix_memalign(&Memory, Alignment, Bytes) == 0 ? static_cast<uint8*>(Memory) : nullptr;
    #endif
    }

    void SystemFree(uint8* Memory)
    {
    #if defined(LE_PLATFORM_WINDOWS)
        _aligned_free(Memory);
    #else
        std::free(Memory);
    #endif
    }

    uint8* MapPages(uint64 Bytes)
    {
    #if defined(LE_PLATFORM_WINDOWS)
        return static_cast<uint8*>(VirtualAlloc(nullptr, Bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    #else
        void* Memory = mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return Memory == MAP_FAILED ? nullptr : static_cast<uint8*>(Memory);
    #endif
    }

    bool ProtectNoAccess(uint8* Memory, uint64 Bytes)
    {
    #if defined(LE_PLATFORM_WINDOWS)
        DWORD Previous = 0;
        return VirtualProtect(Memory, Bytes, PAGE_NOACCESS, &Previous) != 0;
    #else
        return mprotect(Memory, Bytes, PROT_NONE) == 0;
    #endif
    }

    void UnmapPages(uint8* Memory, uint64 Bytes)
    {
    #if defined(LE_PLATFORM_WINDOWS)
        (void)Bytes;
        VirtualFree(Memory, 0, MEM_RELEASE);
    #else
        munmap(Memory, Bytes);
    #endif
    }

    void ReleaseBlock(uint8* Base, uint64 MappedSize, uint64 Flags)
    {
        if ((Flags & kPageBlockFlag) != 0)
        {
            UnmapPages(Base, MappedSize);
        }
        else
        {
            SystemFree(Base);
        }
    }

    FORCENOINLINE uint32 CaptureStack(void** OutFrames)
    {
    #if defined(LE_PLATFORM_WINDOWS)
        return RtlCaptureStackBackTrace(1, kStackDepth, OutFrames, nullptr);
    #else
        void* Raw[kStackDepth + 1];
        const int Count = ::backtrace(Raw, kStackDepth + 1);
        if (Count <= 1)
        {
            return 0;
        }
        const uint32 Kept = static_cast<uint32>(Count - 1);
        std::memcpy(OutFrames, Raw + 1, sizeof(void*) * Kept);
        return Kept;
    #endif
    }

    FHeader* HeaderOf(uint8* User)
    {
        return reinterpret_cast<FHeader*>(User - kFrontGuardSize - sizeof(FHeader));
    }

    uint64 ComputeChecksum(const FHeader& Header)
    {
        uint64 Hash = 0xCBF29CE484222325ull;
        const auto Mix = [&Hash](uint64 Value)
        {
            Hash ^= Value;
            Hash *= 0x100000001B3ull;
            Hash ^= Hash >> 29;
        };
        Mix(Header.Magic);
        Mix(reinterpret_cast<uintptr_t>(Header.Base));
        Mix(Header.MappedSize);
        Mix(Header.DataBytes);
        Mix(Header.Size);
        Mix(Header.Alignment);
        Mix(Header.Serial);
        Mix(Header.AllocThread);
        Mix(Header.FreeThread);
        Mix(reinterpret_cast<uintptr_t>(Header.Stacks));
        Mix(Header.Flags);
        return Hash;
    }

    void Seal(FHeader& Header)
    {
        Header.Checksum = ComputeChecksum(Header);
    }

    void StampGuards(const FHeader& Header, uint8* User)
    {
        const uint8* HeaderBytes = reinterpret_cast<const uint8*>(&Header);
        std::memset(Header.Base, kFrontGuardFill, static_cast<size_t>(HeaderBytes - Header.Base));
        std::memset(User - kFrontGuardSize, kFrontGuardFill, kFrontGuardSize);
        std::memset(User + Header.Size, kBackGuardFill, static_cast<size_t>(Header.Base + Header.DataBytes - (User + Header.Size)));
    }

    const uint8* FindMismatch(const uint8* Data, uint64 Count, uint8 Fill)
    {
        uint64 Pattern;
        std::memset(&Pattern, Fill, sizeof(Pattern));

        uint64 Index = 0;
        for (; Index + sizeof(Pattern) <= Count; Index += sizeof(Pattern))
        {
            uint64 Word;
            std::memcpy(&Word, Data + Index, sizeof(Word));
            if (Word != Pattern)
            {
                break;
            }
        }
        for (; Index < Count; ++Index)
        {
            if (Data[Index] != Fill)
            {
                return Data + Index;
            }
        }
        return nullptr;
    }

    void DescribeHeader(const FHeader& Header, FError& Out)
    {
        Out.Size = Header.Size;
        Out.Alignment = Header.Alignment;
        Out.Serial = Header.Serial;
        Out.AllocThread = Header.AllocThread;
        Out.FreeThread = Header.FreeThread;
        if (Header.Stacks != nullptr)
        {
            Out.AllocStack = Header.Stacks->AllocFrames;
            Out.AllocStackDepth = Header.Stacks->AllocDepth;
            Out.FreeStack = Header.Stacks->FreeFrames;
            Out.FreeStackDepth = Header.Stacks->FreeDepth;
        }
    }

    void DescribeSnapshot(const FSnapshot& Snapshot, FError& Out)
    {
        Out.Size = Snapshot.Slot.Size;
        Out.Serial = Snapshot.Slot.Serial;
        Out.Alignment = Snapshot.Alignment;
        Out.AllocThread = Snapshot.AllocThread;
        Out.FreeThread = Snapshot.FreeThread;
        if (Snapshot.bHasStacks)
        {
            Out.AllocStack = Snapshot.Stacks.AllocFrames;
            Out.AllocStackDepth = Snapshot.Stacks.AllocDepth;
            Out.FreeStack = Snapshot.Stacks.FreeFrames;
            Out.FreeStackDepth = Snapshot.Stacks.FreeDepth;
        }
    }

    bool InspectAndRepair(uint8* User, bool bFreed, const char* Operation, FError& Out)
    {
        FHeader* Header = HeaderOf(User);
        Out.Pointer = User;
        Out.Operation = Operation;

        if (Header->Magic != (bFreed ? kFreedMagic : kLiveMagic) || Header->Checksum != ComputeChecksum(*Header))
        {
            Out.Kind = EError::HeaderCorrupted;
            Out.Offset = -static_cast<int64>(kFrontGuardSize + sizeof(FHeader));
            return false;
        }

        DescribeHeader(*Header, Out);

        uint8* HeaderBytes = reinterpret_cast<uint8*>(Header);
        uint8* BackGuard = User + Header->Size;
        const FRegion Regions[] =
        {
            { User - kFrontGuardSize, kFrontGuardSize, kFrontGuardFill, EError::Underrun },
            { BackGuard, static_cast<uint64>(Header->Base + Header->DataBytes - BackGuard), kBackGuardFill, EError::Overrun },
            { User, bFreed ? Header->Size : 0, kFreeFill, EError::UseAfterFree },
            { Header->Base, static_cast<uint64>(HeaderBytes - Header->Base), kFrontGuardFill, EError::Underrun },
        };

        bool bIntact = true;
        for (const FRegion& Region : Regions)
        {
            const uint8* Bad = FindMismatch(Region.Data, Region.Count, Region.Fill);
            if (Bad == nullptr)
            {
                continue;
            }
            if (bIntact)
            {
                Out.Kind = Region.Kind;
                Out.Offset = static_cast<int64>(Bad - User);
                Out.Expected = Region.Fill;
                Out.Found = *Bad;
                bIntact = false;
            }
            std::memset(Region.Data, Region.Fill, Region.Count);
        }
        return bIntact;
    }

    void AppendText(char*& Cursor, char* End, const char* Format, ...)
    {
        if (End - Cursor <= 1)
        {
            return;
        }
        va_list Arguments;
        va_start(Arguments, Format);
        const int Written = std::vsnprintf(Cursor, static_cast<size_t>(End - Cursor), Format, Arguments);
        va_end(Arguments);
        if (Written > 0)
        {
            Cursor += Written < End - Cursor ? Written : End - Cursor - 1;
        }
    }

    void AppendStack(char*& Cursor, char* End, const char* Title, void* const* Frames, uint32 Depth)
    {
        if (Frames == nullptr || Depth == 0)
        {
            return;
        }

        AppendText(Cursor, End, "\n  %s:\n", Title);

    #if defined(LE_PLATFORM_WINDOWS)
        for (uint32 Index = 0; Index < Depth; ++Index)
        {
            HMODULE Module = nullptr;
            char Path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(Frames[Index]), &Module))
            {
                GetModuleFileNameA(Module, Path, MAX_PATH);
            }
            const char* Name = std::strrchr(Path, '\\');
            AppendText(Cursor, End, "    %s+0x%llx\n", Name != nullptr ? Name + 1 : Path,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(Frames[Index]) - reinterpret_cast<uintptr_t>(Module)));
        }
    #else
        char** Symbols = ::backtrace_symbols(Frames, static_cast<int>(Depth));
        for (uint32 Index = 0; Index < Depth; ++Index)
        {
            AppendText(Cursor, End, "    %s\n", Symbols != nullptr ? Symbols[Index] : "?");
        }
        std::free(Symbols);
    #endif
    }

    [[noreturn]] void ReportAndCrash(const FError& Error)
    {
        static char Message[16384];
        char* Cursor = Message;
        char* End = Message + sizeof(Message);

        AppendText(Cursor, End, "Debug allocator: %s during %s at %p.", DebugAllocator::GetErrorName(Error.Kind), Error.Operation, Error.Pointer);

        if (Error.Serial != 0)
        {
            AppendText(Cursor, End, "\n  Block of %llu bytes, alignment %llu, allocation #%llu on thread %llu",
                static_cast<unsigned long long>(Error.Size), static_cast<unsigned long long>(Error.Alignment),
                static_cast<unsigned long long>(Error.Serial), static_cast<unsigned long long>(Error.AllocThread));
            if (Error.FreeThread != 0)
            {
                AppendText(Cursor, End, ", freed on thread %llu", static_cast<unsigned long long>(Error.FreeThread));
            }
            AppendText(Cursor, End, ".");
        }

        switch (Error.Kind)
        {
        case EError::Underrun:
        case EError::Overrun:
        case EError::UseAfterFree:
            AppendText(Cursor, End, "\n  First damaged byte at offset %+lld: expected 0x%02X, found 0x%02X.",
                static_cast<long long>(Error.Offset), Error.Expected, Error.Found);
            break;
        case EError::InvalidPointer:
            AppendText(Cursor, End, "\n  The allocator never returned this pointer, already released it, or it points inside a block.");
            break;
        case EError::HeaderCorrupted:
            AppendText(Cursor, End, "\n  The block's header was overwritten, usually by an underrun that ran past the front guard.");
            break;
        case EError::InvalidArgument:
            AppendText(Cursor, End, "\n  Asked for %llu bytes at alignment %llu.", static_cast<unsigned long long>(Error.Size), static_cast<unsigned long long>(Error.Alignment));
            break;
        default:
            break;
        }

        AppendStack(Cursor, End, "Allocated at", Error.AllocStack, Error.AllocStackDepth);
        AppendStack(Cursor, End, "Freed at", Error.FreeStack, Error.FreeStackDepth);
        if (Error.AllocStack == nullptr && Error.Serial != 0)
        {
            AppendText(Cursor, End, "\n  Run with LUMINA_DEBUG_ALLOCATOR_STACKS=1 to record where blocks are allocated and freed.");
        }

        std::fputs(Message, stderr);
        std::fputs("\n", stderr);
        std::fflush(stderr);

        thread_local bool bPanicking = false;
        if (!bPanicking)
        {
            bPanicking = true;
            PANIC("{}", Message);
        }
        std::abort();
    }

    void BreakForSerial(uint64 Serial, const char* What)
    {
        std::fprintf(stderr, "Debug allocator: allocation #%llu %s.\n", static_cast<unsigned long long>(Serial), What);
        std::fflush(stderr);
    #if defined(LE_PLATFORM_WINDOWS)
        LUMINA_DEBUG_BREAK();
    #else
        std::raise(SIGTRAP);
    #endif
    }

    class FDebugHeap
    {
    public:

        constexpr FDebugHeap() = default;

        void* Allocate(size_t Size, size_t Alignment);
        void* Reallocate(void* Memory, size_t NewSize, size_t Alignment);
        void Free(void* Memory);
        size_t GetAllocationSize(const void* Memory);

        FErrorHandler SetErrorHandler(FErrorHandler NewHandler);
        uint64 ValidateHeap();
        void FlushQuarantine();
        FStats GetStats();
        bool DumpLiveAllocations(const char* Path);

    private:

        const FOptions& GetOptions();
        FShard& ShardFor(uintptr_t Key);
        void Report(const FError& Error);
        void ReportMisuse(EError Kind, const char* Operation, const void* Pointer, const FSnapshot& Snapshot);
        void MaybeValidate();

        uint8* AllocateFromHeap(uint64 Size, uint64 Alignment, FHeader*& OutHeader);
        uint8* AllocateFromPages(uint64 Size, uint64 Alignment, FHeader*& OutHeader);

        FSnapshot TakeSnapshot(uintptr_t Key, bool bClaimForFree);

        void Quarantine(const FQuarantined& Entry);
        void Evict(uint64 KeepBytes, uint64 KeepCount);
        void Release(const FQuarantined& Entry);

        static FSlot* FindSlot(FShard& Shard, uintptr_t Key);
        static FSlot* InsertSlot(FShard& Shard, uintptr_t Key);
        static void EraseSlot(FShard& Shard, FSlot* Slot);
        static bool GrowShard(FShard& Shard);

        FShard                          Shards[kShardCount];

        FMutex                          QuarantineMutex;
        FQuarantined*                   Ring = nullptr;
        uint64                          RingCapacity = 0;
        uint64                          RingHead = 0;
        uint64                          RingCount = 0;
        uint64                          RingBytes = 0;

        FMutex                          OptionsMutex;
        FOptions                        Options;
        std::atomic<bool>               bOptionsReady{ false };

        std::atomic<FErrorHandler>      Handler{ nullptr };
        std::atomic<uint64>             NextSerial{ 0 };
        std::atomic<uint64>             LiveBytes{ 0 };
        std::atomic<uint64>             LiveCount{ 0 };
        std::atomic<uint64>             PeakLiveBytes{ 0 };
        std::atomic<uint64>             TotalAllocations{ 0 };
        std::atomic<uint64>             TotalFrees{ 0 };
        std::atomic<uint64>             ErrorsReported{ 0 };
        std::atomic<uint64>             Operations{ 0 };
        std::atomic<bool>               bWarnedPageFallback{ false };
    };

    const FOptions& FDebugHeap::GetOptions()
    {
        if (bOptionsReady.load(std::memory_order_acquire))
        {
            return Options;
        }

        FScopeLock Lock(OptionsMutex);
        if (!bOptionsReady.load(std::memory_order_relaxed))
        {
            Options.PageGuardEvery  = ReadNumber("LUMINA_DEBUG_ALLOCATOR_PAGEGUARD", 0);
            Options.bCaptureStacks  = ReadFlag("LUMINA_DEBUG_ALLOCATOR_STACKS", false);
            Options.bFillOnAllocate = !ReadFlag("LUMINA_DEBUG_ALLOCATOR_NOFILL", false);
            Options.QuarantineBytes = ReadNumber("LUMINA_DEBUG_ALLOCATOR_QUARANTINE_MB", 256) << 20;
            Options.QuarantineCount = ReadNumber("LUMINA_DEBUG_ALLOCATOR_QUARANTINE_COUNT", 1ull << 20);
            Options.ValidateEvery   = ReadNumber("LUMINA_DEBUG_ALLOCATOR_VALIDATE_EVERY", 0);
            Options.BreakOnSerial   = ReadNumber("LUMINA_DEBUG_ALLOCATOR_BREAK_ON", 0);
            Options.PageSize        = QueryPageSize();
            bOptionsReady.store(true, std::memory_order_release);
        }
        return Options;
    }

    FShard& FDebugHeap::ShardFor(uintptr_t Key)
    {
        return Shards[HashKey(Key) % kShardCount];
    }

    void FDebugHeap::Report(const FError& Error)
    {
        ErrorsReported.fetch_add(1, std::memory_order_relaxed);
        if (FErrorHandler Custom = Handler.load(std::memory_order_acquire))
        {
            Custom(Error);
            return;
        }
        ReportAndCrash(Error);
    }

    void FDebugHeap::ReportMisuse(EError Kind, const char* Operation, const void* Pointer, const FSnapshot& Snapshot)
    {
        FError Error;
        Error.Kind = Kind;
        Error.Operation = Operation;
        Error.Pointer = Pointer;
        if (Snapshot.bFound)
        {
            DescribeSnapshot(Snapshot, Error);
        }
        Report(Error);
    }

    void FDebugHeap::MaybeValidate()
    {
        const uint64 Every = GetOptions().ValidateEvery;
        if (Every == 0 || Operations.fetch_add(1, std::memory_order_relaxed) % Every != Every - 1)
        {
            return;
        }

        thread_local bool bValidating = false;
        if (!bValidating)
        {
            bValidating = true;
            ValidateHeap();
            bValidating = false;
        }
    }

    FSlot* FDebugHeap::FindSlot(FShard& Shard, uintptr_t Key)
    {
        if (Shard.Capacity == 0)
        {
            return nullptr;
        }

        const uint64 Mask = Shard.Capacity - 1;
        for (uint64 Index = (HashKey(Key) / kShardCount) & Mask;; Index = (Index + 1) & Mask)
        {
            FSlot& Slot = Shard.Slots[Index];
            if (Slot.State == EState::Empty)
            {
                return nullptr;
            }
            if (Slot.Key == Key)
            {
                return &Slot;
            }
        }
    }

    bool FDebugHeap::GrowShard(FShard& Shard)
    {
        const uint64 NewCapacity = Shard.Capacity == 0 ? 1024 : Shard.Capacity * 2;
        FSlot* NewSlots = static_cast<FSlot*>(std::calloc(NewCapacity, sizeof(FSlot)));
        if (NewSlots == nullptr)
        {
            return false;
        }

        const uint64 Mask = NewCapacity - 1;
        for (uint64 Old = 0; Old < Shard.Capacity; ++Old)
        {
            const FSlot& Slot = Shard.Slots[Old];
            if (Slot.State == EState::Empty)
            {
                continue;
            }
            uint64 Index = (HashKey(Slot.Key) / kShardCount) & Mask;
            while (NewSlots[Index].State != EState::Empty)
            {
                Index = (Index + 1) & Mask;
            }
            NewSlots[Index] = Slot;
        }

        std::free(Shard.Slots);
        Shard.Slots = NewSlots;
        Shard.Capacity = NewCapacity;
        return true;
    }

    FSlot* FDebugHeap::InsertSlot(FShard& Shard, uintptr_t Key)
    {
        if ((Shard.Count + 1) * 10 > Shard.Capacity * 7 && !GrowShard(Shard))
        {
            return nullptr;
        }

        const uint64 Mask = Shard.Capacity - 1;
        for (uint64 Index = (HashKey(Key) / kShardCount) & Mask;; Index = (Index + 1) & Mask)
        {
            FSlot& Slot = Shard.Slots[Index];
            if (Slot.State == EState::Empty)
            {
                Slot.Key = Key;
                ++Shard.Count;
                return &Slot;
            }
            if (Slot.Key == Key)
            {
                return &Slot;
            }
        }
    }

    void FDebugHeap::EraseSlot(FShard& Shard, FSlot* Slot)
    {
        const uint64 Mask = Shard.Capacity - 1;
        uint64 Hole = static_cast<uint64>(Slot - Shard.Slots);
        Shard.Slots[Hole].State = EState::Empty;
        --Shard.Count;

        for (uint64 Next = (Hole + 1) & Mask; Shard.Slots[Next].State != EState::Empty; Next = (Next + 1) & Mask)
        {
            const uint64 Home = (HashKey(Shard.Slots[Next].Key) / kShardCount) & Mask;
            const bool bMove = Next > Hole ? (Home <= Hole || Home > Next) : (Home <= Hole && Home > Next);
            if (bMove)
            {
                Shard.Slots[Hole] = Shard.Slots[Next];
                Shard.Slots[Next].State = EState::Empty;
                Hole = Next;
            }
        }
    }

    FSnapshot FDebugHeap::TakeSnapshot(uintptr_t Key, bool bClaimForFree)
    {
        FSnapshot Snapshot;
        FShard& Shard = ShardFor(Key);
        FScopeLock Lock(Shard.Mutex);

        FSlot* Slot = FindSlot(Shard, Key);
        if (Slot == nullptr)
        {
            return Snapshot;
        }

        Snapshot.Slot = *Slot;
        Snapshot.bFound = true;

        if (Slot->State == EState::Live)
        {
            if (bClaimForFree)
            {
                Slot->State = EState::Quarantined;
            }
            return Snapshot;
        }

        if (Slot->Stacks != nullptr)
        {
            Snapshot.Stacks = *Slot->Stacks;
            Snapshot.bHasStacks = true;
        }
        if ((Slot->Flags & kPageBlockFlag) == 0)
        {
            const FHeader* Header = HeaderOf(reinterpret_cast<uint8*>(Key));
            Snapshot.Alignment = Header->Alignment;
            Snapshot.AllocThread = Header->AllocThread;
            Snapshot.FreeThread = Header->FreeThread;
        }
        return Snapshot;
    }

    uint8* FDebugHeap::AllocateFromHeap(uint64 Size, uint64 Alignment, FHeader*& OutHeader)
    {
        const uint64 Prefix = AlignUp(sizeof(FHeader) + kFrontGuardSize, Alignment);
        const uint64 Total = Prefix + AlignUp(Size, 16) + kMinBackGuardSize;

        uint8* Base = SystemAllocate(Alignment, Total);
        if (Base == nullptr)
        {
            return nullptr;
        }

        uint8* User = Base + Prefix;
        OutHeader = HeaderOf(User);
        OutHeader->Base = Base;
        OutHeader->MappedSize = Total;
        OutHeader->DataBytes = Total;
        OutHeader->Flags = 0;
        return User;
    }

    uint8* FDebugHeap::AllocateFromPages(uint64 Size, uint64 Alignment, FHeader*& OutHeader)
    {
        const uint64 Page = GetOptions().PageSize;
        const uint64 Prefix = AlignUp(sizeof(FHeader) + kFrontGuardSize, Alignment);
        const uint64 DataBytes = AlignUp(Prefix + Size + Alignment, Page);
        const uint64 Total = DataBytes + Page;

        uint8* Base = MapPages(Total);
        if (Base == nullptr)
        {
            return nullptr;
        }
        if (!ProtectNoAccess(Base + DataBytes, Page))
        {
            UnmapPages(Base, Total);
            return nullptr;
        }

        const uintptr_t DataEnd = reinterpret_cast<uintptr_t>(Base + DataBytes);
        uint8* User = reinterpret_cast<uint8*>((DataEnd - Size) & ~static_cast<uintptr_t>(Alignment - 1));
        OutHeader = HeaderOf(User);
        OutHeader->Base = Base;
        OutHeader->MappedSize = Total;
        OutHeader->DataBytes = DataBytes;
        OutHeader->Flags = kPageBlockFlag;
        return User;
    }

    void* FDebugHeap::Allocate(size_t Size, size_t Alignment)
    {
        const FOptions& Settings = GetOptions();

        if (Alignment < kMinAlignment)
        {
            Alignment = kMinAlignment;
        }
        if ((Alignment & (Alignment - 1)) != 0 || Alignment > kMaxAlignment || Size > kMaxSaneSize)
        {
            FError Error;
            Error.Kind = EError::InvalidArgument;
            Error.Operation = "Allocate";
            Error.Size = Size;
            Error.Alignment = Alignment;
            Report(Error);
            return nullptr;
        }

        const uint64 Serial = NextSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        if (Serial == Settings.BreakOnSerial)
        {
            BreakForSerial(Serial, "is being allocated");
        }

        FHeader* Header = nullptr;
        uint8* User = nullptr;
        if (Settings.PageGuardEvery != 0 && Serial % Settings.PageGuardEvery == 0)
        {
            User = AllocateFromPages(Size, Alignment, Header);
            if (User == nullptr && !bWarnedPageFallback.exchange(true))
            {
                std::fprintf(stderr, "Debug allocator: the OS refused more guarded pages, so later blocks use guard bytes instead. "
                    "On Linux, raising vm.max_map_count lifts the limit.\n");
            }
        }
        if (User == nullptr)
        {
            User = AllocateFromHeap(Size, Alignment, Header);
        }
        if (User == nullptr)
        {
            return nullptr;
        }

        Header->Magic = kLiveMagic;
        Header->Size = Size;
        Header->Alignment = Alignment;
        Header->Serial = Serial;
        Header->AllocThread = Threading::GetThreadID();
        Header->FreeThread = 0;
        Header->Stacks = nullptr;
        if (Settings.bCaptureStacks)
        {
            Header->Stacks = static_cast<FStackRecord*>(std::calloc(1, sizeof(FStackRecord)));
            if (Header->Stacks != nullptr)
            {
                Header->Stacks->AllocDepth = CaptureStack(Header->Stacks->AllocFrames);
            }
        }
        Seal(*Header);

        StampGuards(*Header, User);
        if (Settings.bFillOnAllocate)
        {
            std::memset(User, kAllocFill, Size);
        }

        const uintptr_t Key = reinterpret_cast<uintptr_t>(User);
        FSnapshot Stale;
        bool bTracked = false;
        {
            FShard& Shard = ShardFor(Key);
            FScopeLock Lock(Shard.Mutex);
            if (FSlot* Slot = InsertSlot(Shard, Key))
            {
                if (Slot->State != EState::Empty)
                {
                    Stale.Slot = *Slot;
                    Stale.bFound = true;
                }
                Slot->State = EState::Live;
                Slot->Size = Size;
                Slot->Serial = Serial;
                Slot->Flags = Header->Flags;
                Slot->Stacks = Header->Stacks;
                bTracked = true;
            }
        }

        if (!bTracked)
        {
            std::free(Header->Stacks);
            ReleaseBlock(Header->Base, Header->MappedSize, Header->Flags);
            return nullptr;
        }

        if (Stale.bFound)
        {
            ReportMisuse(EError::InvalidPointer, "Allocate, which was handed an address it still tracked, so something released a block behind its back", User, Stale);
        }

        const uint64 Bytes = LiveBytes.fetch_add(Size, std::memory_order_relaxed) + Size;
        uint64 Peak = PeakLiveBytes.load(std::memory_order_relaxed);
        while (Bytes > Peak && !PeakLiveBytes.compare_exchange_weak(Peak, Bytes, std::memory_order_relaxed))
        {
        }
        LiveCount.fetch_add(1, std::memory_order_relaxed);
        TotalAllocations.fetch_add(1, std::memory_order_relaxed);

        MaybeValidate();
        return User;
    }

    void FDebugHeap::Free(void* Memory)
    {
        if (Memory == nullptr)
        {
            return;
        }

        uint8* User = static_cast<uint8*>(Memory);
        const uintptr_t Key = reinterpret_cast<uintptr_t>(User);

        const FSnapshot Snapshot = TakeSnapshot(Key, true);
        if (!Snapshot.bFound)
        {
            ReportMisuse(EError::InvalidPointer, "Free", User, Snapshot);
            return;
        }
        if (Snapshot.Slot.State == EState::Quarantined)
        {
            ReportMisuse(EError::DoubleFree, "Free", User, Snapshot);
            return;
        }

        if (Snapshot.Slot.Serial == GetOptions().BreakOnSerial)
        {
            BreakForSerial(Snapshot.Slot.Serial, "is being freed");
        }

        FError Damage;
        if (!InspectAndRepair(User, false, "Free", Damage))
        {
            Report(Damage);
            if (Damage.Kind == EError::HeaderCorrupted)
            {
                // Nothing in the header can be trusted to release the block, so it leaks rather than frees garbage.
                FShard& Shard = ShardFor(Key);
                FScopeLock Lock(Shard.Mutex);
                if (FSlot* Slot = FindSlot(Shard, Key))
                {
                    EraseSlot(Shard, Slot);
                }
                return;
            }
        }

        FHeader* Header = HeaderOf(User);
        Header->FreeThread = Threading::GetThreadID();
        if (Header->Stacks != nullptr)
        {
            Header->Stacks->FreeDepth = CaptureStack(Header->Stacks->FreeFrames);
        }
        Header->Magic = kFreedMagic;
        Seal(*Header);
        std::memset(User, kFreeFill, Header->Size);

        const FQuarantined Entry{ User, Header->Base, Header->MappedSize, Header->Stacks, Header->Flags };
        const uint64 Size = Header->Size;

        if ((Entry.Flags & kPageBlockFlag) != 0)
        {
            ProtectNoAccess(Entry.Base, Entry.MappedSize - GetOptions().PageSize);
        }

        LiveBytes.fetch_sub(Size, std::memory_order_relaxed);
        LiveCount.fetch_sub(1, std::memory_order_relaxed);
        TotalFrees.fetch_add(1, std::memory_order_relaxed);

        Quarantine(Entry);
        MaybeValidate();
    }

    void* FDebugHeap::Reallocate(void* Memory, size_t NewSize, size_t Alignment)
    {
        if (Memory == nullptr)
        {
            return Allocate(NewSize, Alignment);
        }

        uint8* User = static_cast<uint8*>(Memory);
        const FSnapshot Snapshot = TakeSnapshot(reinterpret_cast<uintptr_t>(User), false);
        if (!Snapshot.bFound || Snapshot.Slot.State != EState::Live)
        {
            ReportMisuse(Snapshot.bFound ? EError::FreedPointerUsed : EError::InvalidPointer, "Reallocate", User, Snapshot);
            return nullptr;
        }

        FError Damage;
        if (!InspectAndRepair(User, false, "Reallocate", Damage))
        {
            Report(Damage);
            if (Damage.Kind == EError::HeaderCorrupted)
            {
                return nullptr;
            }
        }

        // Always a new block, so a pointer still held into the old buffer is caught rather than surviving by luck.
        void* Moved = Allocate(NewSize, Alignment);
        if (Moved == nullptr)
        {
            return nullptr;
        }
        std::memcpy(Moved, User, Snapshot.Slot.Size < NewSize ? Snapshot.Slot.Size : NewSize);
        Free(User);
        return Moved;
    }

    size_t FDebugHeap::GetAllocationSize(const void* Memory)
    {
        if (Memory == nullptr)
        {
            return 0;
        }

        const FSnapshot Snapshot = TakeSnapshot(reinterpret_cast<uintptr_t>(Memory), false);
        if (Snapshot.bFound && Snapshot.Slot.State == EState::Live)
        {
            return Snapshot.Slot.Size;
        }

        ReportMisuse(Snapshot.bFound ? EError::FreedPointerUsed : EError::InvalidPointer, "GetAllocationSize", Memory, Snapshot);
        return 0;
    }

    void FDebugHeap::Quarantine(const FQuarantined& Entry)
    {
        bool bQueued = false;
        {
            FScopeLock Lock(QuarantineMutex);
            if (RingCount == RingCapacity)
            {
                const uint64 NewCapacity = RingCapacity == 0 ? 4096 : RingCapacity * 2;
                if (FQuarantined* NewRing = static_cast<FQuarantined*>(std::malloc(NewCapacity * sizeof(FQuarantined))))
                {
                    for (uint64 Index = 0; Index < RingCount; ++Index)
                    {
                        NewRing[Index] = Ring[(RingHead + Index) % RingCapacity];
                    }
                    std::free(Ring);
                    Ring = NewRing;
                    RingCapacity = NewCapacity;
                    RingHead = 0;
                }
            }

            if (RingCount < RingCapacity)
            {
                Ring[(RingHead + RingCount) % RingCapacity] = Entry;
                ++RingCount;
                RingBytes += Entry.MappedSize;
                bQueued = true;
            }
        }

        if (!bQueued)
        {
            Release(Entry);
            return;
        }

        const FOptions& Settings = GetOptions();
        Evict(Settings.QuarantineBytes, Settings.QuarantineCount);
    }

    // Released outside the lock, because a damaged block's report allocates and that allocation frees into this queue.
    void FDebugHeap::Evict(uint64 KeepBytes, uint64 KeepCount)
    {
        for (;;)
        {
            FQuarantined Evicted[kEvictionBatch];
            uint32 EvictedCount = 0;
            {
                FScopeLock Lock(QuarantineMutex);
                while (EvictedCount < kEvictionBatch && RingCount > 0 && (RingBytes > KeepBytes || RingCount > KeepCount))
                {
                    Evicted[EvictedCount++] = Ring[RingHead];
                    RingBytes -= Ring[RingHead].MappedSize;
                    RingHead = (RingHead + 1) % RingCapacity;
                    --RingCount;
                }
            }

            for (uint32 Index = 0; Index < EvictedCount; ++Index)
            {
                Release(Evicted[Index]);
            }

            if (EvictedCount < kEvictionBatch)
            {
                return;
            }
        }
    }

    void FDebugHeap::Release(const FQuarantined& Entry)
    {
        if ((Entry.Flags & kPageBlockFlag) == 0)
        {
            FError Damage;
            if (!InspectAndRepair(Entry.User, true, "quarantine release", Damage))
            {
                Report(Damage);
            }
        }

        {
            const uintptr_t Key = reinterpret_cast<uintptr_t>(Entry.User);
            FShard& Shard = ShardFor(Key);
            FScopeLock Lock(Shard.Mutex);
            if (FSlot* Slot = FindSlot(Shard, Key); Slot != nullptr && Slot->State == EState::Quarantined)
            {
                EraseSlot(Shard, Slot);
            }
        }

        std::free(Entry.Stacks);
        ReleaseBlock(Entry.Base, Entry.MappedSize, Entry.Flags);
    }

    uint64 FDebugHeap::ValidateHeap()
    {
        FError Found[kMaxReportedPerPass];
        uint32 FoundCount = 0;
        uint64 Damaged = 0;

        const auto Record = [&](const FError& Damage)
        {
            ++Damaged;
            if (FoundCount < kMaxReportedPerPass)
            {
                Found[FoundCount++] = Damage;
            }
        };

        for (FShard& Shard : Shards)
        {
            FScopeLock Lock(Shard.Mutex);
            for (uint64 Index = 0; Index < Shard.Capacity; ++Index)
            {
                const FSlot& Slot = Shard.Slots[Index];
                FError Damage;
                if (Slot.State == EState::Live && !InspectAndRepair(reinterpret_cast<uint8*>(Slot.Key), false, "ValidateHeap", Damage))
                {
                    Record(Damage);
                }
            }
        }

        {
            FScopeLock Lock(QuarantineMutex);
            for (uint64 Index = 0; Index < RingCount; ++Index)
            {
                const FQuarantined& Entry = Ring[(RingHead + Index) % RingCapacity];
                FError Damage;
                if ((Entry.Flags & kPageBlockFlag) == 0 && !InspectAndRepair(Entry.User, true, "ValidateHeap", Damage))
                {
                    Record(Damage);
                }
            }
        }

        for (uint32 Index = 0; Index < FoundCount; ++Index)
        {
            Report(Found[Index]);
        }
        return Damaged;
    }

    void FDebugHeap::FlushQuarantine()
    {
        Evict(0, 0);
    }

    FErrorHandler FDebugHeap::SetErrorHandler(FErrorHandler NewHandler)
    {
        return Handler.exchange(NewHandler, std::memory_order_acq_rel);
    }

    FStats FDebugHeap::GetStats()
    {
        FStats Stats;
        Stats.LiveBytes = LiveBytes.load(std::memory_order_relaxed);
        Stats.LiveCount = LiveCount.load(std::memory_order_relaxed);
        Stats.PeakLiveBytes = PeakLiveBytes.load(std::memory_order_relaxed);
        Stats.TotalAllocations = TotalAllocations.load(std::memory_order_relaxed);
        Stats.TotalFrees = TotalFrees.load(std::memory_order_relaxed);
        Stats.ErrorsReported = ErrorsReported.load(std::memory_order_relaxed);
        Stats.bPageGuard = GetOptions().PageGuardEvery != 0;

        FScopeLock Lock(QuarantineMutex);
        Stats.QuarantineBytes = RingBytes;
        Stats.QuarantineCount = RingCount;
        return Stats;
    }

    bool FDebugHeap::DumpLiveAllocations(const char* Path)
    {
        FILE* File = std::fopen(Path, "w");
        if (File == nullptr)
        {
            return false;
        }

        const FStats Stats = GetStats();
        std::fprintf(File, "%llu live blocks, %llu bytes, peak %llu bytes. %llu quarantined, %llu bytes. %llu allocations, %llu frees, %llu errors.\n\n",
            static_cast<unsigned long long>(Stats.LiveCount), static_cast<unsigned long long>(Stats.LiveBytes),
            static_cast<unsigned long long>(Stats.PeakLiveBytes), static_cast<unsigned long long>(Stats.QuarantineCount),
            static_cast<unsigned long long>(Stats.QuarantineBytes), static_cast<unsigned long long>(Stats.TotalAllocations),
            static_cast<unsigned long long>(Stats.TotalFrees), static_cast<unsigned long long>(Stats.ErrorsReported));

        for (FShard& Shard : Shards)
        {
            FScopeLock Lock(Shard.Mutex);
            for (uint64 Index = 0; Index < Shard.Capacity; ++Index)
            {
                const FSlot& Slot = Shard.Slots[Index];
                if (Slot.State != EState::Live)
                {
                    continue;
                }

                const FHeader* Header = HeaderOf(reinterpret_cast<uint8*>(Slot.Key));
                std::fprintf(File, "%p  %llu bytes  #%llu  thread %llu\n", reinterpret_cast<void*>(Slot.Key),
                    static_cast<unsigned long long>(Slot.Size), static_cast<unsigned long long>(Slot.Serial),
                    static_cast<unsigned long long>(Header->AllocThread));
                if (Slot.Stacks != nullptr)
                {
                    for (uint32 Frame = 0; Frame < Slot.Stacks->AllocDepth; ++Frame)
                    {
                        std::fprintf(File, "    %p\n", Slot.Stacks->AllocFrames[Frame]);
                    }
                }
            }
        }

        std::fclose(File);
        return true;
    }

    constinit FDebugHeap GHeap;

    FAutoConsoleCommand GValidateHeapCommand("Memory.ValidateHeap",
        "Debug allocator: check every live and freed block for overruns, underruns and writes after free.", []
        {
            const uint64 Damaged = GHeap.ValidateHeap();
            LOG_INFO("Debug allocator: heap validated, {} damaged block(s).", Damaged);
        });

    FAutoConsoleCommand GFlushQuarantineCommand("Memory.FlushQuarantine",
        "Debug allocator: check and release every freed block held back to catch writes after free.", []
        {
            GHeap.FlushQuarantine();
            LOG_INFO("Debug allocator: quarantine flushed.");
        });

    FAutoConsoleCommand GDebugAllocatorStatsCommand("Memory.DebugAllocatorStats",
        "Debug allocator: live and quarantined bytes, totals and errors reported.", []
        {
            const FStats Stats = GHeap.GetStats();
            LOG_INFO("Debug allocator: {} live blocks ({} MB, peak {} MB), {} quarantined ({} MB), {} allocations, {} frees, {} errors{}.",
                Stats.LiveCount, Stats.LiveBytes >> 20, Stats.PeakLiveBytes >> 20, Stats.QuarantineCount, Stats.QuarantineBytes >> 20,
                Stats.TotalAllocations, Stats.TotalFrees, Stats.ErrorsReported, Stats.bPageGuard ? ", page guarded" : "");
        });
}

namespace Lumina::Memory::DebugAllocator
{
    void* Allocate(size_t Size, size_t Alignment)
    {
        return DebugAllocatorInternal::GHeap.Allocate(Size, Alignment);
    }

    void* Reallocate(void* Memory, size_t NewSize, size_t Alignment)
    {
        return DebugAllocatorInternal::GHeap.Reallocate(Memory, NewSize, Alignment);
    }

    void Free(void* Memory)
    {
        DebugAllocatorInternal::GHeap.Free(Memory);
    }

    size_t GetAllocationSize(const void* Memory)
    {
        return DebugAllocatorInternal::GHeap.GetAllocationSize(Memory);
    }

    FErrorHandler SetErrorHandler(FErrorHandler Handler)
    {
        return DebugAllocatorInternal::GHeap.SetErrorHandler(Handler);
    }

    uint64 ValidateHeap()
    {
        return DebugAllocatorInternal::GHeap.ValidateHeap();
    }

    void FlushQuarantine()
    {
        DebugAllocatorInternal::GHeap.FlushQuarantine();
    }

    FStats GetStats()
    {
        return DebugAllocatorInternal::GHeap.GetStats();
    }

    bool DumpLiveAllocations(const char* Path)
    {
        return DebugAllocatorInternal::GHeap.DumpLiveAllocations(Path);
    }

    const char* GetErrorName(EError Kind)
    {
        switch (Kind)
        {
        case EError::DoubleFree:        return "double free";
        case EError::FreedPointerUsed:  return "use of a freed block";
        case EError::InvalidPointer:    return "invalid pointer";
        case EError::HeaderCorrupted:   return "corrupted block header";
        case EError::Underrun:          return "buffer underrun";
        case EError::Overrun:           return "buffer overrun";
        case EError::UseAfterFree:      return "write after free";
        case EError::InvalidArgument:   return "invalid argument";
        }
        return "unknown error";
    }
}

#endif
