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
