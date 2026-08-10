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

    /** CPU-side interleaved staging for import, procedural primitives and geometry collections.
     *  NOT a GPU format and has no Slang counterpart, so nothing here is size-constrained. */
    struct FSourceVertex
    {
        FVector3       Position;
        uint32          Normal;
        uint32          Tangent;
        uint32          UV;      // packHalf2x16, TEXCOORD_0
        uint32          UV1;     // packHalf2x16, TEXCOORD_1; equals UV when the source has one set
        uint32          Color;

        friend FArchive& operator<<(FArchive& Ar, FSourceVertex& Data)
        {
            Ar << Data.Position;
            Ar << Data.Normal;
            Ar << Data.Tangent;
            Ar << Data.UV;
            Ar << Data.UV1;
            Ar << Data.Color;
            return Ar;
        }
    };

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

        return FU8Vector4((uint8)Quantized[0], (uint8)Quantized[1], (uint8)Quantized[2], (uint8)Quantized[3]);
    }

    struct FSourceSkinnedVertex : FSourceVertex
    {
        FU8Vector4     JointIndices;
        FU8Vector4     JointWeights;

        friend FArchive& operator<<(FArchive& Ar, FSourceSkinnedVertex& Data)
        {
            Ar << Data.Position;
            Ar << Data.Normal;
            Ar << Data.Tangent;
            Ar << Data.UV;
            Ar << Data.UV1;
            Ar << Data.Color;
            Ar << Data.JointIndices;
            Ar << Data.JointWeights;
            return Ar;
        }
    };

    /** The GPU static-vertex format. Common.slang declares an FMeshletVertex that must match field for
     *  field. Position is a 16-bit per-axis offset from the meshlet anchor; decode via MeshQuantization.h. */
    struct FMeshletVertex
    {
        uint16 PositionX;
        uint16 PositionY;
        uint16 PositionZ;
        int16  NormalX;
        int16  NormalY;
        int16  NormalZ;
        uint32 Tangent;
        uint32 UV;   // packHalf2x16, TEXCOORD_0
        uint32 UV1;  // packHalf2x16, TEXCOORD_1
        uint32 Color;

        [[nodiscard]] constexpr FVector3 GetNormal() const
        {
            return FVector3(Math::SNorm16ToFloat(NormalX),
                            Math::SNorm16ToFloat(NormalY),
                            Math::SNorm16ToFloat(NormalZ));
        }
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

    // FMeshlet* are the GPU formats and Common.slang declares structs of the SAME NAME that must stay
    // identical -- re-verify the Slang ArrayStride when a field moves. FSource* are CPU-only and free.
    static_assert(sizeof(FSourceVertex) == 32);
    static_assert(sizeof(FSourceSkinnedVertex) == 40);
    static_assert(sizeof(FMeshletVertex) == 28);
    static_assert(sizeof(FMeshletSkinnedVertex) == 36);
    static_assert(offsetof(FSourceVertex, Position) == 0);
    static_assert(TCanBulkSerialize<FSourceVertex>::value);
    static_assert(TCanBulkSerialize<FSourceSkinnedVertex>::value);
    static_assert(TCanBulkSerialize<FMeshletVertex>::value);
    static_assert(TCanBulkSerialize<FMeshletSkinnedVertex>::value);
    static_assert(TCanBulkSerialize<FBillboardVertex>::value);
    static_assert(TCanBulkSerialize<FSimpleElementVertex>::value);

}
