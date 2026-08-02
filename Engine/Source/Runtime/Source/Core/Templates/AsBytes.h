#pragma once

#include "Containers/Array.h"

namespace Lumina
{
    enum class Byte : uint8;

    template<typename T>
    TSpan<const Byte> AsBytes(TSpan<T> Type) noexcept
    {
        return TSpan<const Byte>(reinterpret_cast<const Byte*>(Type.data()), Type.size_bytes());
    }
    
    template<typename T>
    requires(!eastl::is_pointer_v<T> && eastl::is_trivially_copyable_v<T>)
    TSpan<const Byte> AsBytes(const T& Value)
    {
        return TSpan(reinterpret_cast<const Byte*>(eastl::addressof(Value)), sizeof(T));
    }
    
}
