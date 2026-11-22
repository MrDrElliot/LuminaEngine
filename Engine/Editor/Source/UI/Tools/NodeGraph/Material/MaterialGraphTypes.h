#pragma once
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "MaterialGraphTypes.generated.h"

namespace Lumina
{
    // Lives here rather than on the node so the compiler can name it without a circular include.
    REFLECT()
    enum class EMaterialCoordinateSpace : uint8
    {
        World,
        Local,
        View,
    };

    // Opaque declaration keeps this header free of the MaterialFunction asset include.
    enum class EMaterialValueType : uint8;

    enum class EMaterialInputType : uint8
    {
        Float,
        Float2,
        Float3,
        Float4,
        Texture,
        // A bindless texture index (uint) so hand-written Slang can sample it, distinct from the float-typed rest.
        TextureHandle,
    };

    enum class EComponentMask : uint8
    {
        None,
        RGBA,
        R,
        G,
        B,
        A,
        RG,
        GB,
        RGB,
    };

    // Restated ordinals of the forward-declared enum, which MaterialCompiler.cpp static_asserts so a reorder breaks the build.
    namespace MaterialValueOrdinal
    {
        inline constexpr uint8 Float         = 0;
        inline constexpr uint8 Float2        = 1;
        inline constexpr uint8 Float3        = 2;
        inline constexpr uint8 Float4        = 3;
        inline constexpr uint8 TextureHandle = 4;
    }

    // The enums diverge past Float4 because signature I/O has no Texture entry, which clamps to Float4 on the way back.
    inline EMaterialInputType ToMaterialInputType(EMaterialValueType Type)
    {
        switch (static_cast<uint8>(Type))
        {
        case MaterialValueOrdinal::Float2:        return EMaterialInputType::Float2;
        case MaterialValueOrdinal::Float3:        return EMaterialInputType::Float3;
        case MaterialValueOrdinal::Float4:        return EMaterialInputType::Float4;
        case MaterialValueOrdinal::TextureHandle: return EMaterialInputType::TextureHandle;
        default:                                  return EMaterialInputType::Float;
        }
    }

    inline EMaterialValueType ToMaterialValueType(EMaterialInputType Type)
    {
        switch (Type)
        {
        case EMaterialInputType::Float2:        return static_cast<EMaterialValueType>(MaterialValueOrdinal::Float2);
        case EMaterialInputType::Float3:        return static_cast<EMaterialValueType>(MaterialValueOrdinal::Float3);
        case EMaterialInputType::Float4:
        case EMaterialInputType::Texture:       return static_cast<EMaterialValueType>(MaterialValueOrdinal::Float4);
        case EMaterialInputType::TextureHandle: return static_cast<EMaterialValueType>(MaterialValueOrdinal::TextureHandle);
        default:                                return static_cast<EMaterialValueType>(MaterialValueOrdinal::Float);
        }
    }

    // The mask covering every component of a type ("all of a float3" = RGB).
    inline EComponentMask FullMaskForType(EMaterialInputType Type)
    {
        switch (Type)
        {
        case EMaterialInputType::Float:   return EComponentMask::R;
        case EMaterialInputType::Float2:  return EComponentMask::RG;
        case EMaterialInputType::Float3:  return EComponentMask::RGB;
        case EMaterialInputType::Float4:
        case EMaterialInputType::Texture: return EComponentMask::RGBA;
        // No mask at all, so nothing ever appends a swizzle to a scalar uint.
        case EMaterialInputType::TextureHandle: return EComponentMask::None;
        default:                          return EComponentMask::R;
        }
    }

    // Zero literal of the matching width; the neutral value nodes emit for unconnected or errored slots.
    inline FString ZeroLiteral(EMaterialInputType Type)
    {
        switch (Type)
        {
        case EMaterialInputType::Float:   return "0.0";
        case EMaterialInputType::Float2:  return "float2(0.0, 0.0)";
        case EMaterialInputType::Float3:  return "float3(0.0, 0.0, 0.0)";
        case EMaterialInputType::Float4:
        case EMaterialInputType::Texture: return "float4(0.0, 0.0, 0.0, 0.0)";
        // Slot 0 is a real texture, so this is only the neutral uint a node emits after erroring so the shader parses.
        case EMaterialInputType::TextureHandle: return "0u";
        default:                          return "0.0";
        }
    }

    inline FString GetSwizzleForMask(EComponentMask Mask)
    {
        switch (Mask)
        {
        case EComponentMask::None: return "";
        case EComponentMask::R:    return ".r";
        case EComponentMask::G:    return ".g";
        case EComponentMask::B:    return ".b";
        case EComponentMask::A:    return ".a";
        case EComponentMask::RG:   return ".rg";
        case EComponentMask::GB:   return ".gb";
        case EComponentMask::RGB:  return ".rgb";
        case EComponentMask::RGBA: return ""; // no swizzle needed
        default:                   return "";
        }
    }

    inline FString FixupComponentSwizzle(EComponentMask Mask, const FString& Node)
    {
        switch (Mask)
        {
        case EComponentMask::R:
            return "vec4(" + Node + ".r, " + Node + ".r, " + Node + ".r, " + Node + ".r)";
        case EComponentMask::G:
            return "vec4(" + Node + ".g, " + Node + ".g, " + Node + ".g, " + Node + ".g)";
        case EComponentMask::B:
            return "vec4(" + Node + ".b, " + Node + ".b, " + Node + ".b, " + Node + ".b)";
        case EComponentMask::A:
            return "vec4(" + Node + ".a, " + Node + ".a, " + Node + ".a, " + Node + ".a)";
        case EComponentMask::RG:
            return "vec4(" + Node + ".r, " + Node + ".g, 0.0, " + Node + ".a)";
        case EComponentMask::GB:
            return "vec4(0.0, " + Node + ".g, " + Node + ".b, " + Node + ".a)";
        case EComponentMask::RGB:
            return "vec4(" + Node + ".r, " + Node + ".g, " + Node + ".b, " + Node + ".a)";
        case EComponentMask::RGBA:
            return Node; // Already full vec4, no fixup needed
        default:
            return Node;
        }
    }
}   
