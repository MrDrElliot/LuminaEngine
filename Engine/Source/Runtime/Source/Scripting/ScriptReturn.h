#pragma once

#include "Memory/Memcpy.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Scripting
{
    // Keeps a result too big for the managed caller's scratch, so it copies the bytes rather than calling again.
    RUNTIME_API void StashCallOverflow(const void* CallerScratch, const void* Data, size_t Bytes);

    // A string result for the managed caller, copied into its scratch with any excess stashed, returning the full byte length.
    template<typename TString>
    int ReturnStringToCaller(const TString& String, char* Buffer, int Capacity)
    {
        const int Length = static_cast<int>(String.Len());
        if (Buffer != nullptr && Capacity > 0)
        {
            Memory::Memcpy(Buffer, String.GetData(), static_cast<size_t>(Length < Capacity ? Length : Capacity));
        }
        if (Buffer != nullptr && Length > Capacity)
        {
            StashCallOverflow(Buffer, String.GetData(), static_cast<size_t>(Length));
        }
        return Length;
    }

    template<size_t Size>
    struct TScriptReturnBits;

    template<> struct TScriptReturnBits<1> { uint8  Bits; };
    template<> struct TScriptReturnBits<2> { uint16 Bits; };
    template<> struct TScriptReturnBits<4> { uint32 Bits; };
    template<> struct TScriptReturnBits<8> { uint64 Bits; };

#if defined(LE_PLATFORM_WINDOWS)
    // MSVC hands back a struct with a user-declared constructor through a hidden pointer at any size, the CLR uses RAX.
    template<typename T>
    inline constexpr bool bScriptReturnNeedsPacking =
        sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8;
#else
    template<typename T>
    inline constexpr bool bScriptReturnNeedsPacking = false;
#endif

    template<typename T, bool = bScriptReturnNeedsPacking<T>>
    struct TScriptReturn { using Type = T; };

    template<typename T>
    struct TScriptReturn<T, true> { using Type = TScriptReturnBits<sizeof(T)>; };

    template<typename T>
    FORCEINLINE typename TScriptReturn<T>::Type PackScriptReturn(const T& Value)
    {
        if constexpr (bScriptReturnNeedsPacking<T>)
        {
            typename TScriptReturn<T>::Type Packed{};
            Memory::Memcpy(&Packed, &Value, sizeof(T));
            return Packed;
        }
        else
        {
            return Value;
        }
    }
}
