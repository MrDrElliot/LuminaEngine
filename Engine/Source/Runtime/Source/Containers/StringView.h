#pragma once

#include <compare>
#include <cstring>
#include <iterator>
#include <string_view>

#include "ContainerTraits.h"
#include "HashPrimitives.h"

namespace Lumina::Containers
{
    /** ASCII-only case folding, which is what asset paths and identifiers need. */
    template <typename TChar>
    NODISCARD constexpr TChar FoldAscii(TChar Character) noexcept
    {
        return (Character >= TChar('A') && Character <= TChar('Z'))
             ? static_cast<TChar>(Character - TChar('A') + TChar('a'))
             : Character;
    }

    /** Borrowed character range. Never owns and never terminates; see TCStringView when a c_str is required. */
    template <typename TChar>
    class TStringView
    {
    public:

        using value_type             = TChar;
        using size_type              = size_t;
        using difference_type        = ptrdiff_t;
        using reference              = const TChar&;
        using const_reference        = const TChar&;
        using pointer                = const TChar*;
        using const_pointer          = const TChar*;
        using iterator               = const TChar*;
        using const_iterator         = const TChar*;
        using reverse_iterator       = std::reverse_iterator<const TChar*>;
        using const_reverse_iterator = std::reverse_iterator<const TChar*>;
        using std_view_type          = std::basic_string_view<TChar>;

        static constexpr size_t npos = ~static_cast<size_t>(0);

        constexpr TStringView() noexcept = default;

        constexpr TStringView(const TChar* InData, size_t InSize) noexcept
            : Data(InData)
            , Size(InSize)
        {}

        constexpr TStringView(const TChar* InText) noexcept
            : Data(InText)
            , Size(InText != nullptr ? Length(InText) : 0)
        {}

        constexpr TStringView(std_view_type Other) noexcept
            : Data(Other.data())
            , Size(Other.size())
        {}

        constexpr TStringView(std::nullptr_t) = delete;

        NODISCARD constexpr operator std_view_type() const noexcept { return std_view_type(Data, Size); }

        NODISCARD FORCEINLINE constexpr const TChar* data() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr size_t size() const noexcept { return Size; }
        NODISCARD FORCEINLINE constexpr size_t length() const noexcept { return Size; }
        NODISCARD FORCEINLINE constexpr bool empty() const noexcept { return Size == 0; }
        NODISCARD static constexpr size_t max_size() noexcept { return npos - 1; }

        NODISCARD FORCEINLINE constexpr const TChar* begin() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr const TChar* end() const noexcept { return Data + Size; }
        NODISCARD FORCEINLINE constexpr const TChar* cbegin() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr const TChar* cend() const noexcept { return Data + Size; }

        NODISCARD constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        NODISCARD constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

        NODISCARD FORCEINLINE constexpr const TChar& operator[](size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Index < Size);
            return Data[Index];
        }

        NODISCARD FORCEINLINE constexpr const TChar& at(size_t Index) const noexcept { return (*this)[Index]; }

        NODISCARD FORCEINLINE constexpr const TChar& front() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Size != 0);
            return Data[0];
        }

        NODISCARD FORCEINLINE constexpr const TChar& back() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Size != 0);
            return Data[Size - 1];
        }

        constexpr void remove_prefix(size_t Count) noexcept
        {
            LUMINA_CONTAINER_CHECK(Count <= Size);
            Data += Count;
            Size -= Count;
        }

        constexpr void remove_suffix(size_t Count) noexcept
        {
            LUMINA_CONTAINER_CHECK(Count <= Size);
            Size -= Count;
        }

        FORCEINLINE constexpr void RemovePrefix(size_t Count) noexcept { remove_prefix(Count); }
        FORCEINLINE constexpr void RemoveSuffix(size_t Count) noexcept { remove_suffix(Count); }

        constexpr void swap(TStringView& Other) noexcept
        {
            const TChar* SwapData = Data;
            const size_t SwapSize = Size;
            Data = Other.Data;
            Size = Other.Size;
            Other.Data = SwapData;
            Other.Size = SwapSize;
        }

        NODISCARD constexpr TStringView substr(size_t Position = 0, size_t Count = npos) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Position <= Size);
            return TStringView(Data + Position, Clamp(Position, Count));
        }

        NODISCARD FORCEINLINE constexpr TStringView Left(size_t Count) const noexcept { return substr(0, Count); }
        NODISCARD FORCEINLINE constexpr TStringView Right(size_t Count) const noexcept { return substr(Size - (Count < Size ? Count : Size)); }

        NODISCARD constexpr int compare(TStringView Other) const noexcept
        {
            const size_t Shared = Size < Other.Size ? Size : Other.Size;
            for (size_t Index = 0; Index < Shared; ++Index)
            {
                if (Data[Index] != Other.Data[Index])
                {
                    return Data[Index] < Other.Data[Index] ? -1 : 1;
                }
            }
            if (Size == Other.Size)
            {
                return 0;
            }
            return Size < Other.Size ? -1 : 1;
        }

        NODISCARD constexpr int compare(size_t Position, size_t Count, TStringView Other) const noexcept
        {
            return substr(Position, Count).compare(Other);
        }

        NODISCARD constexpr bool starts_with(TStringView Prefix) const noexcept
        {
            return Size >= Prefix.Size && Match(Data, Prefix);
        }

        NODISCARD constexpr bool starts_with(TChar Character) const noexcept { return Size != 0 && Data[0] == Character; }

        NODISCARD constexpr bool ends_with(TStringView Suffix) const noexcept
        {
            return Size >= Suffix.Size && Match(Data + (Size - Suffix.Size), Suffix);
        }

        NODISCARD constexpr bool ends_with(TChar Character) const noexcept { return Size != 0 && Data[Size - 1] == Character; }

        NODISCARD constexpr size_t find(TStringView Needle, size_t Position = 0) const noexcept
        {
            if (Needle.Size == 0)
            {
                return Position <= Size ? Position : npos;
            }
            if (Position >= Size || Needle.Size > Size - Position)
            {
                return npos;
            }
            const size_t Last = Size - Needle.Size;
            for (size_t Index = Position; Index <= Last; ++Index)
            {
                if (Data[Index] == Needle.Data[0] && Match(Data + Index, Needle))
                {
                    return Index;
                }
            }
            return npos;
        }

        NODISCARD constexpr size_t find(TChar Character, size_t Position = 0) const noexcept
        {
            for (size_t Index = Position; Index < Size; ++Index)
            {
                if (Data[Index] == Character)
                {
                    return Index;
                }
            }
            return npos;
        }

        NODISCARD constexpr size_t rfind(TStringView Needle, size_t Position = npos) const noexcept
        {
            if (Needle.Size > Size)
            {
                return npos;
            }
            const size_t Highest = Size - Needle.Size;
            size_t Index = Position < Highest ? Position : Highest;
            for (;; --Index)
            {
                if (Match(Data + Index, Needle))
                {
                    return Index;
                }
                if (Index == 0)
                {
                    return npos;
                }
            }
        }

        NODISCARD constexpr size_t rfind(TChar Character, size_t Position = npos) const noexcept
        {
            if (Size == 0)
            {
                return npos;
            }
            size_t Index = Position < Size - 1 ? Position : Size - 1;
            for (;; --Index)
            {
                if (Data[Index] == Character)
                {
                    return Index;
                }
                if (Index == 0)
                {
                    return npos;
                }
            }
        }

        NODISCARD constexpr size_t find_first_of(TStringView Set, size_t Position = 0) const noexcept
        {
            for (size_t Index = Position; Index < Size; ++Index)
            {
                if (Set.find(Data[Index]) != npos)
                {
                    return Index;
                }
            }
            return npos;
        }

        NODISCARD FORCEINLINE constexpr size_t find_first_of(TChar Character, size_t Position = 0) const noexcept { return find(Character, Position); }

        NODISCARD constexpr size_t find_last_of(TStringView Set, size_t Position = npos) const noexcept
        {
            if (Size == 0)
            {
                return npos;
            }
            size_t Index = Position < Size - 1 ? Position : Size - 1;
            for (;; --Index)
            {
                if (Set.find(Data[Index]) != npos)
                {
                    return Index;
                }
                if (Index == 0)
                {
                    return npos;
                }
            }
        }

        NODISCARD FORCEINLINE constexpr size_t find_last_of(TChar Character, size_t Position = npos) const noexcept { return rfind(Character, Position); }

        NODISCARD constexpr size_t find_first_not_of(TStringView Set, size_t Position = 0) const noexcept
        {
            for (size_t Index = Position; Index < Size; ++Index)
            {
                if (Set.find(Data[Index]) == npos)
                {
                    return Index;
                }
            }
            return npos;
        }

        NODISCARD constexpr size_t find_first_not_of(TChar Character, size_t Position = 0) const noexcept
        {
            for (size_t Index = Position; Index < Size; ++Index)
            {
                if (Data[Index] != Character)
                {
                    return Index;
                }
            }
            return npos;
        }

        NODISCARD constexpr size_t find_last_not_of(TStringView Set, size_t Position = npos) const noexcept
        {
            if (Size == 0)
            {
                return npos;
            }
            size_t Index = Position < Size - 1 ? Position : Size - 1;
            for (;; --Index)
            {
                if (Set.find(Data[Index]) == npos)
                {
                    return Index;
                }
                if (Index == 0)
                {
                    return npos;
                }
            }
        }

        NODISCARD constexpr size_t find_last_not_of(TChar Character, size_t Position = npos) const noexcept
        {
            if (Size == 0)
            {
                return npos;
            }
            size_t Index = Position < Size - 1 ? Position : Size - 1;
            for (;; --Index)
            {
                if (Data[Index] != Character)
                {
                    return Index;
                }
                if (Index == 0)
                {
                    return npos;
                }
            }
        }

        NODISCARD FORCEINLINE constexpr bool contains(TStringView Needle) const noexcept { return find(Needle) != npos; }
        NODISCARD FORCEINLINE constexpr bool contains(TChar Character) const noexcept { return find(Character) != npos; }

        NODISCARD friend constexpr bool operator==(TStringView A, TStringView B) noexcept
        {
            return A.Size == B.Size && A.compare(B) == 0;
        }

        NODISCARD friend constexpr std::strong_ordering operator<=>(TStringView A, TStringView B) noexcept
        {
            const int Result = A.compare(B);
            return Result < 0 ? std::strong_ordering::less
                 : Result > 0 ? std::strong_ordering::greater
                              : std::strong_ordering::equal;
        }

        // Exact-match overloads, so a mixed comparison does not tie our operator against the standard one.
        NODISCARD friend constexpr bool operator==(TStringView A, std_view_type B) noexcept { return A == TStringView(B); }
        NODISCARD friend constexpr bool operator==(TStringView A, const TChar* B) noexcept { return A == TStringView(B); }

        NODISCARD friend constexpr std::strong_ordering operator<=>(TStringView A, std_view_type B) noexcept { return A <=> TStringView(B); }
        NODISCARD friend constexpr std::strong_ordering operator<=>(TStringView A, const TChar* B) noexcept { return A <=> TStringView(B); }

        NODISCARD static constexpr size_t Length(const TChar* Text) noexcept
        {
            size_t Count = 0;
            while (Text[Count] != TChar(0))
            {
                ++Count;
            }
            return Count;
        }

    private:

        NODISCARD constexpr size_t Clamp(size_t Position, size_t Count) const noexcept
        {
            const size_t Available = Size - Position;
            return Count < Available ? Count : Available;
        }

        NODISCARD static constexpr bool Match(const TChar* At, TStringView Needle) noexcept
        {
            for (size_t Index = 0; Index < Needle.Size; ++Index)
            {
                if (At[Index] != Needle.Data[Index])
                {
                    return false;
                }
            }
            return true;
        }

        const TChar* Data = nullptr;
        size_t       Size = 0;
    };

    /** A view that additionally guarantees a terminator, so it can be handed to any C API without a copy. */
    template <typename TChar>
    class TCStringView
    {
    public:

        using value_type    = TChar;
        using size_type     = size_t;
        using const_pointer = const TChar*;
        using view_type     = TStringView<TChar>;

        static constexpr size_t npos = view_type::npos;

        constexpr TCStringView() noexcept = default;

        constexpr TCStringView(const TChar* InText) noexcept
            : Data(InText != nullptr ? InText : EmptyLiteral)
            , Size(InText != nullptr ? view_type::Length(InText) : 0)
        {}

        constexpr TCStringView(std::nullptr_t) = delete;

        /** For a caller that already knows the terminator is there, so the length need not be recomputed. */
        NODISCARD static constexpr TCStringView FromTerminated(const TChar* InText, size_t InSize) noexcept
        {
            LUMINA_CONTAINER_CHECK(InText != nullptr && InText[InSize] == TChar(0));
            return TCStringView(InText, InSize, FTrusted{});
        }

        NODISCARD FORCEINLINE constexpr const TChar* c_str() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr const TChar* data() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr size_t size() const noexcept { return Size; }
        NODISCARD FORCEINLINE constexpr size_t length() const noexcept { return Size; }
        NODISCARD FORCEINLINE constexpr bool empty() const noexcept { return Size == 0; }

        NODISCARD FORCEINLINE constexpr view_type View() const noexcept { return view_type(Data, Size); }
        NODISCARD FORCEINLINE constexpr operator view_type() const noexcept { return View(); }
        NODISCARD FORCEINLINE constexpr operator std::basic_string_view<TChar>() const noexcept { return std::basic_string_view<TChar>(Data, Size); }

        NODISCARD FORCEINLINE constexpr const TChar* begin() const noexcept { return Data; }
        NODISCARD FORCEINLINE constexpr const TChar* end() const noexcept { return Data + Size; }

        NODISCARD FORCEINLINE constexpr const TChar& operator[](size_t Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK(Index <= Size);
            return Data[Index];
        }

        /** Dropping from the front keeps the terminator, which is why there is no matching RemoveSuffix. */
        constexpr void RemovePrefix(size_t Count) noexcept
        {
            LUMINA_CONTAINER_CHECK(Count <= Size);
            Data += Count;
            Size -= Count;
        }

        NODISCARD FORCEINLINE constexpr int compare(view_type Other) const noexcept { return View().compare(Other); }
        NODISCARD FORCEINLINE constexpr bool starts_with(view_type Prefix) const noexcept { return View().starts_with(Prefix); }
        NODISCARD FORCEINLINE constexpr bool ends_with(view_type Suffix) const noexcept { return View().ends_with(Suffix); }
        NODISCARD FORCEINLINE constexpr bool contains(view_type Needle) const noexcept { return View().contains(Needle); }
        NODISCARD FORCEINLINE constexpr size_t find(view_type Needle, size_t Position = 0) const noexcept { return View().find(Needle, Position); }
        NODISCARD FORCEINLINE constexpr size_t find(TChar Character, size_t Position = 0) const noexcept { return View().find(Character, Position); }
        NODISCARD FORCEINLINE constexpr size_t rfind(TChar Character, size_t Position = npos) const noexcept { return View().rfind(Character, Position); }

        NODISCARD friend constexpr bool operator==(TCStringView A, TCStringView B) noexcept { return A.View() == B.View(); }
        NODISCARD friend constexpr std::strong_ordering operator<=>(TCStringView A, TCStringView B) noexcept { return A.View() <=> B.View(); }

    private:

        struct FTrusted {};

        constexpr TCStringView(const TChar* InText, size_t InSize, FTrusted) noexcept
            : Data(InText)
            , Size(InSize)
        {}

        static constexpr TChar EmptyLiteral[1] = { TChar(0) };

        const TChar* Data = EmptyLiteral;
        size_t       Size = 0;
    };

    template <typename TChar>
    NODISCARD constexpr int CompareIgnoreCase(TStringView<TChar> A, TStringView<TChar> B) noexcept
    {
        const size_t Shared = A.size() < B.size() ? A.size() : B.size();
        for (size_t Index = 0; Index < Shared; ++Index)
        {
            const TChar Left = FoldAscii(A[Index]);
            const TChar Right = FoldAscii(B[Index]);
            if (Left != Right)
            {
                return Left < Right ? -1 : 1;
            }
        }
        if (A.size() == B.size())
        {
            return 0;
        }
        return A.size() < B.size() ? -1 : 1;
    }

    template <typename TChar>
    NODISCARD constexpr bool EqualsIgnoreCase(TStringView<TChar> A, TStringView<TChar> B) noexcept
    {
        return A.size() == B.size() && CompareIgnoreCase(A, B) == 0;
    }

    template <typename TChar>
    NODISCARD inline uint64 GetTypeHash(TStringView<TChar> View) noexcept
    {
        return HashBytes(View.data(), View.size() * sizeof(TChar));
    }

    template <typename TChar>
    NODISCARD inline uint64 GetTypeHash(TCStringView<TChar> View) noexcept
    {
        return HashBytes(View.data(), View.size() * sizeof(TChar));
    }

    template <typename TChar>
    FORCEINLINE constexpr void swap(TStringView<TChar>& A, TStringView<TChar>& B) noexcept
    {
        A.swap(B);
    }
}

namespace Lumina
{
    using FStringView  = Containers::TStringView<char>;
    using FCStringView = Containers::TCStringView<char>;
}
