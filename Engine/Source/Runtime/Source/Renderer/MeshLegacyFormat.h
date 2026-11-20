#pragma once

#include "Renderer/MeshData.h"

namespace Lumina
{
    // The pre-MESHLET_SHARED_VERTICES meshlet, whose last three words held its quantization anchor.
    struct FLegacyMeshlet
    {
        uint32 VertexOffset;
        uint32 TriangleOffset;
        uint32 VertexCount;
        uint32 TriangleCount;
        uint32 LODIndex;
        uint32 PackedAnchorX;
        uint32 PackedAnchorY;
        uint32 PackedAnchorZ;
    };
    static_assert(sizeof(FLegacyMeshlet) == 32);
    static_assert(TCanBulkSerialize<FLegacyMeshlet>::value);

    // The pre-MESHLET_SHARED_VERTICES static vertex, copied into every meshlet that used it.
    struct FLegacyMeshletVertex
    {
        uint16 PositionX;
        uint16 PositionY;
        uint16 PositionZ;
        int16  NormalX;
        int16  NormalY;
        int16  NormalZ;
        uint32 Tangent;
        uint32 UV;
        uint32 UV1;
        uint32 Color;
    };
    static_assert(sizeof(FLegacyMeshletVertex) == 28);
    static_assert(TCanBulkSerialize<FLegacyMeshletVertex>::value);

    // The pre-MESHLET_SHARED_VERTICES skinned vertex, positioned against its meshlet's anchor.
    struct FLegacyMeshletSkinnedVertex
    {
        FLegacyMeshletVertex Base;
        uint32               JointIndices;
        uint32               JointWeights;
    };
    static_assert(sizeof(FLegacyMeshletSkinnedVertex) == 36);
    static_assert(TCanBulkSerialize<FLegacyMeshletSkinnedVertex>::value);

    // Rebuilds old meshlets and their per-meshlet vertex copies in the current format, so an old package needs no reimport.
    RUNTIME_API void ConvertLegacyMeshletData(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets,
                                              const TVector<FLegacyMeshletVertex>& StaticVertices,
                                              const TVector<FLegacyMeshletSkinnedVertex>& SkinnedVertices);
}
