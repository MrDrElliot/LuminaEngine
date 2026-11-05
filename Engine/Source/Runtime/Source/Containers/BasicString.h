#pragma once

#include <bit>
#include <compare>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
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
            int32  Size;
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
        static_assert(std::endian::native == std::endian::little, "The mode byte is the top byte of the heap capacity.");

        union FStorage
        {
            FHeapLayout Heap;
            TChar       Inline[kStorageBytes / sizeof(TChar)];
            uint8       Bytes[kStorageBytes];

            // Zeroed only while small, since a whole-union copy then reads every byte; larger ones are copied by length.
            FStorage() noexcept requires (kStorageBytes <= 32) : Bytes{} {}
            FStorage() noexcept requires (kStorageBytes > 32) {}
        };

    public:

        using ElementType            = TChar;
        using value_type             = TChar;
        using size_type              = int32;
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

        static constexpr int32 MaxLength           = static_cast<int32>(kCapacityMask);
        static constexpr int32 InlineCapacityChars = static_cast<int32>(SsoCapacity);

        TBasicString() noexcept { InitializeEmpty(); }

        TBasicString(const TChar* Text)
        {
            InitializeEmpty();
            AssignRaw(Text, Text != nullptr ? TraitsLength(Text) : 0);
        }

        TBasicString(const TChar* Text, int32 Length)
        {
            InitializeEmpty();
            AssignRaw(Text, CheckedCount(static_cast<uint32>(Length)));
        }

        TBasicString(view_type Text)
        {
            InitializeEmpty();
            AssignRaw(Text.GetData(), static_cast<uint32>(Text.Len()));
        }

        TBasicString(int32 Count, TChar Character)
        {
            InitializeEmpty();
            SetLen(Count, Character);
        }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TBasicString(TIter First, TIter Last)
        {
            InitializeEmpty();
            Append(First, Last);
        }

        TBasicString(const TBasicString& Other)
        {
            if constexpr (kStorageBytes <= 32)
            {
                if (!Other.IsHeap())
                {
                    Store = Other.Store;
                    return;
                }
            }
            InitializeEmpty();
            AssignRaw(Other.GetData(), Other.GetSize());
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
                if constexpr (kStorageBytes <= 32)
                {
                    if (!IsHeap() && !Other.IsHeap())
                    {
                        Store = Other.Store;
                        return *this;
                    }
                }
                AssignRaw(Other.GetData(), Other.GetSize());
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

        TBasicString& operator=(const TChar* Text) { AssignRaw(Text, Text != nullptr ? TraitsLength(Text) : 0); return *this; }
        TBasicString& operator=(view_type Text) { AssignRaw(Text.GetData(), static_cast<uint32>(Text.Len())); return *this; }

        NODISCARD static TBasicString ChrN(int32 Count, TChar Character) { return TBasicString(Count, Character); }

        NODISCARD FORCEINLINE const TChar* operator*() const noexcept { return GetData(); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto GetData(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, TChar>*
        {
            return Self.IsHeap() ? Self.Store.Heap.Data : Self.Store.Inline;
        }

        NODISCARD FORCEINLINE int32 Len() const noexcept { return static_cast<int32>(GetSize()); }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return GetSize() == 0; }
        NODISCARD FORCEINLINE int32 GetCapacity() const noexcept { return static_cast<int32>(GetCapacityRaw()); }
        NODISCARD FORCEINLINE size_t GetAllocatedSize() const noexcept { return IsHeap() ? (static_cast<size_t>(GetCapacityRaw()) + 1) * sizeof(TChar) : 0; }
        NODISCARD FORCEINLINE bool IsValidIndex(int32 Index) const noexcept { return static_cast<uint32>(Index) < GetSize(); }

        /** True while the characters live in the packed buffer rather than a heap block. */
        NODISCARD FORCEINLINE bool IsInline() const noexcept { return !IsHeap(); }

        NODISCARD FORCEINLINE operator view_type() const noexcept { return view_type(GetData(), Len()); }
        NODISCARD FORCEINLINE view_type View() const noexcept { return view_type(GetData(), Len()); }
        NODISCARD FORCEINLINE cview_type CView() const noexcept { return cview_type::FromTerminated(GetData(), Len()); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto begin(this TSelf&& Self) noexcept { return Self.GetData(); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto end(this TSelf&& Self) noexcept { return Self.GetData() + Self.GetSize(); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto rbegin(this TSelf&& Self) noexcept { return std::reverse_iterator(Self.end()); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto rend(this TSelf&& Self) noexcept { return std::reverse_iterator(Self.begin()); }

        // Strictly below Len: the terminator of a full inline string is the mode byte, and writing it corrupts the string.
        NODISCARD FORCEINLINE TChar& operator[](int32 Index) noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, GetSize());
            return GetData()[Index];
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto Last(this TSelf&& Self, int32 IndexFromEnd = 0) noexcept -> TLikeSelf<TSelf, TChar>&
        {
            return Self[Self.Len() - IndexFromEnd - 1];
        }

        NODISCARD FORCEINLINE const TChar& operator[](int32 Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_WITHIN(Index, GetSize());
            return GetData()[Index];
        }

        void Reserve(int32 InNewCapacity)
        {
            ReserveExact(CheckedCount(static_cast<uint32>(InNewCapacity)));
        }

        void Shrink()
        {
            if (!IsHeap())
            {
                return;
            }

            const uint32 Size = GetSize();
            if (Size <= SsoCapacity)
            {
                TChar* Old = Store.Heap.Data;
                const size_t OldBytes = (static_cast<size_t>(GetCapacityRaw()) + 1) * sizeof(TChar);
                InitializeEmpty();
                Memory::Memcpy(Store.Inline, Old, Size * sizeof(TChar));
                SetSize(Size);
                TAllocator::Deallocate(Old, OldBytes, alignof(TChar));
                return;
            }

            if (Size < GetCapacityRaw())
            {
                ReallocateTo(Size);
            }
        }

        /** Drops the characters and the block, keeping room for Slack. */
        void Empty(int32 Slack = 0)
        {
            ReleaseHeapBlock();
            InitializeEmpty();
            if (Slack > 0)
            {
                Reserve(Slack);
            }
        }

        /** Drops the characters but keeps the block, so refilling it does not reallocate. */
        void Reset(int32 NewReservedSize = 0)
        {
            SetSize(0);
            if (NewReservedSize > 0)
            {
                Reserve(NewReservedSize);
            }
        }

        void SetLen(int32 NewLen, TChar Fill = TChar(0))
        {
            ResizeTo(static_cast<uint32>(NewLen), Fill);
        }

        // Growing always ends on the heap, so both full cases reallocate and fall through to the heap store.
        FORCEINLINE void AppendChar(TChar Character)
        {
            uint32 Size;
            if (IsHeap())
            {
                Size = static_cast<uint32>(Store.Heap.Size);
                if (Size == (Store.Heap.Capacity & kCapacityMask)) [[unlikely]]
                {
                    ReallocateTo(CalculateGrowth(Size, static_cast<uint64>(Size) + 1));
                }
            }
            else
            {
                Size = GetInlineSize();
                if (Size != SsoCapacity) [[likely]]
                {
                    Store.Inline[Size] = Character;
                    SetInlineSize(Size + 1);
                    return;
                }
                ReallocateTo(CalculateGrowth(Size, static_cast<uint64>(Size) + 1));
            }

            TChar* Data = Store.Heap.Data;
            Data[Size] = Character;
            Data[Size + 1] = TChar(0);
            Store.Heap.Size = static_cast<int32>(Size + 1);
        }

        TBasicString& AppendChars(TChar Character, int32 Count)
        {
            ResizeTo(CheckedCount(static_cast<uint64>(GetSize()) + static_cast<uint32>(Count)), Character);
            return *this;
        }

        TBasicString& Append(const TChar* Text, int32 Length) { return AppendRaw(Text, static_cast<uint32>(Length)); }
        TBasicString& Append(view_type Text) { return AppendRaw(Text.GetData(), static_cast<uint32>(Text.Len())); }
        TBasicString& Append(const TChar* Text) { return AppendRaw(Text, LengthOrZero(Text)); }

        template <typename TIter>
        requires std::input_iterator<TIter>
        TBasicString& Append(TIter First, TIter Last)
        {
            if constexpr (std::contiguous_iterator<TIter> && std::is_same_v<std::remove_cv_t<std::iter_value_t<TIter>>, TChar>)
            {
                return AppendRaw(std::to_address(First), CheckedCount(static_cast<uint64>(Last - First)));
            }
            else
            {
                // Iterators over our own characters, such as reverse ones, are copied out before growth frees them.
                if constexpr (std::forward_iterator<TIter> && std::is_lvalue_reference_v<std::iter_reference_t<TIter>>)
                {
                    if (First != Last && OwnsAddress(std::addressof(*First)))
                    {
                        const TBasicString Staged(First, Last);
                        return AppendRaw(Staged.GetData(), Staged.GetSize());
                    }
                }
                if constexpr (std::forward_iterator<TIter>)
                {
                    ReserveForGrowth(static_cast<uint64>(GetSize()) + static_cast<uint64>(std::distance(First, Last)));
                }
                for (; First != Last; ++First)
                {
                    AppendChar(static_cast<TChar>(*First));
                }
                return *this;
            }
        }

        TBasicString& operator+=(view_type Text) { return AppendRaw(Text.GetData(), static_cast<uint32>(Text.Len())); }
        TBasicString& operator+=(const TChar* Text) { return AppendRaw(Text, LengthOrZero(Text)); }
        TBasicString& operator+=(const TBasicString& Other) { return AppendRaw(Other.GetData(), Other.GetSize()); }
        TBasicString& operator+=(TChar Character) { AppendChar(Character); return *this; }

        TBasicString& InsertAt(int32 InIndex, view_type Text)
        {
            const uint32 Index = static_cast<uint32>(InIndex);
            const uint32 Size = GetSize();
            LUMINA_CONTAINER_CHECK_WITHIN(Index, Size);

            if (Text.IsEmpty())
            {
                return *this;
            }

            // Text pointing into us is copied first, since both the growth and the shift below would move it.
            if (OwnsAddress(Text.GetData()))
            {
                const TBasicString Staged(Text);
                return InsertAt(InIndex, Staged.View());
            }

            const uint32 Length = static_cast<uint32>(Text.Len());
            ReserveForGrowth(static_cast<uint64>(Size) + Length);

            TChar* Write = GetData();
            std::memmove(Write + Index + Length, Write + Index, static_cast<size_t>(Size - Index) * sizeof(TChar));
            Memory::Memcpy(Write + Index, Text.GetData(), static_cast<size_t>(Length) * sizeof(TChar));
            SetSize(Size + Length);
            return *this;
        }

        TBasicString& InsertAt(int32 Index, TChar Character) { return InsertAt(Index, view_type(&Character, 1)); }

        // Clamped like Mid, so a range running past the end removes to the end.
        TBasicString& RemoveAt(int32 InIndex, int32 InCount = 1)
        {
            const uint32 Size = GetSize();
            const uint32 Index = static_cast<uint32>(InIndex);
            LUMINA_CONTAINER_CHECK_WITHIN(Index, Size);
            const uint32 Count = InCount < 0 ? 0u : static_cast<uint32>(InCount);
            const uint32 Removed = Count < Size - Index ? Count : Size - Index;
            if (Removed != 0)
            {
                TChar* Write = GetData();
                std::memmove(Write + Index, Write + Index + Removed, static_cast<size_t>(Size - Index - Removed) * sizeof(TChar));
                SetSize(Size - Removed);
            }
            return *this;
        }

        NODISCARD TBasicString Mid(int32 Start, int32 Count = MaxLength) const { return TBasicString(View().Mid(Start, Count)); }
        NODISCARD TBasicString Left(int32 Count) const { return TBasicString(View().Left(Count)); }
        NODISCARD TBasicString Right(int32 Count) const { return TBasicString(View().Right(Count)); }
        NODISCARD TBasicString LeftChop(int32 Count) const { return TBasicString(View().LeftChop(Count)); }
        NODISCARD TBasicString RightChop(int32 Count) const { return TBasicString(View().RightChop(Count)); }

        void MidInline(int32 Start, int32 Count = MaxLength) { *this = Mid(Start, Count); }
        void LeftInline(int32 Count) { SetSize(static_cast<uint32>(View().Left(Count).Len())); }
        void LeftChopInline(int32 Count) { SetSize(static_cast<uint32>(View().LeftChop(Count).Len())); }
        void RightChopInline(int32 Count) { RemoveAt(0, Len() - View().RightChop(Count).Len()); }

        NODISCARD int32 Find(view_type Text, int32 StartPosition = 0) const noexcept { return View().Find(Text, StartPosition); }
        NODISCARD int32 Find(TChar Character, int32 StartPosition = 0) const noexcept { return View().Find(Character, StartPosition); }
        NODISCARD int32 FindLast(view_type Text, int32 StartPosition = Constants::kIndexNone) const noexcept { return View().FindLast(Text, StartPosition); }
        NODISCARD int32 FindLast(TChar Character, int32 StartPosition = Constants::kIndexNone) const noexcept { return View().FindLast(Character, StartPosition); }
        NODISCARD bool FindChar(TChar Character, int32& OutIndex) const noexcept { return View().FindChar(Character, OutIndex); }
        NODISCARD bool FindLastChar(TChar Character, int32& OutIndex) const noexcept { return View().FindLastChar(Character, OutIndex); }
        NODISCARD int32 FindFirstOf(view_type Set, int32 StartPosition = 0) const noexcept { return View().FindFirstOf(Set, StartPosition); }
        NODISCARD int32 FindLastOf(view_type Set, int32 StartPosition = Constants::kIndexNone) const noexcept { return View().FindLastOf(Set, StartPosition); }
        NODISCARD int32 FindFirstOf(TChar Character, int32 StartPosition = 0) const noexcept { return View().Find(Character, StartPosition); }
        NODISCARD int32 FindLastOf(TChar Character, int32 StartPosition = Constants::kIndexNone) const noexcept { return View().FindLast(Character, StartPosition); }
        NODISCARD int32 FindFirstNotOf(view_type Set, int32 StartPosition = 0) const noexcept { return View().FindFirstNotOf(Set, StartPosition); }
        NODISCARD int32 FindLastNotOf(view_type Set, int32 StartPosition = Constants::kIndexNone) const noexcept { return View().FindLastNotOf(Set, StartPosition); }

        NODISCARD int Compare(view_type Text) const noexcept { return View().Compare(Text); }
        NODISCARD int CompareIgnoreCase(view_type Text) const noexcept { return Containers::CompareIgnoreCase(View(), Text); }
        NODISCARD bool Equals(view_type Text) const noexcept { return View() == Text; }
        NODISCARD bool EqualsIgnoreCase(view_type Text) const noexcept { return Containers::EqualsIgnoreCase(View(), Text); }
        NODISCARD bool StartsWith(view_type Text) const noexcept { return View().StartsWith(Text); }
        NODISCARD bool StartsWith(TChar Character) const noexcept { return View().StartsWith(Character); }
        NODISCARD bool EndsWith(view_type Text) const noexcept { return View().EndsWith(Text); }
        NODISCARD bool EndsWith(TChar Character) const noexcept { return View().EndsWith(Character); }
        NODISCARD bool Contains(view_type Text) const noexcept { return View().Find(Text) != Constants::kIndexNone; }
        NODISCARD bool Contains(TChar Character) const noexcept { return View().Find(Character) != Constants::kIndexNone; }

        /** Replaces every occurrence of From in place and returns how many there were. */
        int32 ReplaceInline(view_type From, view_type To)
        {
            const int32 First = From.IsEmpty() ? Constants::kIndexNone : Find(From);
            if (First == Constants::kIndexNone)
            {
                return 0;
            }

            // Not growing, the write cursor never passes the read one, so the characters still to be searched are untouched.
            if (To.Len() <= From.Len() && !OwnsAddress(From.GetData()) && !OwnsAddress(To.GetData()))
            {
                TChar* Data = GetData();
                const int32 Size = Len();
                int32 Read = 0;
                int32 Write = 0;
                int32 Replaced = 0;
                for (int32 Hit = First; Hit != Constants::kIndexNone; Hit = View().Find(From, Read))
                {
                    std::memmove(Data + Write, Data + Read, static_cast<size_t>(Hit - Read) * sizeof(TChar));
                    Write += Hit - Read;
                    Memory::Memcpy(Data + Write, To.GetData(), static_cast<size_t>(To.Len()) * sizeof(TChar));
                    Write += To.Len();
                    Read = Hit + From.Len();
                    ++Replaced;
                }
                std::memmove(Data + Write, Data + Read, static_cast<size_t>(Size - Read) * sizeof(TChar));
                SetSize(static_cast<uint32>(Write + Size - Read));
                return Replaced;
            }

            TBasicString Result;
            const int32 Replaced = BuildReplaced(Result, From, To, First);
            *this = std::move(Result);
            return Replaced;
        }

        NODISCARD TBasicString Replace(view_type From, view_type To) const
        {
            const int32 First = From.IsEmpty() ? Constants::kIndexNone : Find(From);
            if (First == Constants::kIndexNone)
            {
                return *this;
            }
            TBasicString Result;
            BuildReplaced(Result, From, To, First);
            return Result;
        }

        /** ASCII-only case folding in place, which is what asset paths and identifiers need. */
        void ToLower() noexcept { FoldCase<'A'>(); }
        void ToUpper() noexcept { FoldCase<'a'>(); }

        void TrimStart() noexcept { RemoveAt(0, Len() - View().TrimStart().Len()); }
        void TrimEnd() noexcept { SetSize(static_cast<uint32>(View().TrimEnd().Len())); }

        void Trim() noexcept
        {
            TrimEnd();
            TrimStart();
        }

        friend void swap(TBasicString& A, TBasicString& B) noexcept
        {
            TBasicString Staged(std::move(A));
            A = std::move(B);
            B = std::move(Staged);
        }

        NODISCARD friend bool operator==(const TBasicString& A, const TBasicString& B) noexcept { return A.View() == B.View(); }
        NODISCARD friend bool operator==(const TBasicString& A, const TChar* B) noexcept { return A.View() == view_type(B); }
        NODISCARD friend bool operator==(const TBasicString& A, view_type B) noexcept { return A.View() == B; }

        NODISCARD friend auto operator<=>(const TBasicString& A, const TBasicString& B) noexcept { return A.View() <=> B.View(); }
        NODISCARD friend auto operator<=>(const TBasicString& A, const TChar* B) noexcept { return A.View() <=> view_type(B); }
        NODISCARD friend auto operator<=>(const TBasicString& A, view_type B) noexcept { return A.View() <=> B; }

        // A temporary on the left appends into its own buffer, so a chain of + allocates about once.
        NODISCARD friend TBasicString operator+(const TBasicString& A, const TBasicString& B) { return Concat(A.View(), B.View()); }
        NODISCARD friend TBasicString operator+(const TBasicString& A, view_type B) { return Concat(A.View(), B); }
        NODISCARD friend TBasicString operator+(const TBasicString& A, const TChar* B) { return Concat(A.View(), ViewOf(B)); }
        NODISCARD friend TBasicString operator+(const TBasicString& A, TChar B) { return Concat(A.View(), view_type(&B, 1)); }
        // Only an actual view on the left, or FString + FFixedString would tie with this overload of the other capacity.
        template <std::same_as<view_type> TView>
        NODISCARD friend TBasicString operator+(TView A, const TBasicString& B) { return Concat(A, B.View()); }
        NODISCARD friend TBasicString operator+(const TChar* A, const TBasicString& B) { return Concat(ViewOf(A), B.View()); }

        NODISCARD friend TBasicString operator+(TBasicString&& A, const TBasicString& B) { A.AppendRaw(B.GetData(), B.GetSize()); return std::move(A); }
        NODISCARD friend TBasicString operator+(TBasicString&& A, view_type B) { A.Append(B); return std::move(A); }
        NODISCARD friend TBasicString operator+(TBasicString&& A, const TChar* B) { A.Append(B); return std::move(A); }
        NODISCARD friend TBasicString operator+(TBasicString&& A, TChar B) { A.AppendChar(B); return std::move(A); }

    private:

        NODISCARD static uint32 TraitsLength(const TChar* Text) noexcept
        {
            return CheckedCount(std::char_traits<TChar>::length(Text));
        }

        NODISCARD static uint32 LengthOrZero(const TChar* Text) noexcept
        {
            return Text != nullptr ? TraitsLength(Text) : 0;
        }

        NODISCARD static view_type ViewOf(const TChar* Text) noexcept
        {
            return view_type(Text, static_cast<int32>(LengthOrZero(Text)));
        }

        // Compared as integers, since ordering pointers into unrelated objects is unspecified.
        NODISCARD FORCEINLINE bool OwnsAddress(const void* Address) const noexcept
        {
            const uintptr_t Value = reinterpret_cast<uintptr_t>(Address);
            const uintptr_t Begin = reinterpret_cast<uintptr_t>(GetData());
            return Value >= Begin && Value <= Begin + static_cast<uintptr_t>(GetSize()) * sizeof(TChar);
        }

        NODISCARD static TBasicString Concat(view_type A, view_type B)
        {
            TBasicString Result;
            Result.ReserveExact(CheckedCount(static_cast<uint64>(A.Len()) + static_cast<uint64>(B.Len())));
            Result.AppendRaw(A.GetData(), static_cast<uint32>(A.Len()));
            Result.AppendRaw(B.GetData(), static_cast<uint32>(B.Len()));
            return Result;
        }

        // Counts the hits first, so Out is sized once; it only reads this string, so From and To may point into it.
        int32 BuildReplaced(TBasicString& Out, view_type From, view_type To, int32 First) const
        {
            int32 Count = 0;
            for (int32 Hit = First; Hit != Constants::kIndexNone; Hit = Find(From, Hit + From.Len()))
            {
                ++Count;
            }

            const int64 NewSize = static_cast<int64>(GetSize()) + static_cast<int64>(Count) * (static_cast<int64>(To.Len()) - From.Len());
            Out.ReserveExact(CheckedCount(static_cast<uint64>(NewSize)));

            const TChar* Data = GetData();
            int32 Cursor = 0;
            for (int32 Hit = First; Hit != Constants::kIndexNone; Hit = Find(From, Cursor))
            {
                Out.AppendRaw(Data + Cursor, static_cast<uint32>(Hit - Cursor));
                Out.AppendRaw(To.GetData(), static_cast<uint32>(To.Len()));
                Cursor = Hit + From.Len();
            }
            Out.AppendRaw(Data + Cursor, GetSize() - static_cast<uint32>(Cursor));
            return Count;
        }

        // A select rather than a branch, so the loop vectorizes.
        template <char kFirst>
        FORCEINLINE void FoldCase() noexcept
        {
            TChar* Write = GetData();
            for (uint32 Index = 0, Size = GetSize(); Index < Size; ++Index)
            {
                const uint32 Code = static_cast<uint32>(static_cast<std::make_unsigned_t<TChar>>(Write[Index]));
                Write[Index] = Code - static_cast<uint32>(kFirst) < 26u ? static_cast<TChar>(Code ^ 0x20u) : Write[Index];
            }
        }

        NODISCARD static uint32 CheckedCount(uint64 Count)
        {
            return static_cast<uint32>(Private::CheckCapacity(Count, kCapacityMask));
        }

        void ResizeTo(uint32 NewSize, TChar Character)
        {
            const uint32 Size = GetSize();
            if (NewSize > Size)
            {
                ReserveForGrowth(NewSize);
                TChar* Write = GetData();
                for (uint32 Index = Size; Index < NewSize; ++Index)
                {
                    Write[Index] = Character;
                }
            }
            SetSize(NewSize);
        }

        // Without growth the bytes written start past the end, so Text cannot overlap them even when it points into us.
        TBasicString& AppendRaw(const TChar* Text, uint32 Length)
        {
            if (Length == 0)
            {
                return *this;
            }

            if (IsHeap())
            {
                const uint32 Size = static_cast<uint32>(Store.Heap.Size);
                if (static_cast<uint64>(Size) + Length > (Store.Heap.Capacity & kCapacityMask)) [[unlikely]]
                {
                    return AppendGrowing(Text, Length);
                }
                TChar* Data = Store.Heap.Data;
                Memory::Memcpy(Data + Size, Text, static_cast<size_t>(Length) * sizeof(TChar));
                Data[Size + Length] = TChar(0);
                Store.Heap.Size = static_cast<int32>(Size + Length);
                return *this;
            }

            const uint32 Size = GetInlineSize();
            if (static_cast<uint64>(Size) + Length > SsoCapacity) [[unlikely]]
            {
                return AppendGrowing(Text, Length);
            }
            Memory::Memcpy(Store.Inline + Size, Text, static_cast<size_t>(Length) * sizeof(TChar));
            SetInlineSize(Size + Length);
            return *this;
        }

        // Growth frees the block Text may point into, so it is re-derived from the new one afterwards.
        FORCENOINLINE TBasicString& AppendGrowing(const TChar* Text, uint32 Length)
        {
            const uint32 Size = GetSize();
            const bool bAliasesSelf = OwnsAddress(Text);
            const size_t Offset = bAliasesSelf ? static_cast<size_t>(Text - GetData()) : 0;

            ReserveForGrowth(static_cast<uint64>(Size) + Length);

            TChar* Write = GetData();
            Memory::Memcpy(Write + Size, bAliasesSelf ? Write + Offset : Text, static_cast<size_t>(Length) * sizeof(TChar));
            SetSize(Size + Length);
            return *this;
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

        FORCEINLINE void WriteInlineSize(uint32 NewSize) noexcept
        {
            std::memcpy(Store.Bytes + kSizeOffset, &NewSize, sizeof(NewSize));
        }

        NODISCARD FORCEINLINE bool IsHeap() const noexcept { return (Store.Bytes[kModeByte] & kHeapFlag) != 0; }

        // The bound is what lets the compiler size every inline copy, and it is an invariant of the representation.
        NODISCARD FORCEINLINE uint32 GetInlineSize() const noexcept
        {
            uint32 Size;
            if constexpr (bPackedSize)
            {
                Size = static_cast<uint32>(SsoCapacity - Store.Bytes[kModeByte]);
            }
            else
            {
                Size = ReadInlineSize();
            }
            [[assume(Size <= SsoCapacity)]];
            return Size;
        }

        // The mode byte goes first: at full capacity the terminator is that same byte, and zero is its right value.
        FORCEINLINE void SetInlineSize(uint32 NewSize) noexcept
        {
            [[assume(NewSize <= SsoCapacity)]];
            if constexpr (bPackedSize)
            {
                Store.Bytes[kModeByte] = static_cast<uint8>(SsoCapacity - NewSize);
            }
            else
            {
                WriteInlineSize(NewSize);
            }
            Store.Inline[NewSize] = TChar(0);
        }

        NODISCARD FORCEINLINE uint32 GetSize() const noexcept
        {
            return IsHeap() ? static_cast<uint32>(Store.Heap.Size) : GetInlineSize();
        }

        NODISCARD FORCEINLINE uint32 GetCapacityRaw() const noexcept
        {
            return IsHeap() ? (Store.Heap.Capacity & kCapacityMask) : static_cast<uint32>(SsoCapacity);
        }

        FORCEINLINE void SetSize(uint32 NewSize) noexcept
        {
            if (IsHeap())
            {
                Store.Heap.Size = static_cast<int32>(NewSize);
                Store.Heap.Data[NewSize] = TChar(0);
            }
            else
            {
                SetInlineSize(NewSize);
            }
        }

        // Capacity's top bit is the mode flag whenever the union is exactly the heap header, so mask it always.
        FORCEINLINE void MarkHeap(TChar* Data, uint32 Size, uint32 Capacity) noexcept
        {
            Store.Heap.Data     = Data;
            Store.Heap.Size     = static_cast<int32>(Size);
            Store.Heap.Capacity = Capacity & kCapacityMask;
            Store.Bytes[kModeByte] |= kHeapFlag;
        }

        FORCEINLINE void ReleaseHeapBlock() noexcept
        {
            if (IsHeap())
            {
                TAllocator::Deallocate(Store.Heap.Data, (static_cast<size_t>(GetCapacityRaw()) + 1) * sizeof(TChar), alignof(TChar));
            }
        }

        NODISCARD static uint32 CalculateGrowth(uint32 CurrentCapacity, uint64 Required)
        {
            uint64 Grown = CurrentCapacity < SsoCapacity ? SsoCapacity : static_cast<uint64>(CurrentCapacity) * 2;
            if (Grown > kCapacityMask)
            {
                Grown = kCapacityMask;
            }
            if (Grown < Required)
            {
                Grown = Required;
            }
            return CheckedCount(Grown);
        }

        FORCEINLINE void ReserveForGrowth(uint64 Required)
        {
            if (Required > GetCapacityRaw())
            {
                ReallocateTo(CalculateGrowth(GetCapacityRaw(), Required));
            }
        }

        FORCEINLINE void ReserveExact(uint32 NewCapacity)
        {
            if (NewCapacity > GetCapacityRaw())
            {
                ReallocateTo(NewCapacity);
            }
        }

        // The heap hands out 16-byte classes, so rounding a block up to one is room the capacity gets for free.
        NODISCARD static uint32 RoundedCapacity(uint32 Capacity) noexcept
        {
            const size_t Bytes = ((static_cast<size_t>(Capacity) + 1) * sizeof(TChar) + 15) & ~static_cast<size_t>(15);
            const size_t Rounded = Bytes / sizeof(TChar) - 1;
            return Rounded > kCapacityMask ? kCapacityMask : static_cast<uint32>(Rounded);
        }

        FORCENOINLINE void ReallocateTo(uint32 RequestedCapacity)
        {
            const uint32 Size = GetSize();
            LUMINA_CONTAINER_CHECK(RequestedCapacity >= Size && RequestedCapacity <= kCapacityMask);

            const uint32 NewCapacity = RoundedCapacity(RequestedCapacity);
            const size_t NewBytes = (static_cast<size_t>(NewCapacity) + 1) * sizeof(TChar);

            if (IsHeap())
            {
                const size_t OldBytes = (static_cast<size_t>(GetCapacityRaw()) + 1) * sizeof(TChar);
                if (NewCapacity > GetCapacityRaw() && TAllocator::TryExpand(Store.Heap.Data, OldBytes, NewBytes))
                {
                    Store.Heap.Capacity = NewCapacity & kCapacityMask;
                    Store.Bytes[kModeByte] |= kHeapFlag;
                    return;
                }

                TChar* NewData = static_cast<TChar*>(TAllocator::Allocate(NewBytes, alignof(TChar)));
                Memory::Memcpy(NewData, Store.Heap.Data, (static_cast<size_t>(Size) + 1) * sizeof(TChar));
                TAllocator::Deallocate(Store.Heap.Data, OldBytes, alignof(TChar));
                MarkHeap(NewData, Size, NewCapacity);
                return;
            }

            // Leaving the inline buffer always asks for more than it holds, so a packed one copies whole in one fixed-size move.
            TChar* NewData = static_cast<TChar*>(TAllocator::Allocate(NewBytes, alignof(TChar)));
            if constexpr (bPackedSize)
            {
                Memory::Memcpy(NewData, Store.Inline, (SsoCapacity + 1) * sizeof(TChar));
            }
            else
            {
                Memory::Memcpy(NewData, Store.Inline, (static_cast<size_t>(Size) + 1) * sizeof(TChar));
            }
            MarkHeap(NewData, Size, NewCapacity);
        }

        void AssignRaw(const TChar* Text, uint32 Length)
        {
            if (Length <= GetCapacityRaw())
            {
                if (Length != 0)
                {
                    std::memmove(GetData(), Text, static_cast<size_t>(Length) * sizeof(TChar));
                }
                SetSize(Length);
                return;
            }

            LUMINA_CONTAINER_CHECK_WITHIN(Length, kCapacityMask);

            // Fill the new block before releasing the old one, since Text may point into it.
            const uint32 NewCapacity = RoundedCapacity(Length);
            TChar* NewData = static_cast<TChar*>(TAllocator::Allocate((static_cast<size_t>(NewCapacity) + 1) * sizeof(TChar), alignof(TChar)));
            Memory::Memcpy(NewData, Text, static_cast<size_t>(Length) * sizeof(TChar));
            NewData[Length] = TChar(0);

            ReleaseHeapBlock();
            MarkHeap(NewData, Length, NewCapacity);
        }

        // A small union moves as one fixed-size copy, which also carries the heap header and mode byte across.
        void AdoptOrCopy(TBasicString& Other) noexcept
        {
            if constexpr (kStorageBytes <= 32)
            {
                Store = Other.Store;
            }
            else if (Other.IsHeap())
            {
                MarkHeap(Other.Store.Heap.Data, Other.GetSize(), Other.GetCapacityRaw());
            }
            else
            {
                const uint32 Size = Other.GetSize();
                Memory::Memcpy(Store.Inline, Other.Store.Inline, (static_cast<size_t>(Size) + 1) * sizeof(TChar));
                SetSize(Size);
            }
            Other.InitializeEmpty();
        }

        FStorage Store;
    };

    using FString  = TBasicString<char>;
    using FWString = TBasicString<wchar_t>;

    template <size_t N>
    using TFixedString = TBasicString<char, N>;
}

namespace Lumina
{
    // Neither representation points into the object itself, since GetData recomputes the inline address.
    template <typename TChar, size_t N, typename TAllocator>
    struct TIsTriviallyRelocatable<Containers::TBasicString<TChar, N, TAllocator>>
    {
        static constexpr bool Value = true;
    };
}

namespace Lumina::Containers
{

    // Matches the view hashers byte for byte, which is what makes heterogeneous string lookup work.
    template <typename TChar, size_t N, typename TAllocator>
    NODISCARD inline uint64 GetTypeHash(const TBasicString<TChar, N, TAllocator>& Str) noexcept
    {
        return HashBytes(Str.GetData(), static_cast<size_t>(Str.Len()) * sizeof(TChar));
    }
}
