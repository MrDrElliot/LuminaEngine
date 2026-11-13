#pragma once

#include "World/ECS/Entity.h"

#include "Platform/GenericPlatform.h"
#include "Containers/StringView.h"
#include "Core/Math/Scalar.h"
#include "Memory/Memcpy.h"

namespace Lumina { class CWorld; }

// LUMINA_DOTNET_EXPORT stamps the LuminaSharp_ prefix and C linkage a [NativeCall] resolves by name, and silences C4190 for a blittable struct return.
#if defined(_MSC_VER)
    #define LUMINA_DOTNET_EXPORT(ReturnType, Name)  \
        __pragma(warning(suppress: 4190))           \
        extern "C" LUMINA_SCRIPT_API ReturnType LuminaSharp_##Name
#else
    #define LUMINA_DOTNET_EXPORT(ReturnType, Name)  \
        extern "C" LUMINA_SCRIPT_API ReturnType LuminaSharp_##Name
#endif

namespace Lumina::DotNet
{
    // Opaque-handle <-> engine-type conversions shared by every gameplay export.
    FORCEINLINE CWorld*      AsWorld (uint64 Handle)        { return reinterpret_cast<CWorld*>(Handle); }
    FORCEINLINE ECS::FEntity AsEntity(uint32 Entity)        { return static_cast<ECS::FEntity>(Entity); }
    FORCEINLINE uint32       ToId    (ECS::FEntity Entity)  { return static_cast<uint32>(Entity); }

    // The caller sizes its buffer from the length returned here, so a reserved terminator costs a character.
    FORCEINLINE int32 CopyOut(FStringView Text, char* Buf, int Cap)
    {
        const int32 Length = Text.Len();
        if (Buf != nullptr && Cap > 0 && Length > 0)
        {
            Memory::Memcpy(Buf, Text.GetData(), (size_t)Math::Min(Length, Cap));
        }
        return Length;
    }
}
