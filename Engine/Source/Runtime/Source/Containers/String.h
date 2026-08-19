#pragma once

#include <format>
#include <iterator>
#include <string_view>

#include "BasicString.h"
#include "Core/DisableAllWarnings.h"
#include "Platform/PlatformString.h"

PRAGMA_DISABLE_ALL_WARNINGS
#include <ostream>
PRAGMA_ENABLE_ALL_WARNINGS


namespace Lumina
{
    using FString                                   = Containers::TBasicString<char>;
    using FStringView                               = Containers::TStringView<char>;
    using FCStringView                              = Containers::TCStringView<char>;
    using FFixedString                              = Containers::TBasicString<char, 255>;

    using FPathString                               = Containers::TBasicString<char, 512>;
    template<size_t S> using TFixedString           = Containers::TBasicString<char, S>;

    using Containers::CompareIgnoreCase;
    using Containers::EqualsIgnoreCase;

    using FWString                                  = Containers::TBasicString<wchar_t>;
    using FFixedWString                             = Containers::TBasicString<wchar_t, 255>;
    using FWStringView                              = Containers::TStringView<wchar_t>;
    using FCWStringView                             = Containers::TCStringView<wchar_t>;

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
        inline FString FromWideString(const WIDECHAR* Str)
        {
            const auto Conv = StringCast<ANSICHAR>(Str);
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

#define TCHAR_TO_UTF8(X) (::Lumina::StringCast<ANSICHAR>(X).Get())
#define UTF8_TO_TCHAR(X) (::Lumina::StringCast<TCHAR>(X).Get())

namespace Lumina::Containers
{
    template <size_t N, typename TAllocator>
    inline std::ostream& operator<<(std::ostream& Out, const TBasicString<char, N, TAllocator>& Str)
    {
        Out.write(Str.data(), static_cast<std::streamsize>(Str.size()));
        return Out;
    }
}
