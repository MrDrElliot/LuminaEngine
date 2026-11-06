#include "RuntimePCH.h"
#include "Json.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>

#include "Containers/HashPrimitives.h"
#include "Containers/StringFormat.h"
#include "Memory/Memory.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    #include <emmintrin.h>
    #define LUMINA_JSON_SSE2 1
#else
    #define LUMINA_JSON_SSE2 0
#endif

namespace Lumina
{
    namespace JsonInternal
    {
        constexpr int32 kIndexThreshold = 16;
        constexpr int32 kMaxDepth       = 512;

        // Each container is one allocation, this header then its elements; an empty container has no block at all.
        struct FArrayBlock
        {
            int32 Num;
            int32 Max;

            FORCEINLINE FJsonValue* Items() noexcept { return reinterpret_cast<FJsonValue*>(this + 1); }
        };

        struct FObjectBlock
        {
            int32  Num;
            int32  Max;
            int32* Index;
            int32  IndexSlots;
            int32  Unused;

            FORCEINLINE FJsonMember* Items() noexcept { return reinterpret_cast<FJsonMember*>(this + 1); }
        };

        static_assert(sizeof(FArrayBlock) % alignof(FJsonValue) == 0);
        static_assert(sizeof(FObjectBlock) % alignof(FJsonMember) == 0);

        const FJsonValue& NullValue()
        {
            static const FJsonValue Null;
            return Null;
        }

        FORCEINLINE uint64 HashKey(FStringView Key)
        {
            return Containers::HashBytes(Key.GetData(), static_cast<size_t>(Key.Len()));
        }

        FORCEINLINE bool KeyEquals(const FJsonString& Stored, FStringView Key)
        {
            return Stored.Len() == Key.Len() && Memory::Memcmp(Stored.CStr(), Key.GetData(), static_cast<size_t>(Key.Len())) == 0;
        }

        FORCEINLINE int32 GrownCapacity(int32 Max, int32 Needed)
        {
            int32 Grown = Max < 4 ? 4 : Max * 2;
            return Grown < Needed ? Needed : Grown;
        }

        FArrayBlock* ResizeArrayBlock(FArrayBlock* Block, int32 Max)
        {
            const int32 Num = Block != nullptr ? Block->Num : 0;
            Block = static_cast<FArrayBlock*>(Memory::Realloc(Block, sizeof(FArrayBlock) + static_cast<size_t>(Max) * sizeof(FJsonValue)));
            Block->Num = Num;
            Block->Max = Max;
            return Block;
        }

        FObjectBlock* ResizeObjectBlock(FObjectBlock* Block, int32 Max)
        {
            const bool bFresh = Block == nullptr;
            Block = static_cast<FObjectBlock*>(Memory::Realloc(Block, sizeof(FObjectBlock) + static_cast<size_t>(Max) * sizeof(FJsonMember)));
            if (bFresh)
            {
                Block->Num = 0;
                Block->Index = nullptr;
                Block->IndexSlots = 0;
                Block->Unused = 0;
            }
            Block->Max = Max;
            return Block;
        }

        void FreeIndex(FObjectBlock& Object) noexcept
        {
            if (Object.Index != nullptr)
            {
                void* Index = Object.Index;
                Memory::Free(Index);
                Object.Index = nullptr;
                Object.IndexSlots = 0;
            }
        }

        void DestroyArrayBlock(FArrayBlock* Block) noexcept
        {
            if (Block == nullptr)
            {
                return;
            }
            FJsonValue* Items = Block->Items();
            for (int32 Index = 0; Index < Block->Num; ++Index)
            {
                Items[Index].~FJsonValue();
            }
            void* Memory = Block;
            Memory::Free(Memory);
        }

        void DestroyObjectBlock(FObjectBlock* Block) noexcept
        {
            if (Block == nullptr)
            {
                return;
            }
            FJsonMember* Items = Block->Items();
            for (int32 Index = 0; Index < Block->Num; ++Index)
            {
                Items[Index].~FJsonMember();
            }
            FreeIndex(*Block);
            void* Memory = Block;
            Memory::Free(Memory);
        }

        FArrayBlock* CopyArrayBlock(const FArrayBlock* Source)
        {
            if (Source == nullptr || Source->Num == 0)
            {
                return nullptr;
            }
            FArrayBlock* Block = ResizeArrayBlock(nullptr, Source->Num);
            const FJsonValue* From = const_cast<FArrayBlock*>(Source)->Items();
            FJsonValue* To = Block->Items();
            for (int32 Index = 0; Index < Source->Num; ++Index)
            {
                new (To + Index) FJsonValue(From[Index]);
            }
            Block->Num = Source->Num;
            return Block;
        }

        FObjectBlock* CopyObjectBlock(const FObjectBlock* Source)
        {
            if (Source == nullptr || Source->Num == 0)
            {
                return nullptr;
            }
            FObjectBlock* Block = ResizeObjectBlock(nullptr, Source->Num);
            const FJsonMember* From = const_cast<FObjectBlock*>(Source)->Items();
            FJsonMember* To = Block->Items();
            for (int32 Index = 0; Index < Source->Num; ++Index)
            {
                new (To + Index) FJsonMember(From[Index]);
            }
            Block->Num = Source->Num;
            if (Source->Index != nullptr)
            {
                Block->Index = static_cast<int32*>(Memory::Malloc(static_cast<size_t>(Source->IndexSlots) * sizeof(int32)));
                Memory::Memcpy(Block->Index, Source->Index, static_cast<size_t>(Source->IndexSlots) * sizeof(int32));
                Block->IndexSlots = Source->IndexSlots;
            }
            return Block;
        }

        void PlaceInIndex(FObjectBlock& Object, int32 MemberIndex)
        {
            const uint64 Mask = static_cast<uint64>(Object.IndexSlots - 1);
            uint64 Slot = HashKey(Object.Items()[MemberIndex].Key.View()) & Mask;
            while (Object.Index[Slot] != 0)
            {
                Slot = (Slot + 1) & Mask;
            }
            Object.Index[Slot] = MemberIndex + 1;
        }

        void AllocateIndex(FObjectBlock& Object)
        {
            int32 Slots = 32;
            while (Slots < Object.Num * 2)
            {
                Slots *= 2;
            }
            FreeIndex(Object);
            Object.Index = static_cast<int32*>(Memory::Malloc(static_cast<size_t>(Slots) * sizeof(int32)));
            std::memset(Object.Index, 0, static_cast<size_t>(Slots) * sizeof(int32));
            Object.IndexSlots = Slots;
        }

        void RebuildIndex(FObjectBlock& Object)
        {
            if (Object.Num < kIndexThreshold)
            {
                FreeIndex(Object);
                return;
            }
            AllocateIndex(Object);
            for (int32 Member = 0; Member < Object.Num; ++Member)
            {
                PlaceInIndex(Object, Member);
            }
        }

        void IndexAppended(FObjectBlock& Object)
        {
            if (Object.Index == nullptr)
            {
                if (Object.Num >= kIndexThreshold)
                {
                    RebuildIndex(Object);
                }
                return;
            }
            if (Object.Num * 2 > Object.IndexSlots)
            {
                RebuildIndex(Object);
                return;
            }
            PlaceInIndex(Object, Object.Num - 1);
        }

        int32 FindMember(const FObjectBlock* Block, FStringView Key)
        {
            if (Block == nullptr)
            {
                return Constants::kIndexNone;
            }
            FObjectBlock& Object = *const_cast<FObjectBlock*>(Block);
            const FJsonMember* Members = Object.Items();
            if (Object.Index == nullptr)
            {
                for (int32 Member = 0; Member < Object.Num; ++Member)
                {
                    if (KeyEquals(Members[Member].Key, Key))
                    {
                        return Member;
                    }
                }
                return Constants::kIndexNone;
            }

            const uint64 Mask = static_cast<uint64>(Object.IndexSlots - 1);
            for (uint64 Slot = HashKey(Key) & Mask;; Slot = (Slot + 1) & Mask)
            {
                const int32 Entry = Object.Index[Slot];
                if (Entry == 0)
                {
                    return Constants::kIndexNone;
                }
                if (KeyEquals(Members[Entry - 1].Key, Key))
                {
                    return Entry - 1;
                }
            }
        }

        // The block may move, so the caller stores it back into the value.
        FJsonValue& AppendMember(FObjectBlock*& Block, FJsonString&& Key, FJsonValue&& Value)
        {
            if (Block == nullptr || Block->Num == Block->Max)
            {
                Block = ResizeObjectBlock(Block, GrownCapacity(Block != nullptr ? Block->Max : 0, (Block != nullptr ? Block->Num : 0) + 1));
            }
            FJsonMember* Placed = new (Block->Items() + Block->Num) FJsonMember{ Move(Key), Move(Value) };
            ++Block->Num;
            IndexAppended(*Block);
            return Placed->Value;
        }

        FJsonValue& AppendElement(FArrayBlock*& Block, FJsonValue&& Value)
        {
            if (Block == nullptr || Block->Num == Block->Max)
            {
                Block = ResizeArrayBlock(Block, GrownCapacity(Block != nullptr ? Block->Max : 0, (Block != nullptr ? Block->Num : 0) + 1));
            }
            FJsonValue* Placed = new (Block->Items() + Block->Num) FJsonValue(Move(Value));
            ++Block->Num;
            return *Placed;
        }

        // Elements shift down bitwise, which is a valid move for these trivially relocatable types.
        template <typename T>
        void EraseAt(T* Items, int32& Num, int32 Index)
        {
            Items[Index].~T();
            std::memmove(static_cast<void*>(Items + Index), Items + Index + 1, static_cast<size_t>(Num - Index - 1) * sizeof(T));
            --Num;
        }

        // The byte length of the valid UTF-8 sequence at Cursor, or 0 when it is malformed, overlong, a surrogate or past U+10FFFF.
        FORCEINLINE int32 ValidUtf8Length(const uint8* Cursor, const uint8* End)
        {
            const uint8 Lead = Cursor[0];
            const int64 Left = End - Cursor;
            auto Continues = [](uint8 Byte) { return (Byte & 0xC0) == 0x80; };
            if (Lead < 0xC2)
            {
                return 0;
            }
            if (Lead < 0xE0)
            {
                return Left >= 2 && Continues(Cursor[1]) ? 2 : 0;
            }
            if (Lead < 0xF0)
            {
                if (Left < 3)
                {
                    return 0;
                }
                const uint8 Second = Cursor[1];
                const bool bSecond = Lead == 0xE0 ? (Second >= 0xA0 && Second <= 0xBF)
                                   : Lead == 0xED ? (Second >= 0x80 && Second <= 0x9F)
                                   : Continues(Second);
                return bSecond && Continues(Cursor[2]) ? 3 : 0;
            }
            if (Lead < 0xF5)
            {
                if (Left < 4)
                {
                    return 0;
                }
                const uint8 Second = Cursor[1];
                const bool bSecond = Lead == 0xF0 ? (Second >= 0x90 && Second <= 0xBF)
                                   : Lead == 0xF4 ? (Second >= 0x80 && Second <= 0x8F)
                                   : Continues(Second);
                return bSecond && Continues(Cursor[2]) && Continues(Cursor[3]) ? 4 : 0;
            }
            return 0;
        }

        // Offset of the first byte that is a quote, a backslash, a control character or non-ASCII, or Length when none is.
        FORCEINLINE int64 FindSpecial(const char* Text, int64 Length)
        {
            int64 Offset = 0;
            #if LUMINA_JSON_SSE2
            const __m128i Quote     = _mm_set1_epi8('"');
            const __m128i Backslash = _mm_set1_epi8('\\');
            const __m128i Space     = _mm_set1_epi8(0x20);
            for (; Offset + 16 <= Length; Offset += 16)
            {
                const __m128i Chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Text + Offset));
                // Signed compare, so bytes at or above 0x80 count as below a space along with the control characters.
                const __m128i Hit = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(Chunk, Quote), _mm_cmpeq_epi8(Chunk, Backslash)), _mm_cmplt_epi8(Chunk, Space));
                if (const int Mask = _mm_movemask_epi8(Hit))
                {
                    return Offset + std::countr_zero(static_cast<uint32>(Mask));
                }
            }
            #endif
            for (; Offset < Length; ++Offset)
            {
                const uint8 Byte = static_cast<uint8>(Text[Offset]);
                if (Byte == '"' || Byte == '\\' || Byte < 0x20 || Byte >= 0x80)
                {
                    return Offset;
                }
            }
            return Length;
        }

        FORCEINLINE int32 HexValue(char C)
        {
            if (C >= '0' && C <= '9') return C - '0';
            if (C >= 'a' && C <= 'f') return C - 'a' + 10;
            if (C >= 'A' && C <= 'F') return C - 'A' + 10;
            return -1;
        }

        void AppendUtf8(FString& Out, uint32 CodePoint)
        {
            if (CodePoint < 0x80)
            {
                Out.AppendChar(static_cast<char>(CodePoint));
            }
            else if (CodePoint < 0x800)
            {
                Out.AppendChar(static_cast<char>(0xC0 | (CodePoint >> 6)));
                Out.AppendChar(static_cast<char>(0x80 | (CodePoint & 0x3F)));
            }
            else if (CodePoint < 0x10000)
            {
                Out.AppendChar(static_cast<char>(0xE0 | (CodePoint >> 12)));
                Out.AppendChar(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
                Out.AppendChar(static_cast<char>(0x80 | (CodePoint & 0x3F)));
            }
            else
            {
                Out.AppendChar(static_cast<char>(0xF0 | (CodePoint >> 18)));
                Out.AppendChar(static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3F)));
                Out.AppendChar(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
                Out.AppendChar(static_cast<char>(0x80 | (CodePoint & 0x3F)));
            }
        }

        FORCEINLINE int64 ClampToInt64(double Value)
        {
            if (std::isnan(Value)) return 0;
            if (Value >= 9223372036854775807.0) return std::numeric_limits<int64>::max();
            if (Value <= -9223372036854775808.0) return std::numeric_limits<int64>::min();
            return static_cast<int64>(Value);
        }

        FORCEINLINE uint64 ClampToUInt64(double Value)
        {
            if (std::isnan(Value) || Value <= 0.0) return 0;
            if (Value >= 18446744073709551615.0) return std::numeric_limits<uint64>::max();
            return static_cast<uint64>(Value);
        }
    }

    namespace JsonInternal
    {
        // Children wait here while their container parses, so every container is allocated once at its exact size.
        template <typename T>
        struct TParseStack
        {
            T*    Data = nullptr;
            int32 Num  = 0;
            int32 Max  = 0;

            TParseStack() = default;
            TParseStack(const TParseStack&) = delete;
            TParseStack& operator=(const TParseStack&) = delete;

            ~TParseStack()
            {
                DestroyFrom(0);
                if (Data != nullptr)
                {
                    void* Block = Data;
                    Memory::Free(Block);
                }
            }

            FORCEINLINE void Push(T&& Value)
            {
                if (Num == Max) [[unlikely]]
                {
                    Max = Max == 0 ? 64 : Max * 2;
                    Data = static_cast<T*>(Memory::Realloc(Data, static_cast<size_t>(Max) * sizeof(T)));
                }
                new (Data + Num) T(Move(Value));
                ++Num;
            }

            T* Release(int32 Base) noexcept
            {
                T* First = Data + Base;
                Num = Base;
                return First;
            }

            void DestroyFrom(int32 Base) noexcept
            {
                for (int32 Index = Base; Index < Num; ++Index)
                {
                    Data[Index].~T();
                }
                Num = Base;
            }
        };

        FArrayBlock* TakeArray(TParseStack<FJsonValue>& Stack, int32 Base)
        {
            const int32 Count = Stack.Num - Base;
            FArrayBlock* Block = ResizeArrayBlock(nullptr, Count);
            Memory::Memcpy(static_cast<void*>(Block->Items()), Stack.Release(Base), static_cast<size_t>(Count) * sizeof(FJsonValue));
            Block->Num = Count;
            return Block;
        }

        FObjectBlock* TakeObject(TParseStack<FJsonMember>& Stack, int32 Base)
        {
            const int32 Count = Stack.Num - Base;
            FObjectBlock* Block = ResizeObjectBlock(nullptr, Count);
            Memory::Memcpy(static_cast<void*>(Block->Items()), Stack.Release(Base), static_cast<size_t>(Count) * sizeof(FJsonMember));
            Block->Num = Count;
            return Block;
        }

        // A parsed object past the linear-scan size: build the index, folding each repeated key into its first slot.
        void IndexParsedObject(FObjectBlock& Object)
        {
            AllocateIndex(Object);
            const uint64 Mask = static_cast<uint64>(Object.IndexSlots - 1);
            FJsonMember* Members = Object.Items();

            TVector<int32> Folded;
            for (int32 Member = 0; Member < Object.Num; ++Member)
            {
                const FStringView Key = Members[Member].Key.View();
                uint64 Slot = HashKey(Key) & Mask;
                int32 Entry;
                while ((Entry = Object.Index[Slot]) != 0 && !KeyEquals(Members[Entry - 1].Key, Key))
                {
                    Slot = (Slot + 1) & Mask;
                }
                if (Entry == 0)
                {
                    Object.Index[Slot] = Member + 1;
                    continue;
                }
                Members[Entry - 1].Value = Move(Members[Member].Value);
                Folded.Add(Member);
            }

            for (int32 Index = Folded.Num() - 1; Index >= 0; --Index)
            {
                EraseAt(Members, Object.Num, Folded[Index]);
            }
            if (!Folded.IsEmpty())
            {
                RebuildIndex(Object);
            }
        }
    }

    //~ FJsonString

    FJsonString::FJsonString(FStringView Text)
    {
        Assign(Text.GetData(), Text.Len());
    }

    FJsonString::FJsonString(const FJsonString& Other)
    {
        if (Other.IsHeap())
        {
            Assign(Other.CStr(), Other.Len());
        }
        else
        {
            Memory::Memcpy(Data, Other.Data, sizeof(Data));
        }
    }

    FJsonString::FJsonString(FJsonString&& Other) noexcept
    {
        Memory::Memcpy(Data, Other.Data, sizeof(Data));
        Other.Data[0] = 0;
        Other.Data[kTagIndex] = 0;
    }

    FJsonString& FJsonString::operator=(const FJsonString& Other)
    {
        if (this != &Other)
        {
            FJsonString Copy(Other);
            *this = Move(Copy);
        }
        return *this;
    }

    FJsonString& FJsonString::operator=(FJsonString&& Other) noexcept
    {
        if (this != &Other)
        {
            Release();
            Memory::Memcpy(Data, Other.Data, sizeof(Data));
            Other.Data[0] = 0;
            Other.Data[kTagIndex] = 0;
        }
        return *this;
    }

    void FJsonString::Assign(const char* Text, int32 Length)
    {
        if (Length <= kInlineCapacity)
        {
            if (Length > 0)
            {
                Memory::Memcpy(Data, Text, static_cast<size_t>(Length));
            }
            Data[Length] = 0;
            Data[kTagIndex] = static_cast<char>(Length);
            return;
        }

        char* Heap = static_cast<char*>(Memory::Malloc(static_cast<size_t>(Length) + 1));
        Memory::Memcpy(Heap, Text, static_cast<size_t>(Length));
        Heap[Length] = 0;
        Memory::Memcpy(Data, &Heap, sizeof(Heap));
        Memory::Memcpy(Data + 8, &Length, sizeof(Length));
        Data[kTagIndex] = static_cast<char>(kHeapMarker);
    }

    void FJsonString::Release() noexcept
    {
        if (IsHeap())
        {
            void* Heap;
            Memory::Memcpy(&Heap, Data, sizeof(Heap));
            Memory::Free(Heap);
            Data[0] = 0;
            Data[kTagIndex] = 0;
        }
    }

    //~ FJsonValue construction and lifetime

    FJsonValue::FJsonValue(FStringView Text)
    {
        new (Storage) FJsonString(Text);
        Type = EJsonType::String;
    }

    FJsonValue::FJsonValue(const FJsonString& Text)
    {
        new (Storage) FJsonString(Text);
        Type = EJsonType::String;
    }

    FJsonValue::FJsonValue(const FJsonValue& Other)
    {
        Type = EJsonType::Null;
        CopyFrom(Other);
    }

    FJsonValue::FJsonValue(FJsonValue&& Other) noexcept
    {
        Memory::Memcpy(Storage, Other.Storage, sizeof(Storage));
        Type = Other.Type;
        Other.Type = EJsonType::Null;
    }

    FJsonValue& FJsonValue::operator=(const FJsonValue& Other)
    {
        if (this != &Other)
        {
            FJsonValue Copy(Other);
            *this = Move(Copy);
        }
        return *this;
    }

    // Other is detached first, since it may live inside the tree this assignment is about to free.
    FJsonValue& FJsonValue::operator=(FJsonValue&& Other) noexcept
    {
        if (this != &Other)
        {
            FJsonValue Detached(Move(Other));
            Release();
            Memory::Memcpy(Storage, Detached.Storage, sizeof(Storage));
            Type = Detached.Type;
            Detached.Type = EJsonType::Null;
        }
        return *this;
    }

    void FJsonValue::CopyFrom(const FJsonValue& Other)
    {
        switch (Other.Type)
        {
        case EJsonType::String:
            new (Storage) FJsonString(Other.StringPayload());
            break;
        case EJsonType::Array:
            SetPayload(JsonInternal::CopyArrayBlock(static_cast<const JsonInternal::FArrayBlock*>(Other.PointerPayload())));
            break;
        case EJsonType::Object:
            SetPayload(JsonInternal::CopyObjectBlock(static_cast<const JsonInternal::FObjectBlock*>(Other.PointerPayload())));
            break;
        default:
            Memory::Memcpy(Storage, Other.Storage, sizeof(Storage));
            break;
        }
        Type = Other.Type;
    }

    void FJsonValue::Release() noexcept
    {
        switch (Type)
        {
        case EJsonType::String:
            StringPayload().~FJsonString();
            break;
        case EJsonType::Array:
            JsonInternal::DestroyArrayBlock(static_cast<JsonInternal::FArrayBlock*>(PointerPayload()));
            break;
        case EJsonType::Object:
            JsonInternal::DestroyObjectBlock(static_cast<JsonInternal::FObjectBlock*>(PointerPayload()));
            break;
        default:
            break;
        }
        Type = EJsonType::Null;
    }

    void FJsonValue::EnsureArray()
    {
        if (Type == EJsonType::Array)
        {
            return;
        }
        LUMINA_CONTAINER_CHECK(Type == EJsonType::Null);
        Release();
        SetPayload<void*>(nullptr);
        Type = EJsonType::Array;
    }

    void FJsonValue::EnsureObject()
    {
        if (Type == EJsonType::Object)
        {
            return;
        }
        LUMINA_CONTAINER_CHECK(Type == EJsonType::Null);
        Release();
        SetPayload<void*>(nullptr);
        Type = EJsonType::Object;
    }

    FJsonValue FJsonValue::MakeArray(int32 Reserve)
    {
        FJsonValue Value;
        Value.EnsureArray();
        Value.Reserve(Reserve);
        return Value;
    }

    FJsonValue FJsonValue::MakeArray(std::initializer_list<FJsonValue> Items)
    {
        FJsonValue Value = MakeArray(static_cast<int32>(Items.size()));
        for (const FJsonValue& Item : Items)
        {
            Value.Add(Item);
        }
        return Value;
    }

    FJsonValue FJsonValue::MakeObject(int32 Reserve)
    {
        FJsonValue Value;
        Value.EnsureObject();
        Value.Reserve(Reserve);
        return Value;
    }

    FJsonValue FJsonValue::MakeObject(std::initializer_list<TPair<FStringView, FJsonValue>> Members)
    {
        FJsonValue Value = MakeObject(static_cast<int32>(Members.size()));
        for (const TPair<FStringView, FJsonValue>& Member : Members)
        {
            Value.Set(Member.Key, Member.Value);
        }
        return Value;
    }

    //~ Reading

    int64 FJsonValue::AsInt64(int64 Default) const noexcept
    {
        switch (Type)
        {
        case EJsonType::Int:    return GetPayload<int64>();
        case EJsonType::UInt:   return static_cast<int64>(GetPayload<uint64>());
        case EJsonType::Double: return JsonInternal::ClampToInt64(GetPayload<double>());
        default:                return Default;
        }
    }

    uint64 FJsonValue::AsUInt64(uint64 Default) const noexcept
    {
        switch (Type)
        {
        case EJsonType::Int:    return static_cast<uint64>(GetPayload<int64>());
        case EJsonType::UInt:   return GetPayload<uint64>();
        case EJsonType::Double: return JsonInternal::ClampToUInt64(GetPayload<double>());
        default:                return Default;
        }
    }

    double FJsonValue::AsDouble(double Default) const noexcept
    {
        switch (Type)
        {
        case EJsonType::Int:    return static_cast<double>(GetPayload<int64>());
        case EJsonType::UInt:   return static_cast<double>(GetPayload<uint64>());
        case EJsonType::Double: return GetPayload<double>();
        default:                return Default;
        }
    }

    bool FJsonValue::TryGet(bool& Out) const noexcept
    {
        if (!IsBool())
        {
            return false;
        }
        Out = GetPayload<bool>();
        return true;
    }

    bool FJsonValue::TryGet(int32& Out) const noexcept
    {
        if (!IsNumber())
        {
            return false;
        }
        Out = static_cast<int32>(AsInt64());
        return true;
    }

    bool FJsonValue::TryGet(int64& Out) const noexcept
    {
        if (!IsNumber())
        {
            return false;
        }
        Out = AsInt64();
        return true;
    }

    bool FJsonValue::TryGet(uint32& Out) const noexcept
    {
        if (!IsNumber())
        {
            return false;
        }
        Out = static_cast<uint32>(AsUInt64());
        return true;
    }

    bool FJsonValue::TryGet(uint64& Out) const noexcept
    {
        if (!IsNumber())
        {
            return false;
        }
        Out = AsUInt64();
        return true;
    }

    bool FJsonValue::TryGet(float& Out) const noexcept
    {
        if (!IsNumber())
        {
            return false;
        }
        Out = static_cast<float>(AsDouble());
        return true;
    }

    bool FJsonValue::TryGet(double& Out) const noexcept
    {
        if (!IsNumber())
        {
            return false;
        }
        Out = AsDouble();
        return true;
    }

    bool FJsonValue::TryGet(FStringView& Out) const noexcept
    {
        if (!IsString())
        {
            return false;
        }
        Out = StringPayload().View();
        return true;
    }

    bool FJsonValue::TryGet(FString& Out) const
    {
        if (!IsString())
        {
            return false;
        }
        Out = StringPayload().ToString();
        return true;
    }

    FString FJsonValue::GetString(FStringView Key, FStringView Default) const
    {
        const FJsonValue* Member = Find(Key);
        return Member != nullptr && Member->IsString() ? Member->StringPayload().ToString() : FString(Default);
    }

    int32 FJsonValue::Num() const noexcept
    {
        if (Type != EJsonType::Array && Type != EJsonType::Object)
        {
            return 0;
        }
        const void* Block = PointerPayload();
        if (Block == nullptr)
        {
            return 0;
        }
        return Type == EJsonType::Array ? static_cast<const JsonInternal::FArrayBlock*>(Block)->Num : static_cast<const JsonInternal::FObjectBlock*>(Block)->Num;
    }

    TSpan<const FJsonValue> FJsonValue::AsArray() const noexcept
    {
        JsonInternal::FArrayBlock* Block = IsArray() ? static_cast<JsonInternal::FArrayBlock*>(PointerPayload()) : nullptr;
        return Block != nullptr ? TSpan<const FJsonValue>(Block->Items(), Block->Num) : TSpan<const FJsonValue>();
    }

    TSpan<FJsonValue> FJsonValue::AsArray() noexcept
    {
        JsonInternal::FArrayBlock* Block = IsArray() ? static_cast<JsonInternal::FArrayBlock*>(PointerPayload()) : nullptr;
        return Block != nullptr ? TSpan<FJsonValue>(Block->Items(), Block->Num) : TSpan<FJsonValue>();
    }

    TSpan<const FJsonMember> FJsonValue::Members() const noexcept
    {
        JsonInternal::FObjectBlock* Block = IsObject() ? static_cast<JsonInternal::FObjectBlock*>(PointerPayload()) : nullptr;
        return Block != nullptr ? TSpan<const FJsonMember>(Block->Items(), Block->Num) : TSpan<const FJsonMember>();
    }

    TSpan<FJsonMember> FJsonValue::Members() noexcept
    {
        JsonInternal::FObjectBlock* Block = IsObject() ? static_cast<JsonInternal::FObjectBlock*>(PointerPayload()) : nullptr;
        return Block != nullptr ? TSpan<FJsonMember>(Block->Items(), Block->Num) : TSpan<FJsonMember>();
    }

    const FJsonValue& FJsonValue::operator[](int32 Index) const noexcept
    {
        const TSpan<const FJsonValue> Items = AsArray();
        return Index >= 0 && Index < Items.Num() ? Items[Index] : JsonInternal::NullValue();
    }

    const FJsonValue& FJsonValue::operator[](FStringView Key) const noexcept
    {
        const FJsonValue* Member = Find(Key);
        return Member != nullptr ? *Member : JsonInternal::NullValue();
    }

    const FJsonValue* FJsonValue::Find(FStringView Key) const noexcept
    {
        if (!IsObject())
        {
            return nullptr;
        }
        JsonInternal::FObjectBlock* Block = static_cast<JsonInternal::FObjectBlock*>(PointerPayload());
        const int32 Member = JsonInternal::FindMember(Block, Key);
        return Member != Constants::kIndexNone ? &Block->Items()[Member].Value : nullptr;
    }

    FJsonValue* FJsonValue::Find(FStringView Key) noexcept
    {
        return const_cast<FJsonValue*>(static_cast<const FJsonValue*>(this)->Find(Key));
    }

    //~ Writing members

    FJsonValue& FJsonValue::Add(FJsonValue Value)
    {
        EnsureArray();
        JsonInternal::FArrayBlock* Block = static_cast<JsonInternal::FArrayBlock*>(PointerPayload());
        FJsonValue& Placed = JsonInternal::AppendElement(Block, Move(Value));
        SetPayload(Block);
        return Placed;
    }

    FJsonValue& FJsonValue::Set(FStringView Key, FJsonValue Value)
    {
        EnsureObject();
        JsonInternal::FObjectBlock* Block = static_cast<JsonInternal::FObjectBlock*>(PointerPayload());
        const int32 Existing = JsonInternal::FindMember(Block, Key);
        if (Existing != Constants::kIndexNone)
        {
            FJsonValue& Slot = Block->Items()[Existing].Value;
            Slot = Move(Value);
            return Slot;
        }
        FJsonString OwnedKey(Key);
        FJsonValue& Placed = JsonInternal::AppendMember(Block, Move(OwnedKey), Move(Value));
        SetPayload(Block);
        return Placed;
    }

    FJsonValue& FJsonValue::FindOrAdd(FStringView Key)
    {
        EnsureObject();
        JsonInternal::FObjectBlock* Block = static_cast<JsonInternal::FObjectBlock*>(PointerPayload());
        const int32 Existing = JsonInternal::FindMember(Block, Key);
        if (Existing != Constants::kIndexNone)
        {
            return Block->Items()[Existing].Value;
        }
        FJsonString OwnedKey(Key);
        FJsonValue& Placed = JsonInternal::AppendMember(Block, Move(OwnedKey), FJsonValue());
        SetPayload(Block);
        return Placed;
    }

    bool FJsonValue::Remove(FStringView Key)
    {
        if (!IsObject())
        {
            return false;
        }
        JsonInternal::FObjectBlock* Block = static_cast<JsonInternal::FObjectBlock*>(PointerPayload());
        const int32 Existing = JsonInternal::FindMember(Block, Key);
        if (Existing == Constants::kIndexNone)
        {
            return false;
        }
        JsonInternal::EraseAt(Block->Items(), Block->Num, Existing);
        JsonInternal::RebuildIndex(*Block);
        return true;
    }

    void FJsonValue::RemoveAt(int32 Index)
    {
        LUMINA_CONTAINER_CHECK(Index >= 0 && Index < Num());
        if (IsArray())
        {
            JsonInternal::FArrayBlock* Block = static_cast<JsonInternal::FArrayBlock*>(PointerPayload());
            JsonInternal::EraseAt(Block->Items(), Block->Num, Index);
        }
        else if (IsObject())
        {
            JsonInternal::FObjectBlock* Block = static_cast<JsonInternal::FObjectBlock*>(PointerPayload());
            JsonInternal::EraseAt(Block->Items(), Block->Num, Index);
            JsonInternal::RebuildIndex(*Block);
        }
    }

    void FJsonValue::Reserve(int32 Count)
    {
        if (IsArray())
        {
            JsonInternal::FArrayBlock* Block = static_cast<JsonInternal::FArrayBlock*>(PointerPayload());
            if (Count > (Block != nullptr ? Block->Max : 0))
            {
                SetPayload(JsonInternal::ResizeArrayBlock(Block, Count));
            }
        }
        else if (IsObject())
        {
            JsonInternal::FObjectBlock* Block = static_cast<JsonInternal::FObjectBlock*>(PointerPayload());
            if (Count > (Block != nullptr ? Block->Max : 0))
            {
                SetPayload(JsonInternal::ResizeObjectBlock(Block, Count));
            }
        }
    }

    void FJsonValue::MergePatch(const FJsonValue& Patch)
    {
        if (!Patch.IsObject())
        {
            *this = Patch;
            return;
        }
        if (!IsObject())
        {
            *this = MakeObject();
        }
        for (const FJsonMember& Member : Patch.Members())
        {
            if (Member.Value.IsNull())
            {
                Remove(Member.Key.View());
            }
            else
            {
                FindOrAdd(Member.Key.View()).MergePatch(Member.Value);
            }
        }
    }

    bool FJsonValue::Equals(const FJsonValue& Other) const noexcept
    {
        if (IsNumber() && Other.IsNumber())
        {
            if (Type == Other.Type)
            {
                return IsDouble() ? GetPayload<double>() == Other.GetPayload<double>() : Memory::Memcmp(Storage, Other.Storage, 8) == 0;
            }
            if (IsDouble() || Other.IsDouble())
            {
                return AsDouble() == Other.AsDouble();
            }
            const FJsonValue& Signed   = Type == EJsonType::Int ? *this : Other;
            const FJsonValue& Unsigned = Type == EJsonType::Int ? Other : *this;
            return Signed.GetPayload<int64>() >= 0 && static_cast<uint64>(Signed.GetPayload<int64>()) == Unsigned.GetPayload<uint64>();
        }
        if (Type != Other.Type)
        {
            return false;
        }
        switch (Type)
        {
        case EJsonType::Null:   return true;
        case EJsonType::Bool:   return GetPayload<bool>() == Other.GetPayload<bool>();
        case EJsonType::String: return StringPayload().View() == Other.StringPayload().View();
        case EJsonType::Array:
        {
            const TSpan<const FJsonValue> Mine = AsArray();
            const TSpan<const FJsonValue> Theirs = Other.AsArray();
            if (Mine.Num() != Theirs.Num())
            {
                return false;
            }
            for (int32 Index = 0; Index < Mine.Num(); ++Index)
            {
                if (!Mine[Index].Equals(Theirs[Index]))
                {
                    return false;
                }
            }
            return true;
        }
        case EJsonType::Object:
        {
            if (Num() != Other.Num())
            {
                return false;
            }
            for (const FJsonMember& Member : Members())
            {
                const FJsonValue* Match = Other.Find(Member.Key.View());
                if (Match == nullptr || !Member.Value.Equals(*Match))
                {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
        }
    }

    //~ Parsing

    class FJsonParser
    {
    public:

        FJsonParser(FStringView Text, EJsonParseFlags InFlags)
            : Begin(Text.GetData())
            , Cursor(Text.GetData())
            , End(Text.GetData() + Text.Len())
            , Flags(InFlags)
        {
        }

        bool Run(FJsonValue& Out, FJsonParseError* OutError)
        {
            Out.Reset();
            const bool bParsed = SkipSpace() && ParseValue(Out, 0) && SkipSpace() && (Cursor == End || Fail("unexpected characters after the document"));
            if (bParsed)
            {
                return true;
            }
            Out.Reset();
            if (OutError != nullptr)
            {
                ReportError(*OutError);
            }
            return false;
        }

    private:

        bool Fail(const char* Message)
        {
            if (ErrorMessage == nullptr)
            {
                ErrorMessage = Message;
                ErrorAt = Cursor;
            }
            return false;
        }

        void ReportError(FJsonParseError& Out) const
        {
            int32 Line = 1;
            const char* LineStart = Begin;
            for (const char* Scan = Begin; Scan < ErrorAt; ++Scan)
            {
                if (*Scan == '\n')
                {
                    ++Line;
                    LineStart = Scan + 1;
                }
            }
            Out.Line = Line;
            Out.Column = static_cast<int32>(ErrorAt - LineStart) + 1;
            Out.Offset = ErrorAt - Begin;
            Out.Message = Format("{} at line {}, column {}", ErrorMessage, Out.Line, Out.Column);
        }

        bool SkipSpace()
        {
            while (Cursor < End)
            {
                const char C = *Cursor;
                if (C == ' ' || C == '\n' || C == '\r' || C == '\t')
                {
                    ++Cursor;
                }
                else if (C == '/' && HasJsonFlag(Flags, EJsonParseFlags::AllowComments))
                {
                    if (Cursor + 1 < End && Cursor[1] == '/')
                    {
                        while (Cursor < End && *Cursor != '\n')
                        {
                            ++Cursor;
                        }
                    }
                    else if (Cursor + 1 < End && Cursor[1] == '*')
                    {
                        const char* Close = Cursor + 2;
                        while (Close + 1 < End && !(Close[0] == '*' && Close[1] == '/'))
                        {
                            ++Close;
                        }
                        if (Close + 1 >= End)
                        {
                            return Fail("unterminated comment");
                        }
                        Cursor = Close + 2;
                    }
                    else
                    {
                        return Fail("unexpected '/'");
                    }
                }
                else
                {
                    break;
                }
            }
            return true;
        }

        bool ParseValue(FJsonValue& Out, int32 Depth)
        {
            if (Cursor == End)
            {
                return Fail("unexpected end of input");
            }
            switch (*Cursor)
            {
            case '{': return ParseObject(Out, Depth);
            case '[': return ParseArray(Out, Depth);
            case '"':
            {
                FJsonString& Text = *new (Out.Storage) FJsonString();
                if (!ParseString(Text))
                {
                    return false;
                }
                Out.Type = EJsonType::String;
                return true;
            }
            case 't':
                Out.SetPayload(true);
                Out.Type = EJsonType::Bool;
                return ParseWord("true", 4);
            case 'f':
                Out.SetPayload(false);
                Out.Type = EJsonType::Bool;
                return ParseWord("false", 5);
            case 'n':
                return ParseWord("null", 4);
            default:
                if (*Cursor == '-' || (*Cursor >= '0' && *Cursor <= '9'))
                {
                    return ParseNumber(Out);
                }
                return Fail("unexpected character");
            }
        }

        bool ParseWord(const char* Word, int32 Length)
        {
            if (End - Cursor < Length || Memory::Memcmp(Cursor, Word, static_cast<size_t>(Length)) != 0)
            {
                return Fail("invalid literal");
            }
            Cursor += Length;
            return true;
        }

        bool ParseObject(FJsonValue& Out, int32 Depth)
        {
            if (Depth >= JsonInternal::kMaxDepth)
            {
                return Fail("nesting too deep");
            }
            ++Cursor;
            if (!SkipSpace())
            {
                return false;
            }

            const int32 Base = MemberStack.Num;
            if (Cursor < End && *Cursor == '}')
            {
                ++Cursor;
                Out.EnsureObject();
                return true;
            }

            while (true)
            {
                if (Cursor == End || *Cursor != '"')
                {
                    return Fail("expected a string key");
                }
                FJsonString Key;
                if (!ParseString(Key) || !SkipSpace())
                {
                    return false;
                }
                if (Cursor == End || *Cursor != ':')
                {
                    return Fail("expected ':'");
                }
                ++Cursor;
                if (!SkipSpace())
                {
                    return false;
                }

                FJsonValue Child;
                if (!ParseValue(Child, Depth + 1) || !SkipSpace())
                {
                    return false;
                }

                // A repeated key keeps its first position and its last value; objects past the scan size resolve this when indexed.
                int32 Repeat = Constants::kIndexNone;
                if (MemberStack.Num - Base < JsonInternal::kIndexThreshold)
                {
                    const FStringView KeyView = Key.View();
                    for (int32 Member = Base; Member < MemberStack.Num; ++Member)
                    {
                        if (JsonInternal::KeyEquals(MemberStack.Data[Member].Key, KeyView))
                        {
                            Repeat = Member;
                            break;
                        }
                    }
                }
                if (Repeat != Constants::kIndexNone)
                {
                    MemberStack.Data[Repeat].Value = Move(Child);
                }
                else
                {
                    MemberStack.Push(FJsonMember{ Move(Key), Move(Child) });
                }

                if (Cursor < End && *Cursor == ',')
                {
                    ++Cursor;
                    if (!SkipSpace())
                    {
                        return false;
                    }
                    if (HasJsonFlag(Flags, EJsonParseFlags::AllowTrailingCommas) && Cursor < End && *Cursor == '}')
                    {
                        ++Cursor;
                        return FinishObject(Out, Base);
                    }
                    continue;
                }
                if (Cursor < End && *Cursor == '}')
                {
                    ++Cursor;
                    return FinishObject(Out, Base);
                }
                return Fail("expected ',' or '}'");
            }
        }

        bool FinishObject(FJsonValue& Out, int32 Base)
        {
            Out.EnsureObject();
            JsonInternal::FObjectBlock* Block = JsonInternal::TakeObject(MemberStack, Base);
            if (Block->Num >= JsonInternal::kIndexThreshold)
            {
                JsonInternal::IndexParsedObject(*Block);
            }
            Out.SetPayload(Block);
            return true;
        }

        bool ParseArray(FJsonValue& Out, int32 Depth)
        {
            if (Depth >= JsonInternal::kMaxDepth)
            {
                return Fail("nesting too deep");
            }
            ++Cursor;
            if (!SkipSpace())
            {
                return false;
            }

            const int32 Base = ValueStack.Num;
            if (Cursor < End && *Cursor == ']')
            {
                ++Cursor;
                Out.EnsureArray();
                return true;
            }

            while (true)
            {
                FJsonValue Child;
                if (!ParseValue(Child, Depth + 1) || !SkipSpace())
                {
                    return false;
                }
                ValueStack.Push(Move(Child));

                if (Cursor < End && *Cursor == ',')
                {
                    ++Cursor;
                    if (!SkipSpace())
                    {
                        return false;
                    }
                    if (HasJsonFlag(Flags, EJsonParseFlags::AllowTrailingCommas) && Cursor < End && *Cursor == ']')
                    {
                        ++Cursor;
                        return FinishArray(Out, Base);
                    }
                    continue;
                }
                if (Cursor < End && *Cursor == ']')
                {
                    ++Cursor;
                    return FinishArray(Out, Base);
                }
                return Fail("expected ',' or ']'");
            }
        }

        bool FinishArray(FJsonValue& Out, int32 Base)
        {
            Out.EnsureArray();
            Out.SetPayload(JsonInternal::TakeArray(ValueStack, Base));
            return true;
        }

        // Unescaped text is copied straight from the input; Scratch only fills once an escape appears.
        bool ParseString(FJsonString& Out)
        {
            ++Cursor;
            const char* Run = Cursor;
            bool bEscaped = false;

            while (true)
            {
                Cursor += JsonInternal::FindSpecial(Cursor, End - Cursor);
                if (Cursor == End)
                {
                    return Fail("unterminated string");
                }

                const uint8 Byte = static_cast<uint8>(*Cursor);
                if (Byte == '"')
                {
                    if (bEscaped)
                    {
                        Scratch.Append(Run, static_cast<int32>(Cursor - Run));
                        Out.Assign(Scratch.GetData(), Scratch.Len());
                    }
                    else
                    {
                        Out.Assign(Run, static_cast<int32>(Cursor - Run));
                    }
                    ++Cursor;
                    return true;
                }

                if (Byte >= 0x80)
                {
                    const int32 Length = JsonInternal::ValidUtf8Length(reinterpret_cast<const uint8*>(Cursor), reinterpret_cast<const uint8*>(End));
                    if (Length == 0)
                    {
                        return Fail("invalid UTF-8 in string");
                    }
                    Cursor += Length;
                    continue;
                }

                if (Byte < 0x20)
                {
                    return Fail("control character in string");
                }

                if (!bEscaped)
                {
                    bEscaped = true;
                    Scratch.Reset();
                }
                Scratch.Append(Run, static_cast<int32>(Cursor - Run));
                if (!ParseEscape())
                {
                    return false;
                }
                Run = Cursor;
            }
        }

        bool ParseEscape()
        {
            ++Cursor;
            if (Cursor == End)
            {
                return Fail("unterminated string");
            }
            const char C = *Cursor++;
            switch (C)
            {
            case '"':  Scratch.AppendChar('"');  return true;
            case '\\': Scratch.AppendChar('\\'); return true;
            case '/':  Scratch.AppendChar('/');  return true;
            case 'b':  Scratch.AppendChar('\b'); return true;
            case 'f':  Scratch.AppendChar('\f'); return true;
            case 'n':  Scratch.AppendChar('\n'); return true;
            case 'r':  Scratch.AppendChar('\r'); return true;
            case 't':  Scratch.AppendChar('\t'); return true;
            case 'u':
            {
                uint32 CodePoint;
                if (!ParseHex4(CodePoint))
                {
                    return false;
                }
                if (CodePoint >= 0xD800 && CodePoint <= 0xDBFF)
                {
                    uint32 Low;
                    if (End - Cursor < 2 || Cursor[0] != '\\' || Cursor[1] != 'u')
                    {
                        return Fail("unpaired surrogate");
                    }
                    Cursor += 2;
                    if (!ParseHex4(Low))
                    {
                        return false;
                    }
                    if (Low < 0xDC00 || Low > 0xDFFF)
                    {
                        return Fail("unpaired surrogate");
                    }
                    CodePoint = 0x10000 + ((CodePoint - 0xD800) << 10) + (Low - 0xDC00);
                }
                else if (CodePoint >= 0xDC00 && CodePoint <= 0xDFFF)
                {
                    return Fail("unpaired surrogate");
                }
                JsonInternal::AppendUtf8(Scratch, CodePoint);
                return true;
            }
            default:
                --Cursor;
                return Fail("invalid escape");
            }
        }

        bool ParseHex4(uint32& Out)
        {
            if (End - Cursor < 4)
            {
                return Fail("truncated \\u escape");
            }
            Out = 0;
            for (int32 Digit = 0; Digit < 4; ++Digit)
            {
                const int32 Value = JsonInternal::HexValue(Cursor[Digit]);
                if (Value < 0)
                {
                    Cursor += Digit;
                    return Fail("invalid \\u escape");
                }
                Out = (Out << 4) | static_cast<uint32>(Value);
            }
            Cursor += 4;
            return true;
        }

        bool ParseNumber(FJsonValue& Out)
        {
            const char* Start = Cursor;
            const bool bNegative = *Cursor == '-';
            if (bNegative)
            {
                ++Cursor;
            }
            if (Cursor == End || *Cursor < '0' || *Cursor > '9')
            {
                return Fail("invalid number");
            }

            uint64 Magnitude = 0;
            bool bOverflow = false;
            const char* IntegerStart = Cursor;
            if (*Cursor == '0')
            {
                ++Cursor;
                if (Cursor < End && *Cursor >= '0' && *Cursor <= '9')
                {
                    return Fail("leading zero in number");
                }
            }
            else
            {
                while (Cursor < End && *Cursor >= '0' && *Cursor <= '9')
                {
                    const uint64 Digit = static_cast<uint64>(*Cursor - '0');
                    if (Magnitude > (std::numeric_limits<uint64>::max() - Digit) / 10)
                    {
                        bOverflow = true;
                    }
                    Magnitude = Magnitude * 10 + Digit;
                    ++Cursor;
                }
            }
            const bool bZeroInteger = Cursor - IntegerStart == 1 && *IntegerStart == '0';

            bool bFloating = false;
            bool bNegativeExponent = false;
            if (Cursor < End && *Cursor == '.')
            {
                bFloating = true;
                ++Cursor;
                if (Cursor == End || *Cursor < '0' || *Cursor > '9')
                {
                    return Fail("expected a digit after '.'");
                }
                while (Cursor < End && *Cursor >= '0' && *Cursor <= '9')
                {
                    ++Cursor;
                }
            }
            if (Cursor < End && (*Cursor == 'e' || *Cursor == 'E'))
            {
                bFloating = true;
                ++Cursor;
                if (Cursor < End && (*Cursor == '+' || *Cursor == '-'))
                {
                    bNegativeExponent = *Cursor == '-';
                    ++Cursor;
                }
                if (Cursor == End || *Cursor < '0' || *Cursor > '9')
                {
                    return Fail("expected a digit in the exponent");
                }
                while (Cursor < End && *Cursor >= '0' && *Cursor <= '9')
                {
                    ++Cursor;
                }
            }

            if (!bFloating && !bOverflow)
            {
                if (!bNegative)
                {
                    Out.SetPayload(Magnitude);
                    Out.Type = EJsonType::UInt;
                    return true;
                }
                if (Magnitude <= static_cast<uint64>(std::numeric_limits<int64>::max()) + 1)
                {
                    Out.SetPayload(static_cast<int64>(0 - Magnitude));
                    Out.Type = EJsonType::Int;
                    return true;
                }
            }

            double Value = 0.0;
            const std::from_chars_result Result = std::from_chars(Start, Cursor, Value);
            if (Result.ec == std::errc::result_out_of_range)
            {
                if (!(bNegativeExponent || bZeroInteger))
                {
                    Cursor = Start;
                    return Fail("number out of range");
                }
                Value = bNegative ? -0.0 : 0.0;
            }
            else if (Result.ec != std::errc() || Result.ptr != Cursor)
            {
                Cursor = Start;
                return Fail("invalid number");
            }
            Out.SetPayload(Value);
            Out.Type = EJsonType::Double;
            return true;
        }

        const char*     Begin;
        const char*     Cursor;
        const char*     End;
        EJsonParseFlags Flags;
        const char*     ErrorMessage = nullptr;
        const char*     ErrorAt      = nullptr;
        FString         Scratch;

        JsonInternal::TParseStack<FJsonValue>  ValueStack;
        JsonInternal::TParseStack<FJsonMember> MemberStack;
    };

    bool FJsonValue::Parse(FStringView Text, FJsonValue& Out, FJsonParseError* OutError, EJsonParseFlags Flags)
    {
        FJsonParser Parser(Text, Flags);
        return Parser.Run(Out, OutError);
    }

    //~ Writing

    class FJsonWriter
    {
    public:

        explicit FJsonWriter(int32 InIndent) : Indent(InIndent) {}

        ~FJsonWriter()
        {
            if (Buffer != nullptr)
            {
                void* Block = Buffer;
                Memory::Free(Block);
            }
        }

        FJsonWriter(const FJsonWriter&) = delete;
        FJsonWriter& operator=(const FJsonWriter&) = delete;

        void Write(const FJsonValue& Value, int32 Depth)
        {
            switch (Value.Type)
            {
            case EJsonType::Null:
                PutText("null", 4);
                break;
            case EJsonType::Bool:
                Value.GetPayload<bool>() ? PutText("true", 4) : PutText("false", 5);
                break;
            case EJsonType::Int:
                PutNumber(Value.GetPayload<int64>());
                break;
            case EJsonType::UInt:
                PutNumber(Value.GetPayload<uint64>());
                break;
            case EJsonType::Double:
                PutDouble(Value.GetPayload<double>());
                break;
            case EJsonType::String:
                PutString(Value.StringPayload().View());
                break;
            case EJsonType::Array:
            {
                const TSpan<const FJsonValue> Items = Value.AsArray();
                if (Items.Num() == 0)
                {
                    PutText("[]", 2);
                    break;
                }
                PutChar('[');
                for (int32 Index = 0; Index < Items.Num(); ++Index)
                {
                    if (Index > 0)
                    {
                        PutChar(',');
                    }
                    PutBreak(Depth + 1);
                    Write(Items[Index], Depth + 1);
                }
                PutBreak(Depth);
                PutChar(']');
                break;
            }
            case EJsonType::Object:
            {
                const TSpan<const FJsonMember> Members = Value.Members();
                if (Members.Num() == 0)
                {
                    PutText("{}", 2);
                    break;
                }
                PutChar('{');
                for (int32 Index = 0; Index < Members.Num(); ++Index)
                {
                    if (Index > 0)
                    {
                        PutChar(',');
                    }
                    PutBreak(Depth + 1);
                    PutString(Members[Index].Key.View());
                    Indent >= 0 ? PutText(": ", 2) : PutChar(':');
                    Write(Members[Index].Value, Depth + 1);
                }
                PutBreak(Depth);
                PutChar('}');
                break;
            }
            }
        }

        void Finish(FString& Out) const
        {
            Out.Append(Buffer, static_cast<int32>(Length));
        }

    private:

        FORCEINLINE char* Room(int64 Count)
        {
            if (Length + Count > Capacity) [[unlikely]]
            {
                Grow(Count);
            }
            return Buffer + Length;
        }

        void Grow(int64 Count)
        {
            int64 NewCapacity = Capacity == 0 ? 256 : Capacity * 2;
            while (NewCapacity < Length + Count)
            {
                NewCapacity *= 2;
            }
            Buffer = static_cast<char*>(Memory::Realloc(Buffer, static_cast<size_t>(NewCapacity)));
            Capacity = NewCapacity;
        }

        FORCEINLINE void PutChar(char C)
        {
            *Room(1) = C;
            ++Length;
        }

        FORCEINLINE void PutText(const char* Text, int64 Count)
        {
            Memory::Memcpy(Room(Count), Text, static_cast<size_t>(Count));
            Length += Count;
        }

        void PutBreak(int32 Depth)
        {
            if (Indent < 0)
            {
                return;
            }
            const int64 Spaces = static_cast<int64>(Depth) * Indent;
            char* Out = Room(Spaces + 1);
            *Out = '\n';
            std::memset(Out + 1, ' ', static_cast<size_t>(Spaces));
            Length += Spaces + 1;
        }

        template <typename T>
        void PutNumber(T Value)
        {
            char* Out = Room(24);
            const std::to_chars_result Result = std::to_chars(Out, Out + 24, Value);
            Length += Result.ptr - Out;
        }

        // Shortest round-trip digits: fixed while the point sits within 15 digits and 4 places, else d.ddde+XX; ".0" on integral values, null when non-finite.
        void PutDouble(double Value)
        {
            if (!std::isfinite(Value))
            {
                PutText("null", 4);
                return;
            }

            char Scientific[32];
            const std::to_chars_result Result = std::to_chars(Scientific, Scientific + sizeof(Scientific), Value, std::chars_format::scientific);
            const char* Cursor = Scientific;
            const char* Stop = Result.ptr;

            const bool bNegative = *Cursor == '-';
            Cursor += bNegative ? 1 : 0;

            char Digits[20];
            int32 DigitCount = 0;
            while (Cursor < Stop && *Cursor != 'e')
            {
                if (*Cursor != '.')
                {
                    Digits[DigitCount++] = *Cursor;
                }
                ++Cursor;
            }
            int32 Exponent = 0;
            std::from_chars(Cursor + (Cursor[1] == '+' ? 2 : 1), Stop, Exponent);

            const int32 Point = Exponent + 1;
            char* Out = Room(DigitCount + 32);
            char* Write = Out;
            if (bNegative)
            {
                *Write++ = '-';
            }

            if (DigitCount <= Point && Point <= 15)
            {
                Memory::Memcpy(Write, Digits, static_cast<size_t>(DigitCount));
                Write += DigitCount;
                std::memset(Write, '0', static_cast<size_t>(Point - DigitCount));
                Write += Point - DigitCount;
                *Write++ = '.';
                *Write++ = '0';
            }
            else if (0 < Point && Point <= 15)
            {
                Memory::Memcpy(Write, Digits, static_cast<size_t>(Point));
                Write += Point;
                *Write++ = '.';
                Memory::Memcpy(Write, Digits + Point, static_cast<size_t>(DigitCount - Point));
                Write += DigitCount - Point;
            }
            else if (-4 < Point && Point <= 0)
            {
                *Write++ = '0';
                *Write++ = '.';
                std::memset(Write, '0', static_cast<size_t>(-Point));
                Write += -Point;
                Memory::Memcpy(Write, Digits, static_cast<size_t>(DigitCount));
                Write += DigitCount;
            }
            else
            {
                *Write++ = Digits[0];
                if (DigitCount > 1)
                {
                    *Write++ = '.';
                    Memory::Memcpy(Write, Digits + 1, static_cast<size_t>(DigitCount - 1));
                    Write += DigitCount - 1;
                }
                const int32 Shown = Point - 1;
                *Write++ = 'e';
                *Write++ = Shown < 0 ? '-' : '+';
                const int32 Magnitude = Shown < 0 ? -Shown : Shown;
                if (Magnitude < 10)
                {
                    *Write++ = '0';
                }
                Write = std::to_chars(Write, Write + 4, Magnitude).ptr;
            }
            Length += Write - Out;
        }

        // Invalid UTF-8 becomes U+FFFD, so whatever the document holds it always writes valid JSON.
        void PutString(FStringView Text)
        {
            PutChar('"');
            const char* Cursor = Text.GetData();
            const char* End = Cursor + Text.Len();
            while (Cursor < End)
            {
                const int64 Clean = JsonInternal::FindSpecial(Cursor, End - Cursor);
                if (Clean > 0)
                {
                    PutText(Cursor, Clean);
                    Cursor += Clean;
                    if (Cursor == End)
                    {
                        break;
                    }
                }

                const uint8 Byte = static_cast<uint8>(*Cursor);
                if (Byte >= 0x80)
                {
                    const int32 Sequence = JsonInternal::ValidUtf8Length(reinterpret_cast<const uint8*>(Cursor), reinterpret_cast<const uint8*>(End));
                    if (Sequence == 0)
                    {
                        PutText("\xEF\xBF\xBD", 3);
                        ++Cursor;
                    }
                    else
                    {
                        PutText(Cursor, Sequence);
                        Cursor += Sequence;
                    }
                    continue;
                }

                switch (Byte)
                {
                case '"':  PutText("\\\"", 2); break;
                case '\\': PutText("\\\\", 2); break;
                case '\b': PutText("\\b", 2); break;
                case '\f': PutText("\\f", 2); break;
                case '\n': PutText("\\n", 2); break;
                case '\r': PutText("\\r", 2); break;
                case '\t': PutText("\\t", 2); break;
                default:
                {
                    static constexpr char kHex[] = "0123456789abcdef";
                    const char Escape[6] = { '\\', 'u', '0', '0', kHex[Byte >> 4], kHex[Byte & 0xF] };
                    PutText(Escape, 6);
                    break;
                }
                }
                ++Cursor;
            }
            PutChar('"');
        }

        int32 Indent;
        char* Buffer   = nullptr;
        int64 Length   = 0;
        int64 Capacity = 0;
    };

    void FJsonValue::WriteTo(FString& Out, int32 Indent) const
    {
        FJsonWriter Writer(Indent);
        Writer.Write(*this, 0);
        Writer.Finish(Out);
    }

    FString FJsonValue::ToString(int32 Indent) const
    {
        FString Out;
        WriteTo(Out, Indent);
        return Out;
    }
}
