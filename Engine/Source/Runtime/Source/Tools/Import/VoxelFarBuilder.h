#pragma once

#include "Containers/HashTable.h"
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
        // Drops voxels buried under enough foliage on every side, which a canopy of many plants is mostly made of.
        bool                  bCullOccluded = false;
        // Shades each splat toward the open side of the surfaces around it, so a crown lights as one volume rather than voxel by voxel.
        bool                  bCanopyNormals = false;
        // A voxel whose surfaces face both ways draws one splat toward its open side, for geometry only ever seen from outside.
        bool                  bFaceOutward = false;
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

    // The surface a triangle puts into one voxel of the grid.
    struct FVoxelCell
    {
        FVector3 NormalSum = FVector3(0.0f);
        // Surface area inside the voxel, which is what the splat's size stands for.
        float    Area      = 0.0f;
        // The largest triangle's UV box, which the material pass averages over rather than one texel of the atlas.
        FVector2 UVMin     = FVector2(0.0f);
        FVector2 UVMax     = FVector2(0.0f);
        FVector3 BestNormal = FVector3(0.0f);
        float    BestArea  = 0.0f;
        uint32   Source    = 0;

        void Merge(const FVoxelCell& Other);
    };

    // One surface voxelized at every level of its grid, finest first, halving the resolution down to two voxels a side.
    struct FSurfaceVoxels
    {
        TVector<THashMap<uint32, FVoxelCell>> Levels;
    };

    struct FVoxelOwner
    {
        // Every surface's area in the voxel.
        float  Area      = 0.0f;
        float  OwnerArea = 0.0f;
        uint32 Owner     = 0;
    };

    // Per level, which of several surfaces on one grid holds the most of each voxel, and how much they hold together.
    struct FSharedVoxels
    {
        TVector<THashMap<uint32, FVoxelOwner>> Levels;
    };

    RUNTIME_API void Voxelize(const FBuildInput& In, FSurfaceVoxels& Out);

    // Every surface must have been voxelized on the same grid.
    RUNTIME_API void Share(TSpan<const FSurfaceVoxels> Surfaces, FSharedVoxels& Out);

    // With Shared, a voxel draws only the surface holding most of it, and all the surfaces together hide and shade one another.
    RUNTIME_API void Emit(const FBuildInput& In, const FSurfaceVoxels& Voxels, const FSharedVoxels* Shared, uint32 SurfaceIndex, TVector<FLevel>& OutLevels);

    // One surface on its own, voxelized and emitted.
    RUNTIME_API void Build(const FBuildInput& In, TVector<FLevel>& OutLevels);
}
