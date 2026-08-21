#pragma once

#include <compare>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "ElementOps.h"
#include "StringView.h"

namespace Lumina::Containers
{
    /** Null-terminated string with a packed small-string buffer; 16 bytes for char at the default capacity. */
    template <typename TChar,
              size_t SsoCapacity = (16 / sizeof(TChar)) - 1,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    class TBasicString
    {
        struct FHeapLayout
        {
            TChar* Data;
            uint32 Size;
            uint32 Capacity;
        };

        // Up to 127 the inline size packs into the mode byte, which is also the terminator when the buffer is full.
        static constexpr bool   bPackedSize   = SsoCapacity <= 127;
        static constexpr size_t kTailBytes    = bPackedSize ? 0 : 8;
        static constexpr size_t kRawBytes     = (SsoCapacity + 1) * sizeof(TChar) + kTailBytes;
        static constexpr size_t kNeededBytes  = kRawBytes > sizeof(FHeapLayout) ? kRawBytes : sizeof(FHeapLayout);
        static constexpr size_t kStorageBytes = (kNeededBytes + 7) & ~static_cast<size_t>(7);
        static constexpr size_t kModeByte     = kStorageBytes - 1;
        static constexpr size_t kSizeOffset   = kStorageBytes - 8;
        static constexpr uint8  kHeapFlag     = 0x80;
        static constexpr uint32 kCapacityMask = 0x7FFFFFFFu;

        static_assert(SsoCapacity >= 1 && SsoCapacity <= kCapacityMask);
        static_assert(bPackedSize || (SsoCapacity + 1) * sizeof(TChar) <= kSizeOffset);

        union FStorage
        {
            FHeapLayout Heap;
            TChar       Inline[kStorageBytes / sizeof(TChar)];
            uint8       Bytes[kStorageBytes];

            FStorage() : Bytes{} {}
        };

    public:

        using value_type             = TChar;
        using size_type              = size_t;
        using difference_type        = ptrdiff_t;
        using reference              = TChar&;
        using const_reference        = const TChar&;
        using pointer                = TChar*;
        using const_pointer          = const TChar*;
        using iterator               = TChar*;
        using const_iterator         = const TChar*;
        using reverse_iterator       = std::reverse_iterator<TChar*>;
        using const_reverse_iterator = std::reverse_iterator<const TChar*>;
        using view_type              = TStringView<TChar>;
        using cview_type             = TCStringView<TChar>;

        static constexpr size_t npos                = ~static_cast<size_t>(0);
        static constexpr size_t MaxCapacity         = kCapacityMask;
        static constexpr size_t InlineCapacityChars = SsoCapacity;

        TBasicString() noexcept { InitializeEmpty(); }

        TBasicString(const TChar* Text)
        {
            InitializeEmpty();
            Assign(Text, Text != nullptr ? TraitsLength(Text) : 0);
        }

        TBasicString(const TChar* Text, size_t Length)
        {
            InitializeEmpty();
            Assign(Text, Length);
        }

        TBasicString(view_type Text)
        {
            InitializeEmpty();
            Assign(Text.data(), Text.size());
        }

        TBasicString(size_t Count, TChar Character)
        {
            InitializeEmpty();
            resize(Count, Character);
        }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TBasicString(TIter First, TIter Last)
        {
            InitializeEmpty();
            for (; First != Last; ++First)
            {
                push_back(*First);
            }
        }

        TBasicString(const TBasicString& Other)
        {
            InitializeEmpty();
            Assign(Other.GetData(), Other.GetSize());
        }

        TBasicString(TBasicString&& Other) noexcept
        {
            InitializeEmpty();
            AdoptOrCopy(Other);
        }

        ~TBasicString() { ReleaseHeapBlock(); }

        TBasicString& operator=(const TBasicString& Other)
        {
            if (this != &Other)
            {
                Assign(Other.GetData(), Other.GetSize());
            }
            return *this;
        }

        TBasicString& operator=(TBasicString&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseHeapBlock();
                InitializeEmpty();
                AdoptOrCopy(Other);
            }
            return *this;
        }

        TBasicString& operator=(const TChar* Text) { Assign(Text, Text != nullptr ? TraitsLength(Text) : 0); return *this; }
        TBasicString& operator=(view_type Text) { Assign(Text.data(), Text.size()); return *this; }

        NODISCARD FORCEINLINE const TChar* c_str() const noexcept { return GetData(); }
        NODISCARD FORCEINLINE TChar* data() noexcept { return GetData(); }
        NODISCARD FORCEINLINE const TChar* data() const noexcept { return GetData(); }

        NODISCARD FORCEINLINE size_t size() const noexcept { return GetSize(); }
        NODISCARD FORCEINLINE size_t length() const noexcept { return GetSize(); }
        NODISCARD FORCEINLINE size_t capacity() const noexcept { return GetCapacity(); }
        NODISCARD FORCEINLINE bool empty() const noexcept { return GetSize() == 0; }
        NODISCARD static constexpr size_t max_size() noexcept { return MaxCapacity; }

        /** True while the characters live in the packed buffer rather than a heap block. */
        NODISCARD FORCEINLINE bool IsInline() const noexcept { return !IsHeap(); }

        NODISCARD FORCEINLINE operator view_type() const noexcept { return view_type(GetData(), GetSize()); }
        NODISCARD FORCEINLINE view_type View() const noexcept { return view_type(GetData(), GetSize()); }
        NODISCARD FORCEINLINE cview_type CView() const noexcept { return cview_type::FromTerminated(GetData(), GetSize()); }

        NODISCARD FORCEINLINE TChar* begin() noexcept { return GetData(); }
        NODISCARD FORCEINLINE const TChar* begin() const noexcept { return GetData(); }
        NODISCARD FORCEINLINE const TChar* cbegin() const noexcept { return GetData(); }
        NODISCARD FORCEINLINE TChar* end() noexcept { return GetData() + GetSize(); }
        NODISCARD FORCEINLINE const TChar* end() const noexcept { return GetData() + GetSize(); }
        NODISCARD FORCEINLINE const TChar* cend() const noexcept { return GetData() + GetSize(); }

        NODISCARD FORCEINLINE reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        NODISCARD FORCEINLINE const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        NODISCARD FORCEINLINE reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        NODISCARD FORCEINLINE const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

        NODISCARD FORCEINLINE TChar& operator[](size_t Index) noexcept
        {
            LUMINA_CONTAINER_CHECK_WITHIN(Index, GetSize());
            return GetData()[Index];
        }

        NODISCARD FORCEINLINE const TChar& operator[](size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_WITHIN(Index, GetSize());
            return GetData()[Index];
        }

        NODISCARD FORCEINLINE TChar& at(size_t Index) noexcept { return (*this)[Index]; }
        NODISCARD FORCEINLINE const TChar& at(size_t Index) const noexcept { return (*this)[Index]; }

        NODISCARD FORCEINLINE TChar& front() noexcept { return (*this)[0]; }
        NODISCARD FORCEINLINE const TChar& front() const noexcept { return (*this)[0]; }

        NODISCARD FORCEINLINE TChar& back() noexcept
        {
            LUMINA_CONTAINER_CHECK(GetSize() != 0);
            return GetData()[GetSize() - 1];
        }

        NODISCARD FORCEINLINE const TChar& back() const noexcept
        {
            LUMINA_CONTAINER_CHECK(GetSize() != 0);
            return GetData()[GetSize() - 1];
        }

        void reserve(size_t NewCapacity)
        {
            if (NewCapacity > GetCapacity())
            {
                ReallocateTo(NewCapacity);
            }
        }

        FORCEINLINE void Reserve(size_t NewCapacity) { reserve(NewCapacity); }

        void shrink_to_fit()
        {
            if (!IsHeap())
            {
                return;
            }

            const size_t Size = GetSize();
            if (Size <= SsoCapacity)
            {
                TChar* Old = Store.Heap.Data;
                const size_t OldBytes = (static_cast<size_t>(GetCapacity()) + 1) * sizeof(TChar);
                InitializeEmpty();
                Memory::Memcpy(Store.Inline, Old, Size * sizeof(TChar));
                SetSize(Size);
                TAllocator::Deallocate(Old, OldBytes, alignof(TChar));
                return;
            }

            if (Size < GetCapacity())
            {
                ReallocateTo(Size);
            }
        }

        FORCEINLINE void clear() noexcept { SetSize(0); }

        void resize(size_t NewSize)
        {
            resize(NewSize, TChar(0));
        }

        void resize(size_t NewSize, TChar Character)
        {
            const size_t Size = GetSize();
            if (NewSize > Size)
            {
                ReserveForGrowth(NewSize);
                TChar* Write = GetData();
                for (size_t Index = Size; Index < NewSize; ++Index)
                {
                    Write[Index] = Character;
                }
            }
            SetSize(NewSize);
        }

        FORCEINLINE void push_back(TChar Character)
        {
            const size_t Size = GetSize();
            if (Size == GetCapacity()) [[unlikely]]
            {
                ReserveForGrowth(Size + 1);
            }
            GetData()[Size] = Character;
            SetSize(Size + 1);
        }

        FORCEINLINE void pop_back() noexcept
        {
            LUMINA_CONTAINER_CHECK(GetSize() != 0);
            SetSize(GetSize() - 1);
        }

        TBasicString& append(const TChar* Text, size_t Length)
        {
            if (Length == 0)
            {
                return *this;
            }

            const size_t Size = GetSize();
            const TChar* Current = GetData();

            // Growth frees the buffer Text may point into, so re-derive it from the new one afterwards.
            const bool bAliasesSelf = Text >= Current && Text <= Current + Size;
            const size_t Offset = bAliasesSelf ? static_cast<size_t>(Text - Current) : 0;

            ReserveForGrowth(Size + Length);

            TChar* Write = GetData();
            Memory::Memcpy(Write + Size, bAliasesSelf ? Write + Offset : Text, Length * sizeof(TChar));
            SetSize(Size + Length);
            return *this;
        }

        TBasicString& append(view_type Text) { return append(Text.data(), Text.size()); }
        TBasicString& append(const TChar* Text) { return append(Text, TraitsLength(Text)); }
        TBasicString& append(size_t Count, TChar Character) { resize(GetSize() + Count, Character); return *this; }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TBasicString& append(TIter First, TIter Last)
        {
            for (; First != Last; ++First)
            {
                push_back(static_cast<TChar>(*First));
            }
            return *this;
        }

        TBasicString& operator+=(view_type Text) { return append(Text.data(), Text.size()); }
        TBasicString& operator+=(const TChar* Text) { return append(Text, TraitsLength(Text)); }
        TBasicString& operator+=(const TBasicString& Other) { return append(Other.GetData(), Other.GetSize()); }
        TBasicString& operator+=(TChar Character) { push_back(Character); return *this; }

        TBasicString& assign(const TChar* Text, size_t Length) { Assign(Text, Length); return *this; }
        TBasicString& assign(view_type Text) { Assign(Text.data(), Text.size()); return *this; }
        TBasicString& assign(const TChar* Text) { Assign(Text, TraitsLength(Text)); return *this; }
        TBasicString& assign(size_t Count, TChar Character) { clear(); resize(Count, Character); return *this; }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TBasicString& assign(TIter First, TIter Last)
        {
            clear();
            return append(First, Last);
        }

        TBasicString& insert(size_t Position, size_t Count, TChar Character)
        {
            return insert(Position, TBasicString(Count, Character).View());
        }

        TChar* insert(const TChar* Position, TChar Character)
        {
            const size_t Index = static_cast<size_t>(Position - GetData());
            insert(Index, view_type(&Character, 1));
            return GetData() + Index;
        }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TChar* insert(const TChar* Position, TIter First, TIter Last)
        {
            const size_t Index = static_cast<size_t>(Position - GetData());
            TBasicString Staged;
            Staged.append(First, Last);
            insert(Index, Staged.View());
            return GetData() + Index;
        }

        TBasicString& insert(size_t Position, view_type Text)
        {
            const size_t Size = GetSize();
            LUMINA_CONTAINER_CHECK_WITHIN(Position, Size);

            if (Text.empty())
            {
                return *this;
            }

            // Copy first: the source may alias our own buffer, which the growth below would invalidate.
            TBasicString Staged(Text.data(), Text.size());
            ReserveForGrowth(Size + Staged.GetSize());

            TChar* Write = GetData();
            std::memmove(Write + Position + Staged.GetSize(), Write + Position, (Size - Position) * sizeof(TChar));
            Memory::Memcpy(Write + Position, Staged.GetData(), Staged.GetSize() * sizeof(TChar));
            SetSize(Size + Staged.GetSize());
            return *this;
        }

        TBasicString& erase(size_t Position = 0, size_t Count = npos)
        {
            const size_t Size = GetSize();
            LUMINA_CONTAINER_CHECK_WITHIN(Position, Size);

            const size_t Removed = Count < Size - Position ? Count : Size - Position;
            if (Removed != 0)
            {
                TChar* Write = GetData();
                std::memmove(Write + Position, Write + Position + Removed, (Size - Position - Removed) * sizeof(TChar));
                SetSize(Size - Removed);
            }
            return *this;
        }

        TChar* erase(const TChar* Position)
        {
            const size_t Index = static_cast<size_t>(Position - GetData());
            erase(Index, 1);
            return GetData() + Index;
        }

        TChar* erase(const TChar* First, const TChar* Last)
        {
            const size_t Index = static_cast<size_t>(First - GetData());
            erase(Index, static_cast<size_t>(Last - First));
            return GetData() + Index;
        }

        TBasicString& replace(size_t Position, size_t Count, view_type Text)
        {
            erase(Position, Count);
            return insert(Position, Text);
        }

        NODISCARD TBasicString substr(size_t Position = 0, size_t Count = npos) const
        {
            const size_t Size = GetSize();
            LUMINA_CONTAINER_CHECK_WITHIN(Position, Size);
            const size_t Taken = Count < Size - Position ? Count : Size - Position;
            return TBasicString(GetData() + Position, Taken);
        }

        NODISCARD size_t find(view_type Text, size_t Position = 0) const noexcept { return View().find(Text, Position); }
        NODISCARD size_t find(TChar Character, size_t Position = 0) const noexcept { return View().find(Character, Position); }
        NODISCARD size_t rfind(view_type Text, size_t Position = npos) const noexcept { return View().rfind(Text, Position); }
        NODISCARD size_t rfind(TChar Character, size_t Position = npos) const noexcept { return View().rfind(Character, Position); }
        NODISCARD size_t find_first_of(view_type Set, size_t Position = 0) const noexcept { return View().find_first_of(Set, Position); }
        NODISCARD size_t find_first_of(TChar Character, size_t Position = 0) const noexcept { return View().find_first_of(Character, Position); }
        NODISCARD size_t find_last_of(view_type Set, size_t Position = npos) const noexcept { return View().find_last_of(Set, Position); }
        NODISCARD size_t find_last_of(TChar Character, size_t Position = npos) const noexcept { return View().find_last_of(Character, Position); }
        NODISCARD size_t find_first_not_of(view_type Set, size_t Position = 0) const noexcept { return View().find_first_not_of(Set, Position); }
        NODISCARD size_t find_first_not_of(TChar Character, size_t Position = 0) const noexcept { return View().find_first_not_of(Character, Position); }
        NODISCARD size_t find_last_not_of(view_type Set, size_t Position = npos) const noexcept { return View().find_last_not_of(Set, Position); }
        NODISCARD size_t find_last_not_of(TChar Character, size_t Position = npos) const noexcept { return View().find_last_not_of(Character, Position); }

        NODISCARD int compare(view_type Text) const noexcept { return View().compare(Text); }
        NODISCARD int compare(size_t Position, size_t Count, view_type Text) const noexcept { return View().compare(Position, Count, Text); }

        /** Ordering that folds ASCII case, for identifiers and paths that are compared case-insensitively. */
        NODISCARD int CompareIgnoreCase(view_type Text) const noexcept { return Containers::CompareIgnoreCase(View(), Text); }
        NODISCARD bool starts_with(view_type Text) const noexcept { return View().starts_with(Text); }
        NODISCARD bool ends_with(view_type Text) const noexcept { return View().ends_with(Text); }
        NODISCARD bool contains(view_type Text) const noexcept { return View().find(Text) != npos; }
        NODISCARD bool contains(TChar Character) const noexcept { return View().find(Character) != npos; }

        /** ASCII-only case folding, which is what asset paths and identifiers need. */
        void ToLower() noexcept
        {
            TChar* Write = GetData();
            for (size_t Index = 0, Size = GetSize(); Index < Size; ++Index)
            {
                if (Write[Index] >= TChar('A') && Write[Index] <= TChar('Z'))
                {
                    Write[Index] = static_cast<TChar>(Write[Index] - TChar('A') + TChar('a'));
                }
            }
        }

        void ToUpper() noexcept
        {
            TChar* Write = GetData();
            for (size_t Index = 0, Size = GetSize(); Index < Size; ++Index)
            {
                if (Write[Index] >= TChar('a') && Write[Index] <= TChar('z'))
                {
                    Write[Index] = static_cast<TChar>(Write[Index] - TChar('a') + TChar('A'));
                }
            }
        }

        NODISCARD bool EqualsIgnoreCase(view_type Text) const noexcept { return Containers::EqualsIgnoreCase(View(), Text); }

        void TrimStart() noexcept
        {
            const size_t First = View().find_first_not_of(WhitespaceSet());
            erase(0, First == npos ? GetSize() : First);
        }

        void TrimEnd() noexcept
        {
            const size_t Last = View().find_last_not_of(WhitespaceSet());
            SetSize(Last == npos ? 0 : Last + 1);
        }

        void Trim() noexcept
        {
            TrimEnd();
            TrimStart();
        }

        void swap(TBasicString& Other) noexcept
        {
            TBasicString Staged(std::move(*this));
            *this = std::move(Other);
            Other = std::move(Staged);
        }

        NODISCARD friend bool operator==(const TBasicString& A, const TBasicString& B) noexcept { return A.View() == B.View(); }
        NODISCARD friend bool operator==(const TBasicString& A, const TChar* B) noexcept { return A.View() == view_type(B); }
        NODISCARD friend bool operator==(const TBasicString& A, view_type B) noexcept { return A.View() == B; }

        NODISCARD friend auto operator<=>(const TBasicString& A, const TBasicString& B) noexcept { return A.View() <=> B.View(); }
        NODISCARD friend auto operator<=>(const TBasicString& A, const TChar* B) noexcept { return A.View() <=> view_type(B); }
        NODISCARD friend auto operator<=>(const TBasicString& A, view_type B) noexcept { return A.View() <=> B; }

        NODISCARD friend TBasicString operator+(const TBasicString& A, const TBasicString& B)
        {
            TBasicString Result(A);
            Result.append(B.GetData(), B.GetSize());
            return Result;
        }

        NODISCARD friend TBasicString operator+(const TBasicString& A, view_type B)
        {
            TBasicString Result(A);
            Result.append(B.data(), B.size());
            return Result;
        }

        NODISCARD friend TBasicString operator+(const TBasicString& A, const TChar* B)
        {
            TBasicString Result(A);
            Result.append(B);
            return Result;
        }

        NODISCARD friend TBasicString operator+(const TChar* A, const TBasicString& B)
        {
            TBasicString Result(A);
            Result.append(B.GetData(), B.GetSize());
            return Result;
        }


        NODISCARD friend TBasicString operator+(const TBasicString& A, TChar B)
        {
            TBasicString Result(A);
            Result.push_back(B);
            return Result;
        }

    private:

        NODISCARD static size_t TraitsLength(const TChar* Text) noexcept
        {
            return std::char_traits<TChar>::length(Text);
        }

        NODISCARD static constexpr view_type WhitespaceSet() noexcept
        {
            if constexpr (sizeof(TChar) == 1)
            {
                return view_type(reinterpret_cast<const TChar*>(" \t\n\r\f\v"), 6);
            }
            else
            {
                static constexpr TChar Set[] = { TChar(' '), TChar('\t'), TChar('\n'), TChar('\r'), TChar('\f'), TChar('\v'), TChar(0) };
                return view_type(Set, 6);
            }
        }

        FORCEINLINE void InitializeEmpty() noexcept
        {
            Store.Bytes[kModeByte] = bPackedSize ? static_cast<uint8>(SsoCapacity) : uint8(0);
            if constexpr (!bPackedSize)
            {
                WriteInlineSize(0);
            }
            Store.Inline[0] = TChar(0);
        }

        NODISCARD FORCEINLINE uint32 ReadInlineSize() const noexcept
        {
            uint32 Value = 0;
            std::memcpy(&Value, Store.Bytes + kSizeOffset, sizeof(Value));
            return Value;
        }

        FORCEINLINE void WriteInlineSize(size_t NewSize) noexcept
        {
            const uint32 Value = static_cast<uint32>(NewSize);
            std::memcpy(Store.Bytes + kSizeOffset, &Value, sizeof(Value));
        }

        NODISCARD FORCEINLINE bool IsHeap() const noexcept { return (Store.Bytes[kModeByte] & kHeapFlag) != 0; }

        NODISCARD FORCEINLINE TChar* GetData() noexcept { return IsHeap() ? Store.Heap.Data : Store.Inline; }
        NODISCARD FORCEINLINE const TChar* GetData() const noexcept { return IsHeap() ? Store.Heap.Data : Store.Inline; }

        NODISCARD FORCEINLINE size_t GetSize() const noexcept
        {
            if (IsHeap())
            {
                return Store.Heap.Size;
            }
            if constexpr (bPackedSize)
            {
                return SsoCapacity - Store.Bytes[kModeByte];
            }
            else
            {
                return ReadInlineSize();
            }
        }

        NODISCARD FORCEINLINE size_t GetCapacity() const noexcept
        {
            return IsHeap() ? (Store.Heap.Capacity & kCapacityMask) : SsoCapacity;
        }

        FORCEINLINE void SetSize(size_t NewSize) noexcept
        {
            if (IsHeap())
            {
                Store.Heap.Size = static_cast<uint32>(NewSize);
            }
            else if constexpr (bPackedSize)
            {
                Store.Bytes[kModeByte] = static_cast<uint8>(SsoCapacity - NewSize);
            }
            else
            {
                WriteInlineSize(NewSize);
            }
            GetData()[NewSize] = TChar(0);
        }

        // Capacity's top bit is the mode flag whenever the union is exactly the heap header, so mask it always.
        FORCEINLINE void MarkHeap(TChar* Data, size_t Size, size_t Capacity) noexcept
        {
            Store.Heap.Data     = Data;
            Store.Heap.Size     = static_cast<uint32>(Size);
            Store.Heap.Capacity = static_cast<uint32>(Capacity) & kCapacityMask;
            Store.Bytes[kModeByte] |= kHeapFlag;
        }

        FORCEINLINE void ReleaseHeapBlock() noexcept
        {
            if (IsHeap())
            {
                TAllocator::Deallocate(Store.Heap.Data, (GetCapacity() + 1) * sizeof(TChar), alignof(TChar));
            }
        }

        NODISCARD static size_t CalculateGrowth(size_t CurrentCapacity, size_t Required)
        {
            size_t Grown = CurrentCapacity < SsoCapacity ? SsoCapacity : CurrentCapacity * 2;
            if (Grown < Required)
            {
                Grown = Required;
            }
            LUMINA_CONTAINER_CHECK_WITHIN(Grown, MaxCapacity);
            return Grown;
        }

        FORCEINLINE void ReserveForGrowth(size_t Required)
        {
            if (Required > GetCapacity())
            {
                ReallocateTo(CalculateGrowth(GetCapacity(), Required));
            }
        }

        void ReallocateTo(size_t NewCapacity)
        {
            const size_t Size = GetSize();
            LUMINA_CONTAINER_CHECK(NewCapacity >= Size && NewCapacity <= MaxCapacity);

            const size_t NewBytes = (NewCapacity + 1) * sizeof(TChar);

            if (IsHeap())
            {
                const size_t OldBytes = (GetCapacity() + 1) * sizeof(TChar);
                if (NewCapacity > GetCapacity() && TAllocator::TryExpand(Store.Heap.Data, OldBytes, NewBytes))
                {
                    Store.Heap.Capacity = static_cast<uint32>(NewCapacity) & kCapacityMask;
                    Store.Bytes[kModeByte] |= kHeapFlag;
                    return;
                }

                TChar* NewData = static_cast<TChar*>(TAllocator::Allocate(NewBytes, alignof(TChar)));
                Memory::Memcpy(NewData, Store.Heap.Data, (Size + 1) * sizeof(TChar));
                TAllocator::Deallocate(Store.Heap.Data, OldBytes, alignof(TChar));
                MarkHeap(NewData, Size, NewCapacity);
                return;
            }

            TChar* NewData = static_cast<TChar*>(TAllocator::Allocate(NewBytes, alignof(TChar)));
            Memory::Memcpy(NewData, Store.Inline, (Size + 1) * sizeof(TChar));
            MarkHeap(NewData, Size, NewCapacity);
        }

        void Assign(const TChar* Text, size_t Length)
        {
            if (Length <= GetCapacity())
            {
                if (Length != 0)
                {
                    std::memmove(GetData(), Text, Length * sizeof(TChar));
                }
                SetSize(Length);
                return;
            }

            LUMINA_CONTAINER_CHECK_WITHIN(Length, MaxCapacity);

            // Fill the new block before releasing the old one, since Text may point into it.
            TChar* NewData = static_cast<TChar*>(TAllocator::Allocate((Length + 1) * sizeof(TChar), alignof(TChar)));
            Memory::Memcpy(NewData, Text, Length * sizeof(TChar));
            NewData[Length] = TChar(0);

            ReleaseHeapBlock();
            MarkHeap(NewData, Length, Length);
        }

        void AdoptOrCopy(TBasicString& Other) noexcept
        {
            if (Other.IsHeap())
            {
                MarkHeap(Other.Store.Heap.Data, Other.GetSize(), Other.GetCapacity());
                Other.InitializeEmpty();
            }
            else
            {
                const size_t Size = Other.GetSize();
                Memory::Memcpy(Store.Inline, Other.Store.Inline, (Size + 1) * sizeof(TChar));
                SetSize(Size);
                Other.InitializeEmpty();
            }
        }

        FStorage Store;
    };

    using FString  = TBasicString<char>;
    using FWString = TBasicString<wchar_t>;

    template <size_t N>
    using TFixedString = TBasicString<char, N>;

    // Matches the view hashers byte for byte, which is what makes heterogeneous string lookup work.
    template <typename TChar, size_t N, typename TAllocator>
    NODISCARD inline uint64 GetTypeHash(const TBasicString<TChar, N, TAllocator>& Str) noexcept
    {
        return HashBytes(Str.data(), Str.size() * sizeof(TChar));
    }

    template <typename TChar, size_t N, typename TAllocator>
    FORCEINLINE void swap(TBasicString<TChar, N, TAllocator>& A, TBasicString<TChar, N, TAllocator>& B) noexcept
    {
        A.swap(B);
    }
}

