#include "RuntimePCH.h"
#include "Platform/PlatformString.h"

#ifdef LE_PLATFORM_WINDOWS

#include <windows.h>

namespace Lumina::Platform
{
    int32 GetConvertedLength_UTF8ToWide(const ANSICHAR* Src, int32 SrcLen)
    {
        if (SrcLen <= 0)
        {
            return 0;
        }
        return MultiByteToWideChar(CP_UTF8, 0, Src, SrcLen, nullptr, 0);
    }

    int32 Convert_UTF8ToWide(WIDECHAR* Dest, int32 DestLen, const ANSICHAR* Src, int32 SrcLen)
    {
        if (SrcLen <= 0 || DestLen <= 0)
        {
            return 0;
        }
        return MultiByteToWideChar(CP_UTF8, 0, Src, SrcLen, reinterpret_cast<LPWSTR>(Dest), DestLen);
    }

    int32 GetConvertedLength_WideToUTF8(const WIDECHAR* Src, int32 SrcLen)
    {
        if (SrcLen <= 0)
        {
            return 0;
        }
        return WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(Src), SrcLen, nullptr, 0, nullptr, nullptr);
    }

    int32 Convert_WideToUTF8(ANSICHAR* Dest, int32 DestLen, const WIDECHAR* Src, int32 SrcLen)
    {
        if (SrcLen <= 0 || DestLen <= 0)
        {
            return 0;
        }
        return WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(Src), SrcLen, Dest, DestLen, nullptr, nullptr);
    }
}

#else // A portable hand-rolled fallback, since wchar_t width is platform dependent.

#include "Containers/StringView.h"

namespace Lumina::Platform
{
    namespace
    {
        // Number of UTF-8 bytes needed to encode a codepoint.
        int32 EncodedUTF8Size(uint32 CP)
        {
            if (CP < 0x80)    return 1;
            if (CP < 0x800)   return 2;
            if (CP < 0x10000) return 3;
            return 4;
        }

        int32 EncodeUTF8(uint32 CP, ANSICHAR* Dest, int32 DestLen, int32& Out)
        {
            const int32 Size = EncodedUTF8Size(CP);
            if (Out + Size > DestLen)
            {
                return 0;
            }
            switch (Size)
            {
            case 1:
                Dest[Out++] = static_cast<ANSICHAR>(CP);
                break;
            case 2:
                Dest[Out++] = static_cast<ANSICHAR>(0xC0 | (CP >> 6));
                Dest[Out++] = static_cast<ANSICHAR>(0x80 | (CP & 0x3F));
                break;
            case 3:
                Dest[Out++] = static_cast<ANSICHAR>(0xE0 | (CP >> 12));
                Dest[Out++] = static_cast<ANSICHAR>(0x80 | ((CP >> 6) & 0x3F));
                Dest[Out++] = static_cast<ANSICHAR>(0x80 | (CP & 0x3F));
                break;
            default:
                Dest[Out++] = static_cast<ANSICHAR>(0xF0 | (CP >> 18));
                Dest[Out++] = static_cast<ANSICHAR>(0x80 | ((CP >> 12) & 0x3F));
                Dest[Out++] = static_cast<ANSICHAR>(0x80 | ((CP >> 6) & 0x3F));
                Dest[Out++] = static_cast<ANSICHAR>(0x80 | (CP & 0x3F));
                break;
            }
            return Size;
        }

        // Wide code units needed for a codepoint (2 for non-BMP when WIDECHAR is 16-bit).
        int32 WideUnitsForCodepoint(uint32 CP)
        {
            if (sizeof(WIDECHAR) >= 4)
            {
                return 1;
            }
            return CP >= 0x10000 ? 2 : 1;
        }
    }

    int32 GetConvertedLength_UTF8ToWide(const ANSICHAR* Src, int32 SrcLen)
    {
        const FStringView Text(Src, SrcLen);
        int32 Count = 0;
        for (int32 i = 0; i < SrcLen; )
        {
            Count += WideUnitsForCodepoint(DecodeUTF8Codepoint(Text, i));
        }
        return Count;
    }

    int32 Convert_UTF8ToWide(WIDECHAR* Dest, int32 DestLen, const ANSICHAR* Src, int32 SrcLen)
    {
        const FStringView Text(Src, SrcLen);
        int32 Out = 0;
        for (int32 i = 0; i < SrcLen && Out < DestLen; )
        {
            const uint32 CP = DecodeUTF8Codepoint(Text, i);
            if (sizeof(WIDECHAR) >= 4 || CP < 0x10000)
            {
                Dest[Out++] = static_cast<WIDECHAR>(CP);
            }
            else
            {
                if (Out + 2 > DestLen)
                {
                    break;
                }
                const uint32 V = CP - 0x10000;
                Dest[Out++] = static_cast<WIDECHAR>(0xD800 + (V >> 10));
                Dest[Out++] = static_cast<WIDECHAR>(0xDC00 + (V & 0x3FF));
            }
        }
        return Out;
    }

    int32 GetConvertedLength_WideToUTF8(const WIDECHAR* Src, int32 SrcLen)
    {
        int32 Count = 0;
        for (int32 i = 0; i < SrcLen; ++i)
        {
            uint32 CP = static_cast<typename std::make_unsigned<WIDECHAR>::type>(Src[i]);
            if (sizeof(WIDECHAR) < 4 && CP >= 0xD800 && CP <= 0xDBFF && i + 1 < SrcLen)
            {
                const uint32 Low = static_cast<uint32>(static_cast<uint16>(Src[i + 1]));
                if (Low >= 0xDC00 && Low <= 0xDFFF)
                {
                    CP = 0x10000 + ((CP - 0xD800) << 10) + (Low - 0xDC00);
                    ++i;
                }
            }
            Count += EncodedUTF8Size(CP);
        }
        return Count;
    }

    int32 Convert_WideToUTF8(ANSICHAR* Dest, int32 DestLen, const WIDECHAR* Src, int32 SrcLen)
    {
        int32 Out = 0;
        for (int32 i = 0; i < SrcLen; ++i)
        {
            uint32 CP = static_cast<typename std::make_unsigned<WIDECHAR>::type>(Src[i]);
            if (sizeof(WIDECHAR) < 4 && CP >= 0xD800 && CP <= 0xDBFF && i + 1 < SrcLen)
            {
                const uint32 Low = static_cast<uint32>(static_cast<uint16>(Src[i + 1]));
                if (Low >= 0xDC00 && Low <= 0xDFFF)
                {
                    CP = 0x10000 + ((CP - 0xD800) << 10) + (Low - 0xDC00);
                    ++i;
                }
            }
            if (EncodeUTF8(CP, Dest, DestLen, Out) == 0)
            {
                break;
            }
        }
        return Out;
    }
}

#endif
