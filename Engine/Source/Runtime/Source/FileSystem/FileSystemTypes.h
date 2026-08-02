#pragma once
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::FileSystem
{
    enum class EFileMode : uint8
    {
        Read        = BIT(0),
        Write       = BIT(1),
        Append      = BIT(2),
        Truncate    = BIT(3),
        
        ReadWrite   = Read | Write,
    };
}
