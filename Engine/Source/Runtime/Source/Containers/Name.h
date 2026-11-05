#pragma once

#include "Containers/Format.h"
#include <compare>
#include <string_view>

#include "String.h"
#include "Core/DisableAllWarnings.h"
#include "Core/LuminaMacros.h"
#include "Core/Assertions/Assert.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Templates/CanBulkSerialize.h"
#include "Core/Threading/Thread.h"

PRAGMA_DISABLE_ALL_WARNINGS
PRAGMA_ENABLE_ALL_WARNINGS

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class RUNTIME_API FName
    {
    public:

        /** Internal number meaning "no numeric suffix". External numbers are stored as +1 of this. */
        static constexpr uint32 kNoNumber   = 0;
        /** Index 0 is the empty name, which is why a default-constructed FName never touches the table. */
        static constexpr uint32 kNoName     = 0;

        using value_type = char;

        constexpr FName() = default;

        FName(const char* Str);

        /** Construct from an explicit base name and external number, e.g. FName("Entity", 3) -> "Entity_3". */
        FName(const char* Str, uint32 InNumber);

#if PLATFORM_TCHAR_IS_WIDE
        FName(const TCHAR* Str) : FName(StringUtils::FromWideString(Str)) {}
#endif
        FName(const FString& Str) : FName(Str.View()) {}
        FName(const FWString& Str) : FName(StringUtils::FromWideString(Str)) {}
        FName(const FFixedString& Str) : FName(Str.View()) {}
        FName(const FFixedWString& Str) : FName(StringUtils::FromWideString(*Str)) {}
        // Interned straight from the view, since a temporary FString allocates for any name past the inline capacity.
        FName(FStringView Str);

        explicit FName(uint32 InIndex) : Index(InIndex) {}

        bool IsNone() const { return Index == kNoName && Number == kNoNumber; }

        // A dense table index: comparable and hashable but not stable between runs, which GetStableHash is.
        uint32 GetID() const { return Index; }
        uint32 GetComparisonID() const { return Index; }

        /** Content hash of the base string, identical in every run, for anything derived from a name that must not move between sessions. */
        uint64 GetStableHash() const;

        /** Whether this name carries a numeric suffix (e.g. "Entity_3"). */
        bool HasNumber() const { return Number != kNoNumber; }

        /** External number (the value shown after the underscore); 0 when HasNumber() is false. */
        uint32 GetNumber() const { return Number == kNoNumber ? 0 : Number - 1; }

        /** Same base name with the numeric suffix stripped. */
        FName GetBaseName() const { return FName(Index); }

        // rebuilds a numbered name from parts stored separately, only valid where HasNumber() was true
        static FName FromIndexAndNumber(uint32 InIndex, uint32 InExternalNumber)
        {
            FName Result(InIndex);
            Result.Number = InExternalNumber + 1;
            return Result;
        }

        explicit operator uint32() const { return Index; }

        // A numbered name renders into a small thread-local rotating buffer, so copy the result if it must outlive a few more calls.
        const char* operator*() const;

        char At(int32 Pos) const;

        FString ToString() const;
        void ToString(FString& Out) const;
        void AppendString(FString& Out) const;

        int32 Len() const;

        // Spelled out because a defaulted comparison on an exported class is called through the DLL import table instead of inlined.
        bool operator==(const FName& Other) const { return ((Index ^ Other.Index) | (Number ^ Other.Number)) == 0; }

        std::strong_ordering operator<=>(const FName& Other) const
        {
            if (const std::strong_ordering Order = Index <=> Other.Index; Order != 0)
            {
                return Order;
            }
            return Number <=> Other.Number;
        }
        bool operator+(const FName& Other) const = delete;

        size_t Hash() const
        {
            return Number == kNoNumber ? Index : Index ^ (static_cast<uint64>(Number) * 0x9E3779B97F4A7C15ull);
        }


    private:

        uint32 Index    = kNoName;
        uint32 Number   = kNoNumber;
    };

    /** The empty name, constant-initialized and never touching the table, so any static initializer may use it. */
    inline constexpr FName NAME_None{};

    template<>
    struct TCanBulkSerialize<FName> : std::false_type {};

    // Mixed rather than used raw: the table takes its control byte from the low 7 bits.
    NODISCARD FORCEINLINE uint64 GetTypeHash(const FName& Name) noexcept
    {
        return Containers::HashScalar(Name.Hash());
    }
    
}

namespace Lumina
{
    FORCEINLINE void FormatArgument(Fmt::FFormatBuffer& Out, const FName& Name, const Fmt::FFormatSpec& Spec)
    {
        Fmt::WriteAligned(Out, FStringView(*Name), Spec);
    }
}
