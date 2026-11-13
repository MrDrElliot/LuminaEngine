#include "gtest/gtest.h"

// Pulls in editor-only headers, so these tests compile out when Tests is built without the editor.
#if WITH_EDITOR

#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kFanSurfaces = 256;

    // One triangle per surface, every one sharing the hub, so every surface's tangent pass lands on the same vertex.
    void BuildFan(FMeshResource& Resource)
    {
        const uint32 RingCount = kFanSurfaces + 1u;
        Resource.ResizeVertices((int32)(RingCount + 1u));

        Resource.Positions[0] = FVector3(0.0f);
        Resource.Normals[0]   = PackNormal(FVector3(0.0f, 1.0f, 0.0f));
        Resource.SetUVAt(0, FVector2(0.5f, 0.5f));
        Resource.SetUV1At(0, FVector2(0.0f, 0.0f));
        Resource.Colors[0] = 0xFFFFFFFFu;

        for (uint32 r = 0; r < RingCount; ++r)
        {
            const int32 i     = (int32)(r + 1u);
            const float Angle = (float)r / (float)RingCount * Math::TwoPi<float>();
            Resource.Positions[i] = FVector3(Math::Cos(Angle), 0.0f, Math::Sin(Angle));
            Resource.Normals[i]   = PackNormal(FVector3(0.0f, 1.0f, 0.0f));
            // Irregular UVs, so each triangle hands the hub a different tangent.
            Resource.SetUVAt(i, FVector2(Math::Fract((float)r * 0.37f), Math::Fract((float)r * 0.61f)));
            Resource.SetUV1At(i, FVector2(0.0f, 0.0f));
            Resource.Colors[i] = 0xFFFFFFFFu;
        }

        Resource.Indices.Reserve((int32)(kFanSurfaces * 3u));
        Resource.GeometrySurfaces.Reserve((int32)kFanSurfaces);
        for (uint32 s = 0; s < kFanSurfaces; ++s)
        {
            FGeometrySurface& Surface = Resource.GeometrySurfaces.EmplaceGetRef();
            Surface.StartIndex    = (uint32)Resource.Indices.Num();
            Surface.IndexCount    = 3u;
            Surface.MaterialIndex = 0;

            Resource.Indices.Add(0u);
            Resource.Indices.Add(s + 1u);
            Resource.Indices.Add(s + 2u);
        }
    }

    TVector<uint32> BuildFanTangents()
    {
        FMeshResource Resource;
        Resource.MaxLODs = 1u;
        BuildFan(Resource);
        Import::Mesh::GenerateMeshlets(Resource);
        return Resource.Tangents;
    }
}

TEST(ImportConcurrency, TangentsOfVerticesSharedBySurfacesAreTheSameOnEveryBuild)
{
    const TVector<uint32> First = BuildFanTangents();
    ASSERT_EQ(First.Num(), (int32)(kFanSurfaces + 2u));

    for (uint32 Run = 0; Run < 32; ++Run)
    {
        const TVector<uint32> Again = BuildFanTangents();
        ASSERT_EQ(Again.Num(), First.Num());
        for (int32 v = 0; v < First.Num(); ++v)
        {
            ASSERT_EQ(Again[v], First[v]) << "vertex " << v << " on run " << Run;
        }
    }
}

#endif
