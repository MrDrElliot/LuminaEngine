#pragma once

#include <compare>
#include <cstring>
#include <iterator>
#include <string>
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

    /** Borrowed character range. Never owns and never terminates; see TCStringView when a C string is required. */
    template <typename TChar>
    class TStringView
    {
    public:

        using ElementType            = TChar;
        using value_type             = TChar;
        using size_type              = int32;
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

        static constexpr int32 MaxLength = 0x7FFFFFFF;

        constexpr TStringView() noexcept = default;

        constexpr TStringView(const TChar* InData, int32 InLen) noexcept
            : Chars(InData)
            , Count(InLen)
        {
            LUMINA_CONTAINER_CHECK(InLen >= 0);
        }

        constexpr TStringView(const TChar* InText) noexcept
            : Chars(InText)
            , Count(InText != nullptr ? StrLen(InText) : 0)
        {}

        constexpr TStringView(std_view_type Other) noexcept
            : Chars(Other.data())
            , Count(static_cast<int32>(Other.size()))
        {
            LUMINA_CONTAINER_CHECK(Other.size() <= static_cast<size_t>(MaxLength));
        }

        constexpr TStringView(std::nullptr_t) = delete;

        NODISCARD constexpr operator std_view_type() const noexcept { return std_view_type(Chars, USize()); }

        NODISCARD FORCEINLINE constexpr const TChar* GetData() const noexcept { return Chars; }
        NODISCARD FORCEINLINE constexpr int32 Len() const noexcept { return Count; }
        NODISCARD FORCEINLINE constexpr bool IsEmpty() const noexcept { return Count == 0; }
        NODISCARD FORCEINLINE constexpr bool IsValidIndex(int32 Index) const noexcept { return static_cast<uint32>(Index) < USize(); }

        NODISCARD FORCEINLINE constexpr const TChar* begin() const noexcept { return Chars; }
        NODISCARD FORCEINLINE constexpr const TChar* end() const noexcept { return Chars + USize(); }
        NODISCARD constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        NODISCARD constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

        NODISCARD FORCEINLINE constexpr const TChar& operator[](int32 Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_INDEX(Index, USize());
            return Chars[Index];
        }

        NODISCARD FORCEINLINE constexpr const TChar& Last(int32 IndexFromEnd = 0) const noexcept { return (*this)[Len() - IndexFromEnd - 1]; }

        constexpr void RemovePrefix(int32 InCount) noexcept
        {
            LUMINA_CONTAINER_CHECK_WITHIN(InCount, USize());
            Chars += InCount;
            Count -= InCount;
        }

        constexpr void RemoveSuffix(int32 InCount) noexcept
        {
            LUMINA_CONTAINER_CHECK_WITHIN(InCount, USize());
            Count -= InCount;
        }

        // Clamped like Unreal's, so a start or count past either end yields what overlaps rather than asserting.
        NODISCARD constexpr TStringView Mid(int32 Start, int32 InCount = MaxLength) const noexcept
        {
            const uint32 First = Start < 0 ? 0u : (static_cast<uint32>(Start) < USize() ? static_cast<uint32>(Start) : USize());
            const uint32 Wanted = InCount < 0 ? 0u : static_cast<uint32>(InCount);
            const uint32 Available = USize() - First;
            return FromRaw(Chars + First, Wanted < Available ? Wanted : Available);
        }

        NODISCARD FORCEINLINE constexpr TStringView Left(int32 InCount) const noexcept { return Mid(0, InCount); }
        NODISCARD FORCEINLINE constexpr TStringView LeftChop(int32 InCount) const noexcept { return Mid(0, Len() - (InCount < 0 ? 0 : InCount)); }
        NODISCARD FORCEINLINE constexpr TStringView Right(int32 InCount) const noexcept
        {
            const int32 Taken = InCount < 0 ? 0 : (InCount < Len() ? InCount : Len());
            return Mid(Len() - Taken, Taken);
        }
        NODISCARD FORCEINLINE constexpr TStringView RightChop(int32 InCount) const noexcept { return Mid(InCount < 0 ? 0 : InCount); }

        NODISCARD constexpr TStringView TrimStart() const noexcept
        {
            uint32 Index = 0;
            while (Index < USize() && IsWhitespace(Chars[Index]))
            {
                ++Index;
            }
            return FromRaw(Chars + Index, USize() - Index);
        }

        NODISCARD constexpr TStringView TrimEnd() const noexcept
        {
            uint32 End = USize();
            while (End > 0 && IsWhitespace(Chars[End - 1]))
            {
                --End;
            }
            return FromRaw(Chars, End);
        }

        NODISCARD FORCEINLINE constexpr TStringView TrimStartAndEnd() const noexcept { return TrimStart().TrimEnd(); }

        // Through the traits, which is memcmp for char: vectorized, and unsigned on every platform as Equals already is.
        NODISCARD constexpr int Compare(TStringView Other) const noexcept
        {
            const uint32 Shared = USize() < Other.USize() ? USize() : Other.USize();
            if (Shared != 0)
            {
                if (const int Order = std::char_traits<TChar>::compare(Chars, Other.Chars, Shared); Order != 0)
                {
                    return Order < 0 ? -1 : 1;
                }
            }
            if (USize() == Other.USize())
            {
                return 0;
            }
            return USize() < Other.USize() ? -1 : 1;
        }

        NODISCARD FORCEINLINE constexpr bool Equals(TStringView Other) const noexcept
        {
            return Count == Other.Count && (Count == 0 || std::char_traits<TChar>::compare(Chars, Other.Chars, USize()) == 0);
        }

        NODISCARD constexpr bool StartsWith(TStringView Prefix) const noexcept
        {
            return USize() >= Prefix.USize() && Match(Chars, Prefix);
        }

        NODISCARD constexpr bool StartsWith(TChar Character) const noexcept { return USize() != 0 && Chars[0] == Character; }

        NODISCARD constexpr bool EndsWith(TStringView Suffix) const noexcept
        {
            return USize() >= Suffix.USize() && Match(Chars + (USize() - Suffix.USize()), Suffix);
        }

        NODISCARD constexpr bool EndsWith(TChar Character) const noexcept { return USize() != 0 && Chars[USize() - 1] == Character; }

        // Positions read as unsigned, so a negative start finds nothing and a negative reverse start searches everything.
        NODISCARD constexpr int32 Find(TStringView Needle, int32 StartPosition = 0) const noexcept
        {
            const uint32 Position = static_cast<uint32>(StartPosition);
            if (Needle.USize() == 0)
            {
                return Position <= USize() ? static_cast<int32>(Position) : Constants::kIndexNone;
            }
            if (Position >= USize() || Needle.USize() > USize() - Position)
            {
                return Constants::kIndexNone;
            }
            const TChar* Cursor = Chars + Position;
            const TChar* Last = Chars + (USize() - Needle.USize());
            while (Cursor <= Last)
            {
                Cursor = std::char_traits<TChar>::find(Cursor, static_cast<size_t>(Last - Cursor) + 1, Needle.Chars[0]);
                if (Cursor == nullptr)
                {
                    return Constants::kIndexNone;
                }
                if (Match(Cursor, Needle))
                {
                    return static_cast<int32>(Cursor - Chars);
                }
                ++Cursor;
            }
            return Constants::kIndexNone;
        }

        NODISCARD constexpr int32 Find(TChar Character, int32 StartPosition = 0) const noexcept
        {
            const uint32 Position = static_cast<uint32>(StartPosition);
            if (Position >= USize())
            {
                return Constants::kIndexNone;
            }
            const TChar* Hit = std::char_traits<TChar>::find(Chars + Position, USize() - Position, Character);
            return Hit != nullptr ? static_cast<int32>(Hit - Chars) : Constants::kIndexNone;
        }

        NODISCARD constexpr int32 FindLast(TStringView Needle, int32 StartPosition = Constants::kIndexNone) const noexcept
        {
            if (Needle.USize() > USize())
            {
                return Constants::kIndexNone;
            }
            const uint32 Position = static_cast<uint32>(StartPosition);
            const uint32 Highest = USize() - Needle.USize();
            for (uint32 Index = Position < Highest ? Position : Highest;; --Index)
            {
                if ((Needle.USize() == 0 || Chars[Index] == Needle.Chars[0]) && Match(Chars + Index, Needle))
                {
                    return static_cast<int32>(Index);
                }
                if (Index == 0)
                {
                    return Constants::kIndexNone;
                }
            }
        }

        NODISCARD constexpr int32 FindLast(TChar Character, int32 StartPosition = Constants::kIndexNone) const noexcept
        {
            return ScanBackward(StartPosition, [Character](TChar Value) { return Value == Character; });
        }

        NODISCARD FORCEINLINE constexpr bool FindChar(TChar Character, int32& OutIndex) const noexcept
        {
            OutIndex = Find(Character);
            return OutIndex != Constants::kIndexNone;
        }

        NODISCARD FORCEINLINE constexpr bool FindLastChar(TChar Character, int32& OutIndex) const noexcept
        {
            OutIndex = FindLast(Character);
            return OutIndex != Constants::kIndexNone;
        }

        NODISCARD constexpr int32 FindFirstOf(TStringView Set, int32 StartPosition = 0) const noexcept
        {
            const FCharSet Members(Set);
            return ScanForward(StartPosition, [&Members](TChar Value) { return Members.Contains(Value); });
        }

        NODISCARD constexpr int32 FindLastOf(TStringView Set, int32 StartPosition = Constants::kIndexNone) const noexcept
        {
            const FCharSet Members(Set);
            return ScanBackward(StartPosition, [&Members](TChar Value) { return Members.Contains(Value); });
        }

        NODISCARD FORCEINLINE constexpr int32 FindFirstOf(TChar Character, int32 StartPosition = 0) const noexcept { return Find(Character, StartPosition); }
        NODISCARD FORCEINLINE constexpr int32 FindLastOf(TChar Character, int32 StartPosition = Constants::kIndexNone) const noexcept { return FindLast(Character, StartPosition); }

        NODISCARD constexpr int32 FindFirstNotOf(TStringView Set, int32 StartPosition = 0) const noexcept
        {
            const FCharSet Members(Set);
            return ScanForward(StartPosition, [&Members](TChar Value) { return !Members.Contains(Value); });
        }

        NODISCARD constexpr int32 FindLastNotOf(TStringView Set, int32 StartPosition = Constants::kIndexNone) const noexcept
        {
            const FCharSet Members(Set);
            return ScanBackward(StartPosition, [&Members](TChar Value) { return !Members.Contains(Value); });
        }

        NODISCARD FORCEINLINE constexpr bool Contains(TStringView Needle) const noexcept { return Find(Needle) != Constants::kIndexNone; }
        NODISCARD FORCEINLINE constexpr bool Contains(TChar Character) const noexcept { return Find(Character) != Constants::kIndexNone; }

        NODISCARD friend constexpr bool operator==(TStringView A, TStringView B) noexcept
        {
            return A.Equals(B);
        }

        NODISCARD friend constexpr std::strong_ordering operator<=>(TStringView A, TStringView B) noexcept
        {
            const int Result = A.Compare(B);
            return Result < 0 ? std::strong_ordering::less
                 : Result > 0 ? std::strong_ordering::greater
                              : std::strong_ordering::equal;
        }

        // Exact-match overloads, so a mixed comparison does not tie our operator against the standard one.
        NODISCARD friend constexpr bool operator==(TStringView A, std_view_type B) noexcept { return A == TStringView(B); }
        NODISCARD friend constexpr bool operator==(TStringView A, const TChar* B) noexcept { return A == TStringView(B); }

        NODISCARD friend constexpr std::strong_ordering operator<=>(TStringView A, std_view_type B) noexcept { return A <=> TStringView(B); }
        NODISCARD friend constexpr std::strong_ordering operator<=>(TStringView A, const TChar* B) noexcept { return A <=> TStringView(B); }

        // The traits fold a literal's length at compile time and call the vectorized strlen at run time.
        NODISCARD static constexpr int32 StrLen(const TChar* Text) noexcept
        {
            const size_t Length = std::char_traits<TChar>::length(Text);
            LUMINA_CONTAINER_CHECK(Length <= static_cast<size_t>(MaxLength));
            return static_cast<int32>(Length);
        }

    private:

        // A one-byte set is a 256-bit table, so membership is one test per character rather than a scan of the set.
        class FCharSet
        {
        public:

            explicit constexpr FCharSet(TStringView Set) noexcept
                : Members(Set)
            {
                if constexpr (sizeof(TChar) == 1)
                {
                    for (const TChar Character : Set)
                    {
                        const uint32 Code = static_cast<uint8>(Character);
                        Bits[Code >> 6] |= uint64(1) << (Code & 63);
                    }
                }
            }

            NODISCARD constexpr bool Contains(TChar Character) const noexcept
            {
                if constexpr (sizeof(TChar) == 1)
                {
                    const uint32 Code = static_cast<uint8>(Character);
                    return ((Bits[Code >> 6] >> (Code & 63)) & 1) != 0;
                }
                else
                {
                    return Members.Find(Character) != Constants::kIndexNone;
                }
            }

        private:

            TStringView Members;
            uint64      Bits[4] = {};
        };

        NODISCARD static constexpr bool IsWhitespace(TChar Character) noexcept
        {
            return Character == TChar(' ') || Character == TChar('\t') || Character == TChar('\n')
                || Character == TChar('\r') || Character == TChar('\f') || Character == TChar('\v');
        }

        template <typename TPredicate>
        NODISCARD constexpr int32 ScanForward(int32 StartPosition, TPredicate Predicate) const noexcept
        {
            for (uint32 Index = static_cast<uint32>(StartPosition); Index < USize(); ++Index)
            {
                if (Predicate(Chars[Index]))
                {
                    return static_cast<int32>(Index);
                }
            }
            return Constants::kIndexNone;
        }

        template <typename TPredicate>
        NODISCARD constexpr int32 ScanBackward(int32 StartPosition, TPredicate Predicate) const noexcept
        {
            if (USize() == 0)
            {
                return Constants::kIndexNone;
            }
            const uint32 Position = static_cast<uint32>(StartPosition);
            for (uint32 Index = Position < USize() - 1 ? Position : USize() - 1;; --Index)
            {
                if (Predicate(Chars[Index]))
                {
                    return static_cast<int32>(Index);
                }
                if (Index == 0)
                {
                    return Constants::kIndexNone;
                }
            }
        }

        NODISCARD static constexpr TStringView FromRaw(const TChar* InData, uint32 InCount) noexcept
        {
            TStringView Result;
            Result.Chars = InData;
            Result.Count = static_cast<int32>(InCount);
            return Result;
        }

        NODISCARD static constexpr bool Match(const TChar* At, TStringView Needle) noexcept
        {
            return Needle.USize() == 0 || std::char_traits<TChar>::compare(At, Needle.Chars, Needle.USize()) == 0;
        }

        NODISCARD FORCEINLINE constexpr uint32 USize() const noexcept { return static_cast<uint32>(Count); }

        const TChar* Chars = nullptr;
        int32        Count = 0;
    };

    /** A view that additionally guarantees a terminator, so it can be handed to any C API without a copy. */
    template <typename TChar>
    class TCStringView
    {
    public:

        using ElementType   = TChar;
        using value_type    = TChar;
        using size_type     = int32;
        using const_pointer = const TChar*;
        using view_type     = TStringView<TChar>;

        constexpr TCStringView() noexcept = default;

        constexpr TCStringView(const TChar* InText) noexcept
            : Chars(InText != nullptr ? InText : EmptyLiteral)
            , Count(InText != nullptr ? view_type::StrLen(InText) : 0)
        {}

        constexpr TCStringView(std::nullptr_t) = delete;

        /** For a caller that already knows the terminator is there, so the length need not be recomputed. */
        NODISCARD static constexpr TCStringView FromTerminated(const TChar* InText, int32 InLen) noexcept
        {
            LUMINA_CONTAINER_CHECK(InText != nullptr && InLen >= 0 && InText[InLen] == TChar(0));
            return TCStringView(InText, InLen, FTrusted{});
        }

        NODISCARD FORCEINLINE constexpr const TChar* operator*() const noexcept { return Chars; }
        NODISCARD FORCEINLINE constexpr const TChar* GetData() const noexcept { return Chars; }
        NODISCARD FORCEINLINE constexpr int32 Len() const noexcept { return Count; }
        NODISCARD FORCEINLINE constexpr bool IsEmpty() const noexcept { return Count == 0; }

        NODISCARD FORCEINLINE constexpr view_type View() const noexcept { return view_type(Chars, Count); }
        NODISCARD FORCEINLINE constexpr operator view_type() const noexcept { return View(); }
        NODISCARD FORCEINLINE constexpr operator std::basic_string_view<TChar>() const noexcept { return std::basic_string_view<TChar>(Chars, static_cast<size_t>(Count)); }

        NODISCARD FORCEINLINE constexpr const TChar* begin() const noexcept { return Chars; }
        NODISCARD FORCEINLINE constexpr const TChar* end() const noexcept { return Chars + Count; }

        NODISCARD FORCEINLINE constexpr const TChar& operator[](int32 Index) const noexcept
        {
            LUMINA_CONTAINER_CHECK_WITHIN(Index, Count);
            return Chars[Index];
        }

        /** Dropping from the front keeps the terminator, which is why there is no matching RemoveSuffix. */
        constexpr void RemovePrefix(int32 InCount) noexcept
        {
            LUMINA_CONTAINER_CHECK_WITHIN(InCount, Count);
            Chars += InCount;
            Count -= InCount;
        }

        NODISCARD FORCEINLINE constexpr int Compare(view_type Other) const noexcept { return View().Compare(Other); }
        NODISCARD FORCEINLINE constexpr bool Equals(view_type Other) const noexcept { return View().Equals(Other); }
        NODISCARD FORCEINLINE constexpr bool StartsWith(view_type Prefix) const noexcept { return View().StartsWith(Prefix); }
        NODISCARD FORCEINLINE constexpr bool EndsWith(view_type Suffix) const noexcept { return View().EndsWith(Suffix); }
        NODISCARD FORCEINLINE constexpr bool Contains(view_type Needle) const noexcept { return View().Contains(Needle); }
        NODISCARD FORCEINLINE constexpr int32 Find(view_type Needle, int32 StartPosition = 0) const noexcept { return View().Find(Needle, StartPosition); }
        NODISCARD FORCEINLINE constexpr int32 Find(TChar Character, int32 StartPosition = 0) const noexcept { return View().Find(Character, StartPosition); }
        NODISCARD FORCEINLINE constexpr int32 FindLast(TChar Character, int32 StartPosition = Constants::kIndexNone) const noexcept { return View().FindLast(Character, StartPosition); }

        NODISCARD friend constexpr bool operator==(TCStringView A, TCStringView B) noexcept { return A.View() == B.View(); }
        NODISCARD friend constexpr std::strong_ordering operator<=>(TCStringView A, TCStringView B) noexcept { return A.View() <=> B.View(); }

    private:

        struct FTrusted {};

        constexpr TCStringView(const TChar* InText, int32 InLen, FTrusted) noexcept
            : Chars(InText)
            , Count(InLen)
        {}

        static constexpr TChar EmptyLiteral[1] = { TChar(0) };

        const TChar* Chars = EmptyLiteral;
        int32        Count = 0;
    };

    // Unsigned like Compare, so case-insensitive and case-sensitive sorting agree on characters past ASCII.
    template <typename TChar>
    NODISCARD constexpr int CompareIgnoreCase(TStringView<TChar> A, TStringView<TChar> B) noexcept
    {
        using FUnsigned = std::make_unsigned_t<TChar>;
        const int32 Shared = A.Len() < B.Len() ? A.Len() : B.Len();
        const TChar* Left = A.GetData();
        const TChar* Right = B.GetData();
        for (int32 Index = 0; Index < Shared; ++Index)
        {
            const FUnsigned FoldedLeft = static_cast<FUnsigned>(FoldAscii(Left[Index]));
            const FUnsigned FoldedRight = static_cast<FUnsigned>(FoldAscii(Right[Index]));
            if (FoldedLeft != FoldedRight)
            {
                return FoldedLeft < FoldedRight ? -1 : 1;
            }
        }
        if (A.Len() == B.Len())
        {
            return 0;
        }
        return A.Len() < B.Len() ? -1 : 1;
    }

    template <typename TChar>
    NODISCARD constexpr bool EqualsIgnoreCase(TStringView<TChar> A, TStringView<TChar> B) noexcept
    {
        return A.Len() == B.Len() && CompareIgnoreCase(A, B) == 0;
    }

    template <typename TChar>
    NODISCARD constexpr bool EndsWithIgnoreCase(TStringView<TChar> View, std::type_identity_t<TStringView<TChar>> Suffix) noexcept
    {
        return View.Len() >= Suffix.Len() && EqualsIgnoreCase(View.Mid(View.Len() - Suffix.Len()), Suffix);
    }

    // Advances Index past one codepoint, returning U+FFFD on bad input and leaving a byte that broke the sequence unread.
    NODISCARD constexpr uint32 DecodeUTF8Codepoint(TStringView<char> Text, int32& Index) noexcept
    {
        constexpr uint32 Replacement = 0xFFFD;
        const uint8 Lead = static_cast<uint8>(Text[Index++]);
        if (Lead < 0x80)
        {
            return Lead;
        }

        int32 Continuations;
        uint32 Codepoint;
        if ((Lead & 0xE0) == 0xC0)      { Continuations = 1; Codepoint = Lead & 0x1F; }
        else if ((Lead & 0xF0) == 0xE0) { Continuations = 2; Codepoint = Lead & 0x0F; }
        else if ((Lead & 0xF8) == 0xF0) { Continuations = 3; Codepoint = Lead & 0x07; }
        else                            { return Replacement; }

        for (int32 i = 0; i < Continuations; ++i, ++Index)
        {
            if (Index >= Text.Len() || (static_cast<uint8>(Text[Index]) & 0xC0) != 0x80)
            {
                return Replacement;
            }
            Codepoint = (Codepoint << 6) | (static_cast<uint8>(Text[Index]) & 0x3F);
        }
        return Codepoint;
    }

    template <typename TChar>
    NODISCARD inline uint64 GetTypeHash(TStringView<TChar> View) noexcept
    {
        return HashBytes(View.GetData(), static_cast<size_t>(View.Len()) * sizeof(TChar));
    }

    template <typename TChar>
    NODISCARD inline uint64 GetTypeHash(TCStringView<TChar> View) noexcept
    {
        return HashBytes(View.GetData(), static_cast<size_t>(View.Len()) * sizeof(TChar));
    }
}

namespace Lumina
{
    using FStringView  = Containers::TStringView<char>;
    using FCStringView = Containers::TCStringView<char>;
    using Containers::DecodeUTF8Codepoint;
}

// Views borrow, so a ranges algorithm on a temporary one returns a usable iterator rather than a dangling marker.
namespace std::ranges
{
    template <typename TChar>
    inline constexpr bool enable_borrowed_range<Lumina::Containers::TStringView<TChar>> = true;

    template <typename TChar>
    inline constexpr bool enable_borrowed_range<Lumina::Containers::TCStringView<TChar>> = true;
}
