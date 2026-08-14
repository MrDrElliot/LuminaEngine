#pragma once
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Declared, not defined: the conversions below only cast to and from the underlying type, which an
    // opaque enum declaration fully supports. Keeps this header free of the MaterialFunction asset
    // include, so a node needing the shared type helpers doesn't drag in (or include another node's
    // header for) that dependency. The cost is MaterialValueOrdinal below; see the note there.
    enum class EMaterialValueType : uint8;

    enum class EMaterialInputType : uint8
    {
        Float,
        Float2,
        Float3,
        Float4,
        Texture,
        // A bindless texture index (uint), not a sampled value. Produced by the TextureHandle node so
        // hand-written Slang can sample the texture itself; every other node is float-typed, which is
        // why this one is a distinct type rather than a float carrying an index.
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

    // EMaterialValueType's ordinals, restated because this header only forward-declares that enum (see
    // above) and an opaque declaration cannot name its enumerators. MaterialCompiler.cpp includes both
    // definitions and static_asserts every one of these, so a reorder there is a build break here.
    namespace MaterialValueOrdinal
    {
        inline constexpr uint8 Float         = 0;
        inline constexpr uint8 Float2        = 1;
        inline constexpr uint8 Float3        = 2;
        inline constexpr uint8 Float4        = 3;
        inline constexpr uint8 TextureHandle = 4;
    }

    // The two enums agree on Float..Float4 but diverge past it: EMaterialInputType has a Texture entry
    // (a sampled float4, drawn with a thumbnail editor) that signature I/O has no use for, so
    // TextureHandle sits at a different ordinal in each and the mapping is spelled out both ways.
    // Texture clamps to Float4 on the way back, since a sampled texture IS a float4 value.
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
        // Bindless slot 0 is a real (and arbitrary) texture, so this is not a "no texture" value -- it is
        // just the neutral uint a node emits after erroring out, so the shader still parses.
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
