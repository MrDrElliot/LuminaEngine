#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Lumina.h"
#include "Renderer/MeshData.h"

namespace Lumina::Import::ClusterLOD
{
    // One surface in its own compact vertex space; every level the build emits indexes these same vertices.
    struct FBuildInput
    {
        TSpan<const uint32>   Indices;
        TSpan<const FVector3> Positions;
        // AttributeCount floats per vertex, which the simplifier weighs with AttributeWeights.
        TSpan<const float>    Attributes;
        TSpan<const float>    AttributeWeights;
        uint32                AttributeCount = 0;
        // Optional meshopt_SimplifyVertex_* flags per vertex, for vertices another surface shares.
        TSpan<const uint8>    Locks;
        // meshopt_simplifyScale of Positions, which sizes the least error a level adds over its children.
        float                 ErrorScale = 1.0f;
        bool                  bConeCulling = true;
        bool                  bOptimizeMeshlets = true;
        // A level past this mesh-local error is never built, since whatever parents the roots is already closer; 0 builds every level.
        float                 MaxLevelError = 0.0f;
    };

    // Every level's meshlets, finest first and grouped within a level, each indexing Positions through Vertices.
    struct FBuildOutput
    {
        TVector<FMeshlet>          Meshlets;
        TVector<uint32>            Vertices;
        // The input's vertices first, then the copies area preservation grew, each mapped back to the input vertex it came from.
        TVector<FVector3>          Positions;
        TVector<uint32>            SourceVertex;
        TVector<uint8>             Triangles;
        TVector<FMeshletSphere>    Spheres;
        TVector<FMeshletCone>      Cones;
        TVector<FMeshletLODBounds> LODBounds;
        // NumLevels + 1 entries, so level L spans meshlets [Begin[L], Begin[L + 1]).
        TVector<uint32>            LevelMeshletBegin;
        uint32                     NumLevels = 0;

        // Largest self error of any root, which anything parenting the roots must exceed.
        float                      MaxRootError = 0.0f;
        // Sphere enclosing every level's bounds.
        FVector3                   BoundsCenter = FVector3(0.0f);
        float                      BoundsRadius = 0.0f;

        FORCEINLINE uint32 LevelBegin(uint32 Level) const { return LevelMeshletBegin[Level]; }
        FORCEINLINE uint32 LevelCount(uint32 Level) const { return LevelMeshletBegin[Level + 1] - LevelMeshletBegin[Level]; }
    };

    // Builds the hierarchy. False leaves Out empty, which happens only for an input without a whole triangle.
    RUNTIME_API bool Build(const FBuildInput& In, FBuildOutput& Out);

    // Points every root at the given parent, which is how the voxel levels take over from the coarsest triangles.
    RUNTIME_API void SetRootParents(FBuildOutput& Out, const FVector3& Center, float Radius, float Error, bool bVoxelParent);
}
