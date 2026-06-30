#pragma once

#include "String.h"
#include "Core/DisableAllWarnings.h"
#include "Core/LuminaMacros.h"
#include "Core/Assertions/Assert.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Templates/CanBulkSerialize.h"
#include "Core/Threading/Thread.h"

enum class EName : uint32;
PRAGMA_DISABLE_ALL_WARNINGS
#include "EASTL/hash_map.h"
PRAGMA_ENABLE_ALL_WARNINGS

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    /** Internal number meaning "no numeric suffix". External numbers are stored as +1 of this. */
    static constexpr uint32 NAME_NO_NUMBER = 0;

    class RUNTIME_API FName
    {
    public:

        using value_type = char;

        static void Initialize();

        static void Shutdown();

    public:

        FName() = default;
        FName(EName Name);

        FName(const char* Str);

        /** Construct from an explicit base name and external number, e.g. FName("Entity", 3) -> "Entity_3". */
        FName(const char* Str, uint32 InNumber);

        FName(const TCHAR* Str) : FName(StringUtils::FromWideString(Str)) {}
        FName(const FString& Str) : FName(Str.c_str()) {}
        FName(const FWString& Str) : FName(StringUtils::FromWideString(Str)) {}
        FName(const FFixedString& Str) : FName(Str.c_str()) {}
        FName(const FFixedWString& Str) : FName(Str.c_str()) {}
        FName(FStringView Str) : FName(FString(Str.data(), Str.length()).c_str()) {}

        explicit FName(uint64 InID) : ID(InID) {}

        bool IsNone() const { return ID == 0 && Number == NAME_NO_NUMBER; }

        /** Case-folded hash of the base string, ignoring any numeric suffix (so "A" and "a" share it). */
        uint64 GetID() const { return ID; }
        uint64 GetComparisonID() const { return ID; }

        /** Whether this name carries a numeric suffix (e.g. "Entity_3"). */
        bool HasNumber() const { return Number != NAME_NO_NUMBER; }

        /** External number (the value shown after the underscore); 0 when HasNumber() is false. */
        uint32 GetNumber() const { return Number == NAME_NO_NUMBER ? 0 : Number - 1; }

        /** Same base name with the numeric suffix stripped. */
        FName GetBaseName() const { return FName(ID); }

        operator uint64() const { return ID; }

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

        FName& operator=(const EName InName) { *this = FName(InName); return *this; }

        bool operator==(const FName& Other) const   { return ID == Other.ID && Number == Other.Number; }
        bool operator!=(const FName& Other) const   { return !(*this == Other); }
        bool operator<(const FName& Other) const    { return ID != Other.ID ? ID < Other.ID : Number < Other.Number; }
        bool operator<=(const FName& Other) const   { return !(Other < *this); }
        bool operator>(const FName& Other) const    { return Other < *this; }
        bool operator>=(const FName& Other) const   { return !(*this < Other); }
        bool operator+(const FName& Other) const = delete;

        bool operator==(const EName Name) const { return ID == (uint64)Name && Number == NAME_NO_NUMBER; }
        bool operator!=(const EName Name) const { return !(*this == Name); }

        size_t Hash() const
        {
            return Number == NAME_NO_NUMBER ? ID : ID ^ (static_cast<uint64>(Number) * 0x9E3779B97F4A7C15ull);
        }

        const char* operator * () const
        {
            return c_str();
        }


    private:

        uint64 ID = 0;
        uint32 Number = NAME_NO_NUMBER;
    };
    
    template<>
    struct TCanBulkSerialize<FName> : eastl::false_type {};
    
}

namespace eastl
{
    template <typename T> struct hash;

    template <>
    struct eastl::hash<Lumina::FName>
    {
        size_t operator()(const Lumina::FName& Name) const
        {
            return Name.Hash();
        }
    };
}

template <>
struct std::formatter<Lumina::FName>
{
    constexpr auto parse(format_parse_context& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const Lumina::FName& str, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "{}", std::string_view(str.c_str()));
    }
};
