#pragma once

#include "Format.h"
#include <iterator>
#include <string_view>

#include "BasicString.h"
#include "Core/DisableAllWarnings.h"
#include "Platform/PlatformString.h"

PRAGMA_DISABLE_ALL_WARNINGS
#include <iosfwd>
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


    namespace StringUtils
    {
        inline FWString ToWideString(FStringView str)
        {
            const auto Conv = StringCast<WIDECHAR>(str.GetData(), str.Len());
            return FWString(Conv.Get(), Conv.Length());
        }
        inline FWString ToWideString(const char* pStr)
        {
            const auto Conv = StringCast<WIDECHAR>(pStr);
            return FWString(Conv.Get(), Conv.Length());
        }
        inline FString FromWideString(const FWString& Str)
        {
            const auto Conv = StringCast<ANSICHAR>(*Str, Str.Len());
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
            Fmt::TInlineFormatBuffer<64> Buffer;
            Fmt::WriteFloat(Buffer, Size, Fmt::FFormatSpec{ -1, 2, ' ', 'f' });
            Buffer.AppendChar(' ');
            Buffer.Append(Suffixes[Suffix], Suffix == 0 ? 1 : 2);
            return FString(Buffer.GetData(), Buffer.Len());
        }

    }
}

#define TCHAR_TO_UTF8(X) (::Lumina::StringCast<ANSICHAR>(X).Get())
#define UTF8_TO_TCHAR(X) (::Lumina::StringCast<TCHAR>(X).Get())

namespace Lumina::Containers
{
    // Templated on the traits so the stream stays dependent, which lets this header forward declare it.
    template <typename TTraits, size_t N, typename TAllocator>
    inline std::basic_ostream<char, TTraits>& operator<<(std::basic_ostream<char, TTraits>& Out, const TBasicString<char, N, TAllocator>& Str)
    {
        Out.write(Str.GetData(), static_cast<std::streamsize>(Str.Len()));
        return Out;
    }
}
