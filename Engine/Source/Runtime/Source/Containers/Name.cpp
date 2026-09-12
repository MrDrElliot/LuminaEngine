#include "RuntimePCH.h"
#include "Name.h"
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
        static constexpr size_t CHUNK_SIZE = 1024 * 1024; // 1MB

        const char* AllocateString(const char* Str, size_t Length);

        // Bytes handed out (sum of string lengths + null terminators); excludes chunk headers and slack.
        size_t GetUsedBytes() const { return TotalUsed; }
        // Bytes reserved from the allocator (chunk headers + capacities).
        size_t GetReservedBytes() const { return TotalReserved; }

        ~FStringPool();

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
        size_t TotalUsed = 0;
        size_t TotalReserved = 0;
    };

    /**
     * Interns names and hands out a DENSE INDEX as the identity.
     *
     * The index is what makes an FName 8 bytes instead of 16: a 64-bit hash needs all its bits to stay
     * collision-free, an index needs only as many as there are distinct names. Resolving one is an array
     * subscript rather than a hash lookup, so c_str() got cheaper too.
     *
     * The content hash is kept alongside, because it -- not the index -- is what is stable across runs, and
     * a couple of callers legitimately want that (a per-class colour, an ImGui dock class).
     */
    class FNameTable
    {
    public:
        
        FNameTable();
        
        uint32 GetOrCreateIndex(const char* Str, size_t Length);
        
        const char* GetString(uint32 Index) const;
        uint64 GetStableHash(uint32 Index) const;
        size_t GetMemoryUsage() const;
        
    private:
        
        size_t GetStringPoolUsage() const;
        
    private:
        
        FMutex Mutex;

        //~ Parallel, indexed by the name's index: its interned text and the hash it was found by.
        TVector<const char*> Strings;
        TVector<uint64>      Hashes;

        THashMap<uint64, uint32> HashToIndex;
        FStringPool Pool;
        
        static constexpr size_t INITIAL_CAPACITY = 16384;
        
    };
    
    
    FNameTable::FNameTable()
    {
        HashToIndex.reserve(INITIAL_CAPACITY);
        Strings.reserve(INITIAL_CAPACITY);
        Hashes.reserve(INITIAL_CAPACITY);

        // Index 0 is the empty name, so a default-constructed FName resolves without touching the table.
        Strings.push_back("NAME_None");
        Hashes.push_back(0);
    }

    uint32 FNameTable::GetOrCreateIndex(const char* Str, size_t Length)
    {
        if (!Str || !Str[0])
        {
            return 0;
        }
        
        char StackBuffer[256];
        char* Lower = StackBuffer;
        FString HeapBuffer;
        if (Length >= sizeof(StackBuffer))
        {
            HeapBuffer.resize(Length);
            Lower = HeapBuffer.data();
        }

        for (size_t i = 0; i < Length; ++i)
        {
            const char C = Str[i];
            Lower[i] = (C >= 'A' && C <= 'Z') ? char(C + ('a' - 'A')) : C;
        }

        const uint64 Hash = Hash::XXHash::GetHash64(Lower, Length);

        FScopeLock Lock(Mutex);

        // Probes on a hash hit whose text differs. The old table returned on the hash alone, so two unrelated
        // names that collided became the SAME name silently; at 64 bits that is vanishingly unlikely, but it
        // is the kind of unlikely that is impossible to diagnose when it happens.
        for (uint64 Key = Hash; ; ++Key)
        {
            auto It = HashToIndex.find(Key);
            if (It == HashToIndex.end())
            {
                const char* PermanentStr = Pool.AllocateString(Str, Length);

                const uint32 Index = (uint32)Strings.size();
                Strings.push_back(PermanentStr);
                Hashes.push_back(Hash);
                HashToIndex.insert_or_assign(Key, Index);
                return Index;
            }

            // Case-folded, matching the lowercased form the hash was taken of.
            const char* Existing = Strings[It->second];
            if (strlen(Existing) == Length && EqualsIgnoreCase(FStringView(Existing, Length), FStringView(Str, Length)))
            {
                return It->second;
            }
        }
    }

    const char* FNameTable::GetString(uint32 Index) const
    {
        return Index < Strings.size() ? Strings[Index] : nullptr;
    }

    uint64 FNameTable::GetStableHash(uint32 Index) const
    {
        return Index < Hashes.size() ? Hashes[Index] : 0;
    }

    size_t FNameTable::GetMemoryUsage() const
    {
        return HashToIndex.size() * (sizeof(uint64) + sizeof(uint32))
             + Strings.size() * (sizeof(const char*) + sizeof(uint64))
             + GetStringPoolUsage();
    }

    size_t FNameTable::GetStringPoolUsage() const
    {
        return Pool.GetUsedBytes();
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
        TotalReserved += BlockSize;

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
        TotalUsed += Need;

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
        bool TrySplitNumber(const char* Str, size_t Length, size_t& OutBaseLength, uint32& OutExternalNumber)
        {
            if (Length < 3)
            {
                return false;
            }

            size_t DigitStart = Length;
            while (DigitStart > 0 && Str[DigitStart - 1] >= '0' && Str[DigitStart - 1] <= '9')
            {
                --DigitStart;
            }

            // Need at least one digit, a preceding underscore, and a non-empty base before it.
            if (DigitStart == Length || DigitStart < 2 || Str[DigitStart - 1] != '_')
            {
                return false;
            }

            const size_t DigitCount = Length - DigitStart;

            // Reject leading zeros ("_05") but allow the single-digit "_0".
            if (DigitCount > 1 && Str[DigitStart] == '0')
            {
                return false;
            }

            // Parse with overflow guard against uint32.
            uint64 Value = 0;
            for (size_t i = DigitStart; i < Length; ++i)
            {
                Value = Value * 10 + uint64(Str[i] - '0');
                if (Value > 0xFFFFFFFFull)
                {
                    return false;
                }
            }

            OutExternalNumber = uint32(Value);
            OutBaseLength = DigitStart - 1; // drop the underscore too
            return true;
        }
    }

    FName::FName(const char* Str)
    {
        if (!Str || !Str[0])
        {
            return;
        }

        size_t Length = strlen(Str);
        uint32 ExternalNumber = 0;
        if (TrySplitNumber(Str, Length, Length, ExternalNumber))
        {
            Number = ExternalNumber + 1;
        }

        Index = GetNameTable().GetOrCreateIndex(Str, Length);
    }

    FName::FName(const char* Str, uint32 InNumber)
    {
        if (Str && Str[0])
        {
            Index = GetNameTable().GetOrCreateIndex(Str, strlen(Str));
        }

        Number = InNumber + 1;
    }

    const char* FName::c_str() const
    {
        const char* Base = GetNameTable().GetString(Index);

        if (Number == FName::kNoNumber)
        {
            return Base ? Base : "";
        }

        // A handful of overlapping c_str() calls in one format expression stay valid.
        static constexpr int BufferCount = 4;
        static constexpr int BufferSize = 256;
        static thread_local char Buffers[BufferCount][BufferSize];
        static thread_local int Next = 0;

        char* Out = Buffers[Next];
        Next = (Next + 1) % BufferCount;

        snprintf(Out, BufferSize, "%s_%u", Base ? Base : "", Number - 1);
        return Out;
    }

    void FName::AppendString(FString& Out) const
    {
        const char* Base = GetNameTable().GetString(Index);
        if (Base)
        {
            Out.append(Base);
        }

        if (Number != FName::kNoNumber)
        {
            Out.push_back('_');
            Out.append(Format("{}", Number - 1));
        }
    }

    void FName::ToString(FString& Out) const
    {
        Out.clear();
        AppendString(Out);
    }

    FString FName::ToString() const
    {
        FString Out;
        AppendString(Out);
        return Out;
    }

    size_t FName::Length() const
    {
        const char* Base = GetNameTable().GetString(Index);
        size_t Len = Base ? strlen(Base) : 0;

        if (Number != FName::kNoNumber)
        {
            uint32 Display = Number - 1;
            Len += 2; // underscore + at least one digit
            while (Display >= 10)
            {
                Display /= 10;
                ++Len;
            }
        }

        return Len;
    }

    uint64 FName::GetStableHash() const
    {
        return GetNameTable().GetStableHash(Index);
    }

    char FName::At(size_t Pos) const
    {
        if (Number == FName::kNoNumber)
        {
            const char* Str = GetNameTable().GetString(Index);
            size_t Len = Str ? strlen(Str) : 0;
            return (Pos < Len) ? Str[Pos] : '\0';
        }

        FString Str = ToString();
        return (Pos < Str.size()) ? Str[Pos] : '\0';
    }
}
