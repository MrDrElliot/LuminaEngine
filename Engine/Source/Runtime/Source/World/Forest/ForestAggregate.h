#pragma once

#include "ForestScatter.h"
#include "Renderer/MeshData.h"

namespace Lumina
{
    class CMaterialInterface;
    class CStaticMesh;
}

namespace Lumina::Forest
{
    // One level of a species surface in the mesh's own space, decoded from its meshlets so a worker can transform it.
    struct FSurfaceLevel
    {
        // How far this level strays from the source, mesh-local.
        float            Error = 0.0f;
        TVector<FVector3> Positions;
        TVector<uint32>  Normals;
        TVector<uint32>  Tangents;
        TVector<uint32>  UVs;
        TVector<uint32>  Colors;
        TVector<uint32>  Indices;
    };

    struct FSpeciesSurface
    {
        // Kept alive by the species' mesh, which the forest component holds.
        CMaterialInterface*    Material = nullptr;
        // Finest first, each coarser one with a larger error.
        TVector<FSurfaceLevel> Levels;
    };

    struct FSpeciesGeometry
    {
        TVector<FSpeciesSurface> Surfaces;
        // Changes with the decoded geometry, so a reimported mesh invalidates its cached aggregates.
        uint64                   Hash = 0;
    };

    // Game thread, since it reads the mesh's resource. Null when the mesh kept no CPU meshlets.
    RUNTIME_API TSharedPtr<const FSpeciesGeometry> DecodeSpecies(const CStaticMesh& Mesh);

    struct FNodeKey
    {
        int32 Level = 0;
        int32 X     = 0;
        int32 Z     = 0;

        uint64 Pack() const { return ((uint64)(uint8)Level << 56) | ((uint64)(uint32)(X & 0x0FFFFFFF) << 28) | (uint64)(uint32)(Z & 0x0FFFFFFF); }
        bool operator==(const FNodeKey&) const = default;
    };

    struct FAggregateRequest
    {
        TSharedPtr<const FRules>                    Rules;
        TVector<TSharedPtr<const FSpeciesGeometry>> Geometry;
        FNodeKey                                    Key;
        uint32                                      Resolution = 256;
        bool                                        bUseCache  = true;
    };

    struct FAggregateResult
    {
        FMeshResource Resource;
        FVector3      LocalMin = FVector3(0.0f);
        FVector3      LocalMax = FVector3(0.0f);
        // The entity sits here and the resource is built around it.
        FVector3      Origin   = FVector3(0.0f);
        // Per section, the species and surface whose material it draws with, packed species high and surface low.
        TVector<uint32> SectionSources;
        bool          bEmpty   = true;
    };

    RUNTIME_API float GetNodeSize(const FRules& Rules, int32 Level);
    RUNTIME_API FVector2 GetNodeMin(const FRules& Rules, const FNodeKey& Key);

    // Any thread. Reads the artifact cache first and stores what it builds.
    RUNTIME_API void BuildAggregate(const FAggregateRequest& Request, FAggregateResult& Out);
}
