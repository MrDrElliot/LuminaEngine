#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Lumina.h"

namespace Lumina::Import::VoxelFar
{
    // One surface in its own compact vertex space, voxelized on a grid the whole mesh shares.
    struct FBuildInput
    {
        TSpan<const uint32>   Indices;
        TSpan<const FVector3> Positions;
        // AttributeCount floats per vertex, normal xyz first and then uv.
        TSpan<const float>    Attributes;
        uint32                AttributeCount = 5;
        // Corner of the finest grid and the size of one of its voxels.
        FVector3              GridMin = FVector3(0.0f);
        float                 VoxelSize = 1.0f;
        // Voxels along the longest side at the finest level, a power of two.
        uint32                Resolution = 32;
    };

    // One splat level, a triangle per surface voxel and two for a voxel whose surfaces face both ways, in its own vertex space.
    struct FLevel
    {
        TVector<FVector3> Positions;
        // AttributeCount per vertex, in the input's layout.
        TVector<float>    Attributes;
        // The input vertex each emitted vertex stands in for, which is where its color and second UV come from.
        TVector<uint32>   SourceVertex;
        TVector<uint32>   Indices;
        float             VoxelSize = 0.0f;
        // How far a splat corner reaches from its voxel center, the distance a splat can stray from the surface it stands for.
        float             Error = 0.0f;
        uint32            VoxelCount = 0;
    };

    // Finest first, halving the resolution each level down to two voxels a side.
    RUNTIME_API void Build(const FBuildInput& In, TVector<FLevel>& OutLevels);
}
