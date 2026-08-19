#include "RuntimePCH.h"
#include "Name.h"

#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
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

    class FNameTable
    {
    public:
        
        FNameTable();
        
        uint64 GetOrCreateID(const char* Str, size_t Length);
        
        const char* GetString(uint64 ID) const;
        size_t GetMemoryUsage() const;
        
    private:
        
        size_t GetStringPoolUsage() const;
        
    private:
        
        FMutex Mutex;
        THashMap<uint64, const char*> HashToString;
        FStringPool Pool;
        
        static constexpr size_t INITIAL_CAPACITY = 16384;
        
    };
    
    
    FNameTable::FNameTable()
    {
        HashToString.reserve(INITIAL_CAPACITY);
        HashToString.insert_or_assign(0, "NAME_None");
    }

    uint64 FNameTable::GetOrCreateID(const char* Str, size_t Length)
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

        const uint64 ID = Hash::XXHash::GetHash64(Lower, Length);

        FScopeLock Lock(Mutex);
            
        auto It = HashToString.find(ID);
        if (It != HashToString.end())
        {
            return ID;
        }
            
        const char* PermanentStr = Pool.AllocateString(Str, Length);
            
        HashToString.insert_or_assign(ID, PermanentStr);
            
        return ID;
    }

    const char* FNameTable::GetString(uint64 ID) const
    {
        auto It = HashToString.find(ID);
        return (It != HashToString.end()) ? It->second : nullptr;
    }

    size_t FNameTable::GetMemoryUsage() const
    {
        return HashToString.size() * (sizeof(uint64) + sizeof(char*)) + GetStringPoolUsage();
    }

    size_t FNameTable::GetStringPoolUsage() const
    {
        return Pool.GetUsedBytes();
    }
    
    static FNameTable& GetNameTable()
    {
        alignas(FNameTable) static uint8 Storage[sizeof(FNameTable)];
        static FNameTable* Table = new (Storage) FNameTable{};
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
        // Tightly packed: just the bytes plus a null terminator, no per-entry alignment padding.
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

        ID = GetNameTable().GetOrCreateID(Str, Length);
    }

    FName::FName(const char* Str, uint32 InNumber)
    {
        if (Str && Str[0])
        {
            ID = GetNameTable().GetOrCreateID(Str, strlen(Str));
        }

        Number = InNumber + 1;
    }

    const char* FName::c_str() const
    {
        const char* Base = GetNameTable().GetString(ID);

        if (Number == FName::kNoNumber)
        {
            return Base ? Base : "";
        }

        // Numbered names are rendered into a small per-thread rotating set of buffers so a handful of
        // overlapping c_str() calls (e.g. inside a single format expression) stay valid.
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
        const char* Base = GetNameTable().GetString(ID);
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
        const char* Base = GetNameTable().GetString(ID);
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

    char FName::At(size_t Pos) const
    {
        if (Number == FName::kNoNumber)
        {
            const char* Str = GetNameTable().GetString(ID);
            size_t Len = Str ? strlen(Str) : 0;
            return (Pos < Len) ? Str[Pos] : '\0';
        }

        FString Str = ToString();
        return (Pos < Str.size()) ? Str[Pos] : '\0';
    }
}
