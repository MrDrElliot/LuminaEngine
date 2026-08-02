#pragma once
#include "Core/DisableAllWarnings.h"
#include "Platform/PlatformString.h"

PRAGMA_DISABLE_ALL_WARNINGS
#include "EASTL/fixed_string.h"
#include "EASTL/string.h"
#include <ostream>
PRAGMA_ENABLE_ALL_WARNINGS


namespace Lumina
{
    using FString                                   = eastl::basic_string<char>;
    using FStringView                               = eastl::string_view;
    using FFixedString                              = eastl::fixed_string<char, 255>;
    template<eastl_size_t S> using TFixedString     = eastl::fixed_string<char, S>;
    
    using FWString                                  = eastl::basic_string<wchar_t>;
    using FFixedWString                             = eastl::fixed_string<wchar_t, 255>;

    template<typename T>
    concept StringLike = requires(T s)
    {
        { s.length() } -> std::convertible_to<size_t>;
        { s.data() }   -> std::convertible_to<const char*>;
    };
    
    namespace StringUtils
    {
        inline FWString ToWideString(FStringView str)
        {
            const auto Conv = StringCast<WIDECHAR>(str.data(), static_cast<int32>(str.size()));
            return FWString(Conv.Get(), Conv.Length());
        }
        inline FWString ToWideString(const char* pStr)
        {
            const auto Conv = StringCast<WIDECHAR>(pStr);
            return FWString(Conv.Get(), Conv.Length());
        }
        inline FString FromWideString(const FWString& Str)
        {
            const auto Conv = StringCast<ANSICHAR>(Str.c_str(), static_cast<int32>(Str.size()));
            return FString(Conv.Get(), Conv.Length());
        }
        
        inline FString FormatSize(size_t Bytes)
        {
            const char* Suffixes[] = { "B", "KB", "MB", "GB" };
            double Size = static_cast<double>(Bytes);
            int Suffix = 0;

            while (Size >= 1024.0 && Suffix < 3)
            {
                Size /= 1024.0;
                ++Suffix;
            }
            FString Value;
            std::format_to(std::back_inserter(Value), "{:.2f} {}", Size, Suffixes[Suffix]);
            return Value;
        }

    }
}

// Backed by StringCast: the temporary conversion lives to the end of the full expression, same as the
// owning-string version it replaced, but uses an inline buffer (no heap for short strings) and the
// platform code-unit conversion. Prefer Lumina::StringCast<> directly in new code.
#define TCHAR_TO_UTF8(X) (::Lumina::StringCast<ANSICHAR>(X).Get())
#define UTF8_TO_TCHAR(X) (::Lumina::StringCast<WIDECHAR>(X).Get())

namespace std
{
    template <>
    struct formatter<Lumina::FString>
    {
        constexpr auto parse(std::format_parse_context& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const Lumina::FString& str, FormatContext& ctx) const
        {
            return std::format_to(ctx.out(), "{}", str.c_str());
        }
    };
    
    template <>
    struct formatter<Lumina::FFixedString>
    {
        constexpr auto parse(std::format_parse_context& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const Lumina::FFixedString& str, FormatContext& ctx) const
        {
            return std::format_to(ctx.out(), "{}", str.c_str());
        }
    };

    template <>
    struct formatter<eastl::string_view>
    {
        constexpr auto parse(format_parse_context& ctx)
        {
            return ctx.begin();
        }
        
        template <typename FormatContext>
        auto format(const eastl::string_view& str, FormatContext& ctx) const
        {
            return std::format_to(ctx.out(), "{}", std::string_view(str.data(), str.length()));
        }
    };
}


template <eastl_size_t S>
struct eastl::hash<eastl::fixed_string<char, S, true>>
{
    size_t operator()(const eastl::fixed_string<char, S, true>& str) const noexcept
    {
        return eastl::hash<eastl::string_view>{}(eastl::string_view(str.c_str(), str.length()));
    }
};

namespace eastl
{
    inline std::ostream& operator<<(std::ostream& os, const Lumina::FString& str)
    {
        os.write(str.c_str(), str.size());
        return os;
    }
}