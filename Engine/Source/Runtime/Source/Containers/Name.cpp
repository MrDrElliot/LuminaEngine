#include "RuntimePCH.h"
#include "Name.h"

#include <atomic>
#include <charconv>

#include "Memory/Construct.h"

#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
    // An index plus a number, not a 64-bit hash plus a number. FName is a member of nearly every reflected
    // thing in the engine, so this is load-bearing for CObjectBase fitting in a cache line.
    static_assert(sizeof(FName) == 8, "FName grew; CObjectBase and FProperty are sized around it.");

    // Append-only arena for interned name strings.
    class FStringPool
    {
    public:
        // Default capacity reserved per chunk; a string larger than this gets its own exact-fit chunk.
        static constexpr size_t CHUNK_SIZE = Constants::kMiB;

        const char* AllocateString(const char* Str, size_t Length);

        ~FStringPool();
        FStringPool() = default;
        FStringPool(const FStringPool&) = delete;
        FStringPool& operator=(const FStringPool&) = delete;
        FStringPool(FStringPool&&) = delete;
        FStringPool& operator=(FStringPool&&) = delete;

    private:

        // A header followed immediately, in the same allocation, by Capacity bytes of string storage.
        struct Chunk
        {
            Chunk* Next;
            size_t Used;
            size_t Capacity;
        };

        static char* DataOf(Chunk* C) { return reinterpret_cast<char*>(C) + sizeof(Chunk); }

        Chunk* AllocateChunk(size_t Capacity);

        Chunk* Head = nullptr;
    };

    struct FPrehashedNameHash
    {
        NODISCARD FORCEINLINE static uint64 operator()(uint64 Hash) noexcept { return Hash; }
    };

    // One interned name, written once before its index is published and never moved after.
    struct FNameEntry
    {
        const char* Text;
        uint64      StableHash;
        int32       Length;
    };

    // Dense indices into blocks that never move, so a name resolves without the lock; the content hash is the run-stable identity.
    class FNameTable
    {
    public:

        FNameTable();

        uint32 GetOrCreateIndex(const char* Str, int32 Length);

        NODISCARD FORCEINLINE const FNameEntry* FindEntry(uint32 Index) const noexcept
        {
            if (Index >= NumEntries.load(std::memory_order_acquire))
            {
                return nullptr;
            }
            return &Blocks[Index >> kBlockShift].load(std::memory_order_acquire)[Index & (kBlockSize - 1)];
        }

    private:

        static constexpr uint32 kBlockShift = 12;
        static constexpr uint32 kBlockSize  = 1u << kBlockShift;
        static constexpr uint32 kMaxBlocks  = 1u << 12;
        static constexpr uint32 kNotFound   = ~0u;

        NODISCARD uint32 Probe(const char* Lower, int32 Length, uint64 Hash, uint64& OutFreeKey) const;
        uint32 Publish(const char* Str, int32 Length, uint64 StableHash);

        mutable FSharedMutex Mutex;
        THashMap<uint64, uint32, FPrehashedNameHash> HashToIndex;
        FStringPool Pool;
        std::atomic<FNameEntry*> Blocks[kMaxBlocks] = {};
        std::atomic<uint32> NumEntries = 0;
    };

    FNameTable::FNameTable()
    {
        HashToIndex.Reserve(16384);

        // Index 0 is the empty name, so a default-constructed FName resolves without touching the table.
        Publish("NAME_None", 9, 0);
    }

    // Probes on a hash hit whose text differs, so two names that collide stay two names; the key after the chain is OutFreeKey.
    uint32 FNameTable::Probe(const char* Lower, int32 Length, uint64 Hash, uint64& OutFreeKey) const
    {
        for (uint64 Key = Hash; ; ++Key)
        {
            const uint32* Found = HashToIndex.Find(Key);
            if (Found == nullptr)
            {
                OutFreeKey = Key;
                return kNotFound;
            }

            const FNameEntry* Entry = FindEntry(*Found);
            if (Entry->Length != Length)
            {
                continue;
            }

            int32 Index = 0;
            for (; Index < Length; ++Index)
            {
                const char C = Entry->Text[Index];
                const char Folded = (C >= 'A' && C <= 'Z') ? char(C + ('a' - 'A')) : C;
                if (Folded != Lower[Index])
                {
                    break;
                }
            }
            if (Index == Length)
            {
                return *Found;
            }
        }
    }

    uint32 FNameTable::Publish(const char* Str, int32 Length, uint64 StableHash)
    {
        const uint32 Index = NumEntries.load(std::memory_order_relaxed);
        LUMINA_CONTAINER_CHECK(Index < kMaxBlocks * kBlockSize);

        std::atomic<FNameEntry*>& Slot = Blocks[Index >> kBlockShift];
        FNameEntry* Block = Slot.load(std::memory_order_relaxed);
        if (Block == nullptr)
        {
            LUMINA_MEMORY_SCOPE("FName");
            Block = static_cast<FNameEntry*>(Memory::Malloc(sizeof(FNameEntry) * kBlockSize, alignof(FNameEntry)));
            Slot.store(Block, std::memory_order_release);
        }

        Block[Index & (kBlockSize - 1)] = FNameEntry{ Pool.AllocateString(Str, static_cast<size_t>(Length)), StableHash, Length };
        NumEntries.store(Index + 1, std::memory_order_release);
        return Index;
    }

    uint32 FNameTable::GetOrCreateIndex(const char* Str, int32 Length)
    {
        if (!Str || Length <= 0)
        {
            return 0;
        }

        char StackBuffer[256];
        char* Lower = StackBuffer;
        FString HeapBuffer;
        if (Length >= static_cast<int32>(sizeof(StackBuffer)))
        {
            HeapBuffer.SetLen(Length);
            Lower = HeapBuffer.GetData();
        }

        for (int32 i = 0; i < Length; ++i)
        {
            const char C = Str[i];
            Lower[i] = (C >= 'A' && C <= 'Z') ? char(C + ('a' - 'A')) : C;
        }

        const uint64 Hash = Hash::XXHash::GetHash64(Lower, static_cast<size_t>(Length));

        // Nearly every lookup after start-up is a hit, so those share the lock and only a new name takes it alone.
        uint64 FreeKey = 0;
        {
            FReadScopeLock Lock(Mutex);
            if (const uint32 Found = Probe(Lower, Length, Hash, FreeKey); Found != kNotFound)
            {
                return Found;
            }
        }

        FWriteScopeLock Lock(Mutex);
        if (const uint32 Found = Probe(Lower, Length, Hash, FreeKey); Found != kNotFound)
        {
            return Found;
        }

        const uint32 Index = Publish(Str, Length, Hash);
        HashToIndex.Add(FreeKey, Index);
        return Index;
    }

    static FNameTable& GetNameTable()
    {
        alignas(FNameTable) static uint8 Storage[sizeof(FNameTable)];
        static FNameTable* Table = Memory::ConstructAt(reinterpret_cast<FNameTable*>(Storage));
        return *Table;
    }

    FStringPool::Chunk* FStringPool::AllocateChunk(size_t Capacity)
    {
        LUMINA_MEMORY_SCOPE("FName");
        const size_t BlockSize = sizeof(Chunk) + Capacity;
        Chunk* NewChunk = static_cast<Chunk*>(Memory::Malloc(BlockSize, alignof(Chunk)));

        NewChunk->Next = Head;
        NewChunk->Used = 0;
        NewChunk->Capacity = Capacity;

        Head = NewChunk;

        return NewChunk;
    }

    const char* FStringPool::AllocateString(const char* Str, size_t Length)
    {
        // Tightly packed, just the bytes plus a terminator with no per-entry alignment padding.
        const size_t Need = Length + 1;

        if (!Head || Head->Used + Need > Head->Capacity)
        {
            // Oversized strings get a dedicated exact-fit chunk so they can never overflow a fixed one.
            AllocateChunk(Need > CHUNK_SIZE ? Need : CHUNK_SIZE);
        }

        char* Result = DataOf(Head) + Head->Used;
        memcpy(Result, Str, Length);
        Result[Length] = '\0';

        Head->Used += Need;

        return Result;
    }

    FStringPool::~FStringPool()
    {
        Chunk* C = Head;
        while (C)
        {
            Chunk* Next = C->Next;
            void* Block = C;
            Memory::Free(Block);
            C = Next;
        }
        Head = nullptr;
    }

    namespace
    {
        // Split a trailing "_<digits>" suffix off Str.
        bool TrySplitNumber(const char* Str, int32 Length, int32& OutBaseLength, uint32& OutExternalNumber)
        {
            if (Length < 3)
            {
                return false;
            }

            int32 DigitStart = Length;
            while (DigitStart > 0 && Str[DigitStart - 1] >= '0' && Str[DigitStart - 1] <= '9')
            {
                --DigitStart;
            }

            // Need at least one digit, a preceding underscore, and a non-empty base before it.
            if (DigitStart == Length || DigitStart < 2 || Str[DigitStart - 1] != '_')
            {
                return false;
            }

            const int32 DigitCount = Length - DigitStart;

            // Reject leading zeros ("_05") but allow the single-digit "_0".
            if (DigitCount > 1 && Str[DigitStart] == '0')
            {
                return false;
            }

            uint64 Value = 0;
            for (int32 i = DigitStart; i < Length; ++i)
            {
                Value = Value * 10 + uint64(Str[i] - '0');
                if (Value > 0xFFFFFFFFull)
                {
                    return false;
                }
            }

            OutExternalNumber = uint32(Value);
            OutBaseLength = DigitStart - 1;
            return true;
        }

        // A uint32 is at most ten digits.
        int32 WriteNumber(char* Out, uint32 Value) noexcept
        {
            return static_cast<int32>(std::to_chars(Out, Out + 10, Value).ptr - Out);
        }
    }

    FName::FName(const char* Str)
        : FName(FStringView(Str))
    {
    }

    FName::FName(FStringView Str)
    {
        if (Str.IsEmpty())
        {
            return;
        }

        int32 Length = Str.Len();
        uint32 ExternalNumber = 0;
        if (TrySplitNumber(Str.GetData(), Length, Length, ExternalNumber))
        {
            Number = ExternalNumber + 1;
        }

        Index = GetNameTable().GetOrCreateIndex(Str.GetData(), Length);
    }

    FName::FName(const char* Str, uint32 InNumber)
    {
        const FStringView Text(Str);
        if (!Text.IsEmpty())
        {
            Index = GetNameTable().GetOrCreateIndex(Text.GetData(), Text.Len());
        }

        Number = InNumber + 1;
    }

    const char* FName::operator*() const
    {
        const FNameEntry* Entry = GetNameTable().FindEntry(Index);
        if (Number == FName::kNoNumber)
        {
            return Entry != nullptr ? Entry->Text : "";
        }

        // A handful of overlapping operator* calls in one format expression stay valid.
        static constexpr int32 BufferCount = 4;
        static constexpr int32 BufferSize = 256;
        static thread_local char Buffers[BufferCount][BufferSize];
        static thread_local int32 Next = 0;

        char* Out = Buffers[Next];
        Next = (Next + 1) % BufferCount;

        // Room is kept for the underscore, ten digits and the terminator, so only an overlong base is cut short.
        const uint32 FullLength = Entry != nullptr ? static_cast<uint32>(Entry->Length) : 0u;
        constexpr uint32 MaxBase = static_cast<uint32>(BufferSize - 12);
        const int32 BaseLength = static_cast<int32>(FullLength < MaxBase ? FullLength : MaxBase);
        if (BaseLength != 0)
        {
            Memory::Memcpy(Out, Entry->Text, static_cast<size_t>(BaseLength));
        }
        Out[BaseLength] = '_';
        const int32 Digits = WriteNumber(Out + BaseLength + 1, Number - 1);
        Out[BaseLength + 1 + Digits] = '\0';
        return Out;
    }

    void FName::AppendString(FString& Out) const
    {
        if (const FNameEntry* Entry = GetNameTable().FindEntry(Index))
        {
            Out.Append(Entry->Text, Entry->Length);
        }

        if (Number != FName::kNoNumber)
        {
            char Suffix[11];
            Suffix[0] = '_';
            Out.Append(Suffix, 1 + WriteNumber(Suffix + 1, Number - 1));
        }
    }

    void FName::ToString(FString& Out) const
    {
        Out.Reset();
        AppendString(Out);
    }

    FString FName::ToString() const
    {
        FString Out;
        Out.Reserve(Len());
        AppendString(Out);
        return Out;
    }

    int32 FName::Len() const
    {
        const FNameEntry* Entry = GetNameTable().FindEntry(Index);
        int32 Length = Entry != nullptr ? Entry->Length : 0;

        if (Number != FName::kNoNumber)
        {
            uint32 Display = Number - 1;
            Length += 2;
            while (Display >= 10)
            {
                Display /= 10;
                ++Length;
            }
        }

        return Length;
    }

    uint64 FName::GetStableHash() const
    {
        const FNameEntry* Entry = GetNameTable().FindEntry(Index);
        return Entry != nullptr ? Entry->StableHash : 0;
    }

    char FName::At(int32 Pos) const
    {
        const FNameEntry* Entry = GetNameTable().FindEntry(Index);
        const int32 BaseLength = Entry != nullptr ? Entry->Length : 0;
        if (Pos < 0)
        {
            return '\0';
        }
        if (Pos < BaseLength)
        {
            return Entry->Text[Pos];
        }
        if (Number == FName::kNoNumber)
        {
            return '\0';
        }
        if (Pos == BaseLength)
        {
            return '_';
        }

        char Digits[10];
        const int32 Count = WriteNumber(Digits, Number - 1);
        const int32 Digit = Pos - BaseLength - 1;
        return Digit < Count ? Digits[Digit] : '\0';
    }
}
