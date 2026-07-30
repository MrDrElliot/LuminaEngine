#pragma once

#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    inline uint32 PackColor(FVector4 color)
    {
        uint8 r = (uint8)(Math::Clamp(color.r, 0.0f, 1.0f) * 255.0f);
        uint8 g = (uint8)(Math::Clamp(color.g, 0.0f, 1.0f) * 255.0f);
        uint8 b = (uint8)(Math::Clamp(color.b, 0.0f, 1.0f) * 255.0f);
        uint8 a = (uint8)(Math::Clamp(color.a, 0.0f, 1.0f) * 255.0f);
        return (a << 24) | (b << 16) | (g << 8) | r;
    }

    inline FVector4 UnpackColor(uint32 packed)
    {
        uint8 r = (packed >> 0) & 0xFF;
        uint8 g = (packed >> 8) & 0xFF;
        uint8 b = (packed >> 16) & 0xFF;
        uint8 a = (packed >> 24) & 0xFF;

        return FVector4(
            (float)r / 255.0f,
            (float)g / 255.0f,
            (float)b / 255.0f,
            (float)a / 255.0f
        );
    }

    // Octahedral 16-16 unit-normal pack.
    inline uint32 PackNormal(FVector3 n)
    {
        n /= Math::Abs(n.x) + Math::Abs(n.y) + Math::Abs(n.z) + 1e-12f;
        FVector2 e = FVector2(n.x, n.y);
        if (n.z < 0.0f)
        {
            e = FVector2(
                (1.0f - Math::Abs(e.y)) * (e.x >= 0.0f ? 1.0f : -1.0f),
                (1.0f - Math::Abs(e.x)) * (e.y >= 0.0f ? 1.0f : -1.0f));
        }
        int32 qx = (int32)Math::Round(Math::Clamp(e.x, -1.0f, 1.0f) * 32767.0f);
        int32 qy = (int32)Math::Round(Math::Clamp(e.y, -1.0f, 1.0f) * 32767.0f);
        return ((uint32)(qx & 0xFFFF)) | (((uint32)(qy & 0xFFFF)) << 16);
    }

    inline FVector3 UnpackNormal(uint32 packed)
    {
        int16 sx = (int16)(packed & 0xFFFF);
        int16 sy = (int16)((packed >> 16) & 0xFFFF);
        FVector2 e = FVector2((float)sx, (float)sy) / 32767.0f;
        FVector3 n(e.x, e.y, 1.0f - Math::Abs(e.x) - Math::Abs(e.y));
        if (n.z < 0.0f)
        {
            float nx = (1.0f - Math::Abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
            float ny = (1.0f - Math::Abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
            n.x = nx;
            n.y = ny;
        }
        return Math::Normalize(n);
    }


    // Octahedral 15-15 tangent + 1-bit handedness packed into uint32 (4-byte aligned).
    // Layout: [0..14]=qx, [15..29]=qy, [30]=hand (1=+, 0=-), [31]=reserved.
    inline uint32 PackTangent(FVector3 t, float Sign)
    {
        t /= Math::Abs(t.x) + Math::Abs(t.y) + Math::Abs(t.z) + 1e-12f;
        FVector2 e = FVector2(t.x, t.y);
        if (t.z < 0.0f)
        {
            e = FVector2(
                (1.0f - Math::Abs(e.y)) * (e.x >= 0.0f ? 1.0f : -1.0f),
                (1.0f - Math::Abs(e.x)) * (e.y >= 0.0f ? 1.0f : -1.0f));
        }
        int32 qx = (int32)Math::Round(Math::Clamp(e.x, -1.0f, 1.0f) * 16383.0f);
        int32 qy = (int32)Math::Round(Math::Clamp(e.y, -1.0f, 1.0f) * 16383.0f);
        uint32 Hand = (Sign >= 0.0f) ? 1u : 0u;
        return ((uint32)(qx & 0x7FFFu))
             | (((uint32)(qy & 0x7FFFu)) << 15)
             | (Hand << 30);
    }

    inline FVector4 UnpackTangent(uint32 Packed)
    {
        // Sign-extend 15-bit fields into int32.
        auto SignExtend15 = [](uint32 V) -> int32
        {
            int32 X = (int32)(V & 0x7FFFu);
            return (X & 0x4000) ? (X | int32(0xFFFF8000u)) : X;
        };
        int32 sx = SignExtend15(Packed);
        int32 sy = SignExtend15(Packed >> 15);
        FVector2 e = FVector2((float)sx, (float)sy) / 16383.0f;
        FVector3 t(e.x, e.y, 1.0f - Math::Abs(e.x) - Math::Abs(e.y));
        if (t.z < 0.0f)
        {
            float tx = (1.0f - Math::Abs(t.y)) * (t.x >= 0.0f ? 1.0f : -1.0f);
            float ty = (1.0f - Math::Abs(t.x)) * (t.y >= 0.0f ? 1.0f : -1.0f);
            t.x = tx;
            t.y = ty;
        }
        float Sign = ((Packed >> 30) & 1u) != 0u ? 1.0f : -1.0f;
        return FVector4(Math::Normalize(t), Sign);
    }

    // Importers must zero Tangent: dedup byte-compares before MikkTSpace runs.
    // Cannot use member initializer, would break TCanBulkSerialize (needs trivial).
    struct FVertex
    {
        FVector3       Position;
        uint32          Normal;
        uint32          Tangent;
        uint32          UV;
        uint32          Color;

        friend FArchive& operator<<(FArchive& Ar, FVertex& Data)
        {
            Ar << Data.Position;
            Ar << Data.Normal;
            Ar << Data.Tangent;
            Ar << Data.UV;
            Ar << Data.Color;
            return Ar;
        }
    };

    /** Normalizes a vertex's four blend weights and quantizes them to u8 summing to EXACTLY 255.
     *
     *  Both parts matter. The GPU divides by 255 and blends the bone rows, so the quantized quartet IS the
     *  weight set -- and an unnormalized set scales the whole blended affine matrix, translation row
     *  included, dragging the vertex toward the model origin by (1 - sum). Plain `FU8Vector4(W * 255.0f)`
     *  truncates each component, so even a perfectly normalized input lands at a sum of 252-255: a silent
     *  ~1% shrink toward the root on every skinned vertex. Rounding alone still misses by +/-2.
     *
     *  Largest-remainder: floor each scaled weight, then hand the leftover units to the components with the
     *  biggest discarded fractions. Exact by construction and stable.
     *
     *  A zero-sum input (no usable influence -- e.g. a mesh whose skin data never bound) becomes rigid to
     *  joint 0 rather than all-zeroes, so it renders in bind pose instead of collapsing onto the origin.
     */
    inline FU8Vector4 PackSkinWeights(FVector4 Weights)
    {
        float W[4] = { Weights.x, Weights.y, Weights.z, Weights.w };

        float Sum = 0.0f;
        for (int32 i = 0; i < 4; ++i)
        {
            // A negative weight would make the blend leave the bones' convex hull; clamp rather than trust.
            W[i] = (W[i] > 0.0f) ? W[i] : 0.0f;
            Sum += W[i];
        }

        if (Sum <= 0.0f)
        {
            return FU8Vector4(255, 0, 0, 0);
        }

        const float Scale = 255.0f / Sum;

        int32 Quantized[4];
        float Remainder[4];
        int32 Total = 0;
        for (int32 i = 0; i < 4; ++i)
        {
            // Non-negative, so a truncating cast IS floor -- no Math::Floor dependency in this header.
            const float Scaled = W[i] * Scale;
            Quantized[i] = (int32)Scaled;
            Remainder[i] = Scaled - (float)Quantized[i];
            Total += Quantized[i];
        }

        // Hand out the shortfall (0..3 units) to the largest discarded fractions.
        for (int32 Unit = Total; Unit < 255; ++Unit)
        {
            int32 Best = 0;
            for (int32 i = 1; i < 4; ++i)
            {
                if (Remainder[i] > Remainder[Best])
                {
                    Best = i;
                }
            }
            Quantized[Best] += 1;
            Remainder[Best] = -1.0f;   // each component can only win one unit
        }

        // Each component is in [0,255] by construction (non-negative floors of values summing to 255, plus at
        // most one extra unit each), so no clamp is needed.
        return FU8Vector4((uint8)Quantized[0], (uint8)Quantized[1], (uint8)Quantized[2], (uint8)Quantized[3]);
    }

    struct FSkinnedVertex : FVertex
    {
        FU8Vector4     JointIndices;
        FU8Vector4     JointWeights;

        friend FArchive& operator<<(FArchive& Ar, FSkinnedVertex& Data)
        {
            Ar << Data.Position;
            Ar << Data.Normal;
            Ar << Data.Tangent;
            Ar << Data.UV;
            Ar << Data.Color;
            Ar << Data.JointIndices;
            Ar << Data.JointWeights;
            return Ar;
        }
    };

    // 28-byte runtime vertex. Position is full mesh-local float3 (exact, gap-free across meshlet boundaries);
    // Normal/Tangent octahedral; UV half2; Color RGBA8.
    struct FMeshletVertex
    {
        FVector3 Position;
        uint32 Normal;
        uint32 Tangent;
        uint32 UV;
        uint32 Color;
    };

    struct FMeshletSkinnedVertex : FMeshletVertex
    {
        uint32 JointIndices;
        uint32 JointWeights;
    };

    struct FSimpleElementVertex
    {
        FVector3   Position;
        uint32      Color;
    };

    struct FBillboardVertex
    {
        FVector3   Position;
        float       Size;
    };

    static_assert(sizeof(FVertex) == 28);
    static_assert(sizeof(FSkinnedVertex) == 36);
    static_assert(sizeof(FMeshletVertex) == 28);
    static_assert(sizeof(FMeshletSkinnedVertex) == 36);
    static_assert(offsetof(FVertex, Position) == 0);
    static_assert(TCanBulkSerialize<FVertex>::value);
    static_assert(TCanBulkSerialize<FSkinnedVertex>::value);
    static_assert(TCanBulkSerialize<FMeshletVertex>::value);
    static_assert(TCanBulkSerialize<FMeshletSkinnedVertex>::value);
    static_assert(TCanBulkSerialize<FBillboardVertex>::value);
    static_assert(TCanBulkSerialize<FSimpleElementVertex>::value);

}
