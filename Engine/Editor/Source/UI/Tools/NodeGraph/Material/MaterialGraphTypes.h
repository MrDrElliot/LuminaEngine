#pragma once
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Declared, not defined: the conversions below are pure casts, which an opaque enum declaration
    // fully supports. Keeps this header free of the MaterialFunction asset include, so a node needing
    // the shared type helpers doesn't drag in (or include another node's header for) that dependency.
    enum class EMaterialValueType : uint8;

    enum class EMaterialInputType : uint8
    {
        Float,
        Float2,
        Float3,
        Float4,
        Texture,
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

    // EMaterialValueType and EMaterialInputType share Float..Float4 ordering, so conversion is a cast.
    // Texture-typed signature I/O is unsupported, so a Texture input type clamps to Float4 on the way back.
    inline EMaterialInputType ToMaterialInputType(EMaterialValueType Type)
    {
        return static_cast<EMaterialInputType>(Type);
    }

    inline EMaterialValueType ToMaterialValueType(EMaterialInputType Type)
    {
        return static_cast<EMaterialValueType>(Type == EMaterialInputType::Texture ? EMaterialInputType::Float4 : Type);
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