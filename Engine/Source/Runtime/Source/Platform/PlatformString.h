#pragma once
#include <cstring>
#include <cwchar>
#include <EASTL/type_traits.h>
#include "GenericPlatform.h"
#include "Platform.h"

// Forward-declared rather than including Memory.h: this header is pulled in by Containers/String.h, and
// Memory.h transitively re-includes String.h (via Assert.h), which the reflector rejects as a cycle.
namespace Lumina::Memory
{
    RUNTIME_API NODISCARD void* Malloc(size_t Size, size_t Alignment);
    RUNTIME_API void Free(void*& Memory);
}


// Stack-friendly string conversion.
namespace Lumina::Platform
{
    // UTF-8 (narrow) -> UTF-16/UTF-32 (wide). Lengths are in code units, excluding any null terminator.
    RUNTIME_API int32 GetConvertedLength_UTF8ToWide(const ANSICHAR* Src, int32 SrcLen);
    RUNTIME_API int32 Convert_UTF8ToWide(WIDECHAR* Dest, int32 DestLen, const ANSICHAR* Src, int32 SrcLen);

    // Wide -> UTF-8 (narrow).
    RUNTIME_API int32 GetConvertedLength_WideToUTF8(const WIDECHAR* Src, int32 SrcLen);
    RUNTIME_API int32 Convert_WideToUTF8(ANSICHAR* Dest, int32 DestLen, const WIDECHAR* Src, int32 SrcLen);
}

namespace Lumina
{
    namespace StringCastPrivate
    {
        FORCEINLINE int32 CStringLen(const ANSICHAR* Str) { return static_cast<int32>(::strlen(Str)); }
        FORCEINLINE int32 CStringLen(const WIDECHAR* Str) { return static_cast<int32>(::wcslen(Str)); }

        // Conversion trait keyed on (To, From). Only the type pairs the engine actually uses are defined;
        // anything else is a compile error rather than a silent wrong-encoding copy.
        template <typename To, typename From>
        struct TStringConvert;

        // Same char type: straight copy, no re-encoding.
        template <typename T>
        struct TStringConvert<T, T>
        {
            static int32 ConvertedLength(const T*, int32 SrcLen) { return SrcLen; }
            static int32 Convert(T* Dest, int32 DestLen, const T* Src, int32 SrcLen)
            {
                const int32 Count = SrcLen < DestLen ? SrcLen : DestLen;
                ::memcpy(Dest, Src, static_cast<size_t>(Count) * sizeof(T));
                return Count;
            }
        };

        // UTF-8 -> wide.
        template <>
        struct TStringConvert<WIDECHAR, ANSICHAR>
        {
            static int32 ConvertedLength(const ANSICHAR* Src, int32 SrcLen) { return Platform::GetConvertedLength_UTF8ToWide(Src, SrcLen); }
            static int32 Convert(WIDECHAR* Dest, int32 DestLen, const ANSICHAR* Src, int32 SrcLen) { return Platform::Convert_UTF8ToWide(Dest, DestLen, Src, SrcLen); }
        };

        // Wide -> UTF-8.
        template <>
        struct TStringConvert<ANSICHAR, WIDECHAR>
        {
            static int32 ConvertedLength(const WIDECHAR* Src, int32 SrcLen) { return Platform::GetConvertedLength_WideToUTF8(Src, SrcLen); }
            static int32 Convert(ANSICHAR* Dest, int32 DestLen, const WIDECHAR* Src, int32 SrcLen) { return Platform::Convert_WideToUTF8(Dest, DestLen, Src, SrcLen); }
        };
    }

    template <typename ToType, typename FromType, int32 InlineSize = 128>
    class TStringConversion
    {
    public:

        explicit TStringConversion(const FromType* Src)
        {
            Init(Src, Src ? StringCastPrivate::CStringLen(Src) : 0);
        }

        TStringConversion(const FromType* Src, int32 SrcLen)
        {
            Init(Src, SrcLen);
        }

        ~TStringConversion()
        {
            if (Ptr && Ptr != InlineBuffer)
            {
                void* Mem = Ptr;
                Memory::Free(Mem);
            }
        }

        // RVO-only: copies and moves would dangle the inline buffer pointer.
        TStringConversion(const TStringConversion&) = delete;
        TStringConversion(TStringConversion&&) = delete;
        TStringConversion& operator=(const TStringConversion&) = delete;
        TStringConversion& operator=(TStringConversion&&) = delete;

        NODISCARD const ToType* Get() const       { return Ptr; }
        NODISCARD operator const ToType*() const  { return Ptr; }
        NODISCARD int32 Length() const            { return ConvertedLength; }
        NODISCARD bool IsEmpty() const            { return ConvertedLength == 0; }

    private:

        void Init(const FromType* Src, int32 SrcLen)
        {
            using Convert = StringCastPrivate::TStringConvert<ToType, FromType>;

            if (!Src || SrcLen <= 0)
            {
                InlineBuffer[0] = ToType(0);
                Ptr = InlineBuffer;
                ConvertedLength = 0;
                return;
            }

            const int32 NeededLen = Convert::ConvertedLength(Src, SrcLen);

            if (NeededLen + 1 <= InlineSize)
            {
                Ptr = InlineBuffer;
            }
            else
            {
                Ptr = static_cast<ToType*>(Memory::Malloc((static_cast<size_t>(NeededLen) + 1) * sizeof(ToType), alignof(ToType)));
            }

            const int32 Written = Convert::Convert(Ptr, NeededLen, Src, SrcLen);
            Ptr[Written] = ToType(0);
            ConvertedLength = Written;
        }

        ToType  InlineBuffer[InlineSize];
        ToType* Ptr = nullptr;
        int32   ConvertedLength = 0;
    };

    // StringCast<To>(Src), To is explicit, From is deduced.
    template <typename ToType, typename FromType>
    NODISCARD FORCEINLINE TStringConversion<ToType, FromType> StringCast(const FromType* Src)
    {
        return TStringConversion<ToType, FromType>(Src);
    }

    template <typename ToType, typename FromType>
    NODISCARD FORCEINLINE TStringConversion<ToType, FromType> StringCast(const FromType* Src, int32 Len)
    {
        return TStringConversion<ToType, FromType>(Src, Len);
    }
}
