#pragma once

#include "Lumina.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Shared/SharedConstants.h"
#include "meshoptimizer.h"

#include <bit>

namespace Lumina
{
    constexpr int32 MeshPositionBits = MESH_POSITION_BITS;
    static_assert((1u << MeshPositionBits) - 1u == MESH_POSITION_MAX, "MESH_POSITION_MAX must match MESH_POSITION_BITS");

    // Built from the float exponent field rather than exp2, so C++ and the shader produce identical bits.
    FORCEINLINE constexpr float PositionGridScale(int32 Exponent)
    {
        return std::bit_cast<float>((uint32)(Math::Clamp(Exponent, -126, 127) + 127) << 23);
    }

    inline FMeshPositionGrid ComputeMeshPositionGrid(const FVector3* Positions, size_t Count)
    {
        FMeshPositionGrid Grid;
        if (Positions == nullptr || Count == 0)
        {
            return Grid;
        }

        FVector3 Min = Positions[0];
        FVector3 Max = Positions[0];
        for (size_t i = 1; i < Count; ++i)
        {
            Min = Math::Min(Min, Positions[i]);
            Max = Math::Max(Max, Positions[i]);
        }

        Grid.Exponent = meshopt_computePositionExponent(&Min.x, &Max.x, -126, MeshPositionBits);

        const float Scale = PositionGridScale(Grid.Exponent);
        Grid.AnchorX = (int32)Math::Floor(Min.x / Scale);
        Grid.AnchorY = (int32)Math::Floor(Min.y / Scale);
        Grid.AnchorZ = (int32)Math::Floor(Min.z / Scale);
        return Grid;
    }

    FORCEINLINE FMeshVertexPosition EncodeMeshPosition(const FMeshPositionGrid& Grid, const FVector3& P)
    {
        const float Scale = PositionGridScale(Grid.Exponent);

        auto Component = [&](float Value, int32 Anchor) -> uint32
        {
            const int64 Offset = (int64)Math::Floor(Value / Scale + 0.5f) - (int64)Anchor;
            return (uint32)Math::Clamp<int64>(Offset, (int64)0, (int64)MESH_POSITION_MAX);
        };

        const uint32 X = Component(P.x, Grid.AnchorX);
        const uint32 Y = Component(P.y, Grid.AnchorY);
        const uint32 Z = Component(P.z, Grid.AnchorZ);

        FMeshVertexPosition Out;
        Out.XY = X | (Y << MeshPositionBits);
        Out.YZ = (Y >> (32 - MeshPositionBits)) | (Z << (2 * MeshPositionBits - 32));
        return Out;
    }

    FORCEINLINE FVector3 DecodeMeshPosition(const FMeshPositionGrid& Grid, const FMeshVertexPosition& P)
    {
        const float  Scale = PositionGridScale(Grid.Exponent);
        const uint32 X     = P.XY & MESH_POSITION_MAX;
        const uint32 Y     = ((P.XY >> MeshPositionBits) | (P.YZ << (32 - MeshPositionBits))) & MESH_POSITION_MAX;
        const uint32 Z     = (P.YZ >> (2 * MeshPositionBits - 32)) & MESH_POSITION_MAX;

        return FVector3(
            (float)(Grid.AnchorX + (int32)X) * Scale,
            (float)(Grid.AnchorY + (int32)Y) * Scale,
            (float)(Grid.AnchorZ + (int32)Z) * Scale);
    }

    // Appends a static meshlet's refs, as 16-bit offsets from its lowest vertex whenever the span allows.
    inline void AppendMeshletVertexRefs(FMeshletData& Data, FMeshlet& M, const uint32* Vertices, uint32 Count)
    {
        uint32 Lo = Count > 0 ? Vertices[0] : 0u;
        uint32 Hi = Lo;
        for (uint32 i = 1; i < Count; ++i)
        {
            Lo = Math::Min(Lo, Vertices[i]);
            Hi = Math::Max(Hi, Vertices[i]);
        }

        const bool bShort = (Hi - Lo) <= 0xFFFFu;
        M.VertexOffset     = (uint32)Data.MeshletVertexRefs.size();
        M.VertexCount      = Count;
        M.BaseVertex       = Lo;
        M.bShortVertexRefs = bShort ? 1u : 0u;

        if (bShort)
        {
            for (uint32 i = 0; i < Count; i += 2)
            {
                const uint32 First  = Vertices[i] - Lo;
                const uint32 Second = (i + 1 < Count) ? Vertices[i + 1] - Lo : 0u;
                Data.MeshletVertexRefs.push_back(First | (Second << 16));
            }
        }
        else
        {
            for (uint32 i = 0; i < Count; ++i)
            {
                Data.MeshletVertexRefs.push_back(Vertices[i] - Lo);
            }
        }
    }

    // Where a static meshlet's Local-th vertex sits in the mesh's shared vertex streams.
    FORCEINLINE uint32 GetStaticVertexIndex(const FMeshletData& Data, const FMeshlet& M, uint32 Local)
    {
        if (M.HasShortVertexRefs())
        {
            const uint32 Word = Data.MeshletVertexRefs[M.VertexOffset + Local / 2u];
            return M.BaseVertex + ((Local & 1u) ? (Word >> 16) : (Word & 0xFFFFu));
        }
        return M.BaseVertex + Data.MeshletVertexRefs[M.VertexOffset + Local];
    }

    FORCEINLINE FVector3 GetMeshletVertexPosition(const FMeshletData& Data, const FMeshlet& M, uint32 Local, bool bSkinned)
    {
        if (bSkinned)
        {
            return DecodeMeshPosition(Data.PositionGrid, Data.MeshletSkinnedVertices[M.VertexOffset + Local].Position);
        }
        return DecodeMeshPosition(Data.PositionGrid, Data.VertexPositions[GetStaticVertexIndex(Data, M, Local)]);
    }
}
