#pragma once

#include "Lumina.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Shared/SharedConstants.h"
#include "meshoptimizer.h"

namespace Lumina
{
    constexpr int32 MeshletPositionBits = 16;
    static_assert((1 << MeshletPositionBits) - 1 == MESHLET_POSITION_MAX,
                  "MeshletPositionBits must describe the width of FMeshletVertex's position fields");

    struct FMeshletQuantization
    {
        int32 AnchorX  = 0;
        int32 AnchorY  = 0;
        int32 AnchorZ  = 0;
        int32 Exponent = 0;
    };

    FORCEINLINE float MeshletExponentScale(int32 Exponent)
    {
        union { uint32 U; float F; } Bits;
        Bits.U = (uint32)(Math::Clamp(Exponent, -126, 127) + 127) << 23;
        return Bits.F;
    }

    FORCEINLINE int32 UnpackMeshletAnchor(uint32 Packed)
    {
        // Sign-extend from 24 bits.
        const uint32 Field = Packed & MESHLET_ANCHOR_MASK;
        return (int32)(Field ^ MESHLET_ANCHOR_SIGN) - (int32)MESHLET_ANCHOR_SIGN;
    }

    FORCEINLINE int32 UnpackMeshletExponent(uint32 PackedAnchorX)
    {
        return (int32)(int8)(uint8)(PackedAnchorX >> MESHLET_EXPONENT_SHIFT);
    }

    FORCEINLINE FVector3 DecodeMeshletPosition(const FMeshlet& M, const FMeshletVertex& V)
    {
        const float Scale = MeshletExponentScale(UnpackMeshletExponent(M.PackedAnchorX));

        const int32 Ox = (int32)V.PositionX;
        const int32 Oy = (int32)V.PositionY;
        const int32 Oz = (int32)V.PositionZ;

        return FVector3(
            (float)(UnpackMeshletAnchor(M.PackedAnchorX) + Ox) * Scale,
            (float)(UnpackMeshletAnchor(M.PackedAnchorY) + Oy) * Scale,
            (float)(UnpackMeshletAnchor(M.PackedAnchorZ) + Oz) * Scale);
    }

    inline FMeshletQuantization ComputeMeshletQuantization(const FVector3* Positions, uint32 Count)
    {
        FMeshletQuantization Q;
        if (Positions == nullptr || Count == 0)
        {
            return Q;
        }

        FVector3 Min = Positions[0];
        FVector3 Max = Positions[0];
        for (uint32 i = 1; i < Count; ++i)
        {
            Min.x = Math::Min(Min.x, Positions[i].x);
            Min.y = Math::Min(Min.y, Positions[i].y);
            Min.z = Math::Min(Min.z, Positions[i].z);
            Max.x = Math::Max(Max.x, Positions[i].x);
            Max.y = Math::Max(Max.y, Positions[i].y);
            Max.z = Math::Max(Max.z, Positions[i].z);
        }

        // Solves both constraints the packing has: the offset fits max_bits, the anchor fits a signed 24-bit grid.
        const int32 Exponent = meshopt_computePositionExponent(&Min.x, &Max.x, -126, MeshletPositionBits);

        const float Scale = MeshletExponentScale(Exponent);
        Q.Exponent = Exponent;
        Q.AnchorX  = (int32)Math::Floor(Min.x / Scale);
        Q.AnchorY  = (int32)Math::Floor(Min.y / Scale);
        Q.AnchorZ  = (int32)Math::Floor(Min.z / Scale);
        return Q;
    }

    /** Writes the quantization frame into the meshlet its encoded vertices will be decoded against. */
    FORCEINLINE void ApplyMeshletQuantization(FMeshlet& M, const FMeshletQuantization& Q)
    {
        M.PackedAnchorX = ((uint32)Q.AnchorX & MESHLET_ANCHOR_MASK)
                        | ((uint32)(uint8)(int8)Q.Exponent << MESHLET_EXPONENT_SHIFT);
        M.PackedAnchorY = (uint32)Q.AnchorY & MESHLET_ANCHOR_MASK;
        M.PackedAnchorZ = (uint32)Q.AnchorZ & MESHLET_ANCHOR_MASK;
    }

    /** Encodes one position into the vertex's two packed dwords; leaves its other members alone. */
    FORCEINLINE void EncodeMeshletPosition(const FMeshletQuantization& Q, const FVector3& P, FMeshletVertex& Out)
    {
        const float Scale = MeshletExponentScale(Q.Exponent);

        auto Component = [&](float Value, int32 Anchor) -> uint32
        {
            const int64 Offset = (int64)Math::Floor(Value / Scale + 0.5f) - (int64)Anchor;
            return (uint32)Math::Clamp<int64>(Offset, (int64)0, (int64)MESHLET_POSITION_MAX);
        };

        Out.PositionX = (uint16)Component(P.x, Q.AnchorX);
        Out.PositionY = (uint16)Component(P.y, Q.AnchorY);
        Out.PositionZ = (uint16)Component(P.z, Q.AnchorZ);
    }
}
