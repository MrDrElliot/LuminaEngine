#pragma once

#include "Containers/Format.h"
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
        FName(const FString& Str) : FName(Str.c_str()) {}
        FName(const FWString& Str) : FName(StringUtils::FromWideString(Str)) {}
        FName(const FFixedString& Str) : FName(Str.c_str()) {}
        FName(const FFixedWString& Str) : FName(StringUtils::FromWideString(Str.c_str())) {}
        FName(FStringView Str) : FName(FString(Str.data(), Str.length()).c_str()) {}

        explicit FName(uint32 InIndex) : Index(InIndex) {}

        bool IsNone() const { return Index == kNoName && Number == kNoNumber; }

        /**
         * This name's identity: a dense index into the name table, unique per distinct string.
         *
         * Comparable and hashable, but NOT stable across runs -- it depends on the order names were first
         * seen. Anything that needs a value stable between sessions wants GetStableHash.
         */
        uint32 GetID() const { return Index; }
        uint32 GetComparisonID() const { return Index; }

        /** Content hash of the base string, identical in every run. For a persisted or displayed derivation
         *  (a per-name colour, an ImGui id) that must not move between sessions. */
        RUNTIME_API uint64 GetStableHash() const;

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

        /**
         * Pointer to a null-terminated rendering of this name including any numeric suffix.
         * Names without a number return a stable pooled pointer; numbered names render into a
         * small thread-local rotating buffer, so treat the result as short-lived (copy it if you
         * need it past a few subsequent c_str() calls on the same thread).
         */
        const char* c_str() const;

        char At(size_t Pos) const;

        FString ToString() const;
        void ToString(FString& Out) const;
        void AppendString(FString& Out) const;

        size_t Length() const;
        size_t length() const { return Length(); }

        auto operator <=> (const FName& Other) const = default;
        bool operator==(const FName& Other) const = default;
        bool operator+(const FName& Other) const = delete;

        size_t Hash() const
        {
            return Number == kNoNumber ? Index : Index ^ (static_cast<uint64>(Number) * 0x9E3779B97F4A7C15ull);
        }

        const char* operator * () const
        {
            return c_str();
        }


    private:

        uint32 Index    = kNoName;
        uint32 Number   = kNoNumber;
    };

    /** The empty name. Constant-initialized (never touches the name table), so it's safe to use
     *  from any static initializer. */
    inline constexpr FName NAME_None{};

    template<>
    struct TCanBulkSerialize<FName> : std::false_type {};

    // Mixed rather than used raw: the table takes its control byte from the low 7 bits.
    NODISCARD FORCEINLINE uint64 GetTypeHash(const FName& Name) noexcept
    {
        return Containers::MixHash64(Name.Hash());
    }
    
}

namespace Lumina
{
    FORCEINLINE void FormatArgument(Fmt::FFormatBuffer& Out, const FName& Name, const Fmt::FFormatSpec& Spec)
    {
        Fmt::WriteAligned(Out, FStringView(Name.c_str()), Spec);
    }
}
