#include "RuntimePCH.h"
#include "MeshLibrary.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Package/Package.h"
#include "Renderer/Vertex.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/World.h"

#include <atomic>

namespace Lumina
{
    CStaticMesh* CMeshLibrary::GetPrimitiveMesh(EPrimitiveShape Shape)
    {
        CPrimitiveManager& Primitives = CPrimitiveManager::Get();
        switch (Shape)
        {
        case EPrimitiveShape::Cube:     return Primitives.CubeMesh.Get();
        case EPrimitiveShape::Sphere:   return Primitives.SphereMesh.Get();
        case EPrimitiveShape::Plane:    return Primitives.PlaneMesh.Get();
        case EPrimitiveShape::Cylinder: return Primitives.CylinderMesh.Get();
        case EPrimitiveShape::Cone:     return Primitives.ConeMesh.Get();
        case EPrimitiveShape::Capsule:  return Primitives.CapsuleMesh.Get();
        }
        return nullptr;
    }

    CStaticMesh* CMeshLibrary::CreateStaticMesh(CWorld* World, const float* Positions, int32 PositionFloats,
        const float* Normals, int32 NormalFloats, const float* UVs, int32 UVFloats,
        const float* Colors, int32 ColorFloats, const uint32* Indices, int32 IndexCount,
        CMaterialInterface* Material, int32 MaxLODs)
    {
        const int32 VertexCount = PositionFloats / 3;
        if (World == nullptr || Positions == nullptr || Indices == nullptr || VertexCount == 0 || IndexCount < 3)
        {
            return nullptr;
        }

        LUMINA_PROFILE_SCOPE();

        FDynamicMeshBuildData Data;
        Data.Positions.resize(VertexCount);
        Memory::Memcpy(Data.Positions.data(), Positions, sizeof(FVector3) * VertexCount);

        if (Normals != nullptr && NormalFloats == PositionFloats)
        {
            Data.Normals.resize(VertexCount);
            Memory::Memcpy(Data.Normals.data(), Normals, sizeof(FVector3) * VertexCount);
        }

        if (UVs != nullptr && UVFloats == VertexCount * 2)
        {
            Data.UVs.resize(VertexCount);
            Memory::Memcpy(Data.UVs.data(), UVs, sizeof(FVector2) * VertexCount);
        }

        if (Colors != nullptr && ColorFloats == VertexCount * 4)
        {
            Data.Colors.resize(VertexCount);
            for (int32 Index = 0; Index < VertexCount; ++Index)
            {
                const float* Color = Colors + Index * 4;
                Data.Colors[Index] = PackColor(FVector4(Color[0], Color[1], Color[2], Color[3]));
            }
        }

        Data.Indices.assign(Indices, Indices + IndexCount);

        // Built once and drawn many times, so the slower, tighter meshlet build pays for itself.
        FMeshBuildOptions Options;
        Options.MaxLODs             = (uint32)Math::Max(MaxLODs, 1);
        Options.bMeshletConeCulling = true;
        Options.bFastMeshletBuild   = false;

        TUniquePtr<FMeshResource> Resource = BuildMeshResource(Data, Options);

        static std::atomic<uint32> Serial = 0;
        CStaticMesh* Mesh = NewObject<CStaticMesh>(CPackage::GetTransientPackage(), FName(Format("ScriptMesh_{}", Serial.fetch_add(1))));

        // An empty slot never reports ready, which would keep the mesh from ever drawing.
        Mesh->Materials.resize(1);
        Mesh->Materials[0] = Material != nullptr ? Material : static_cast<CMaterialInterface*>(CMaterial::GetDefaultMaterial());

        World->RetainObject(Mesh);
        Mesh->SetMeshResourceBatched(Move(Resource));
        return Mesh;
    }
}
