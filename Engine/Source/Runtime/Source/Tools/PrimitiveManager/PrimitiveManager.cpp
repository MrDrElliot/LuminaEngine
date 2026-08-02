#include "RuntimePCH.h"
#include "PrimitiveManager.h"

#include "Core/Object/Package/Package.h"
#include "World/Scene/RenderScene/SceneMeshes.h"

namespace Lumina
{
    static CPrimitiveManager* PrimitiveManagerSingleton = nullptr;

    CPrimitiveManager::CPrimitiveManager()
    {
    }

    void CPrimitiveManager::Initialize()
    {
        // Transient package with stable GUIDs so worlds reference them across save/load without disk files.
        CPackage* TransientPackage = CPackage::GetTransientPackage();

        auto BuildPrimitive = [TransientPackage](TObjectPtr<CStaticMesh>& OutMesh,
                                                 const char* ObjectName,
                                                 const char* SurfaceID,
                                                 const char* DeterministicTag,
                                                 auto&& Generate)
        {
            TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();

            // The Generate* helpers build an interleaved FVertex array; scatter it into
            // the resource's structure-of-arrays vertex streams.
            TVector<FVertex> GeneratedVerts;
            Generate(GeneratedVerts, Resource->Indices);
            Resource->ReserveVertices(GeneratedVerts.size());
            for (const FVertex& V : GeneratedVerts)
            {
                Resource->AppendVertex(V);
            }

            FGeometrySurface Surface;
            Surface.ID = SurfaceID;
            Surface.IndexCount = (uint32)Resource->Indices.size();
            Surface.StartIndex = 0;
            Surface.MaterialIndex = 0;
            Resource->GeometrySurfaces.push_back(Surface);

            OutMesh = NewObject<CStaticMesh>(TransientPackage, ObjectName, FGuid::NewDeterministic(DeterministicTag));
            OutMesh->Materials.resize(1);
            OutMesh->SetMeshResource(Move(Resource));
        };

        BuildPrimitive(CubeMesh,     "EngineCubeMesh",     "CubeMesh",     "Engine.PrimitiveMesh.Cube",     PrimitiveMeshes::GenerateCube);
        BuildPrimitive(SphereMesh,   "EngineSphereMesh",   "SphereMesh",   "Engine.PrimitiveMesh.Sphere",   [](auto& V, auto& I) { PrimitiveMeshes::GenerateSphere(V, I); });
        BuildPrimitive(PlaneMesh,    "EnginePlaneMesh",    "PlaneMesh",    "Engine.PrimitiveMesh.Plane",    PrimitiveMeshes::GeneratePlane);
        BuildPrimitive(CylinderMesh, "EngineCylinderMesh", "CylinderMesh", "Engine.PrimitiveMesh.Cylinder", [](auto& V, auto& I) { PrimitiveMeshes::GenerateCylinder(V, I); });
        BuildPrimitive(ConeMesh,     "EngineConeMesh",     "ConeMesh",     "Engine.PrimitiveMesh.Cone",     [](auto& V, auto& I) { PrimitiveMeshes::GenerateCone(V, I); });
        BuildPrimitive(CapsuleMesh,  "EngineCapsuleMesh",  "CapsuleMesh",  "Engine.PrimitiveMesh.Capsule",  [](auto& V, auto& I) { PrimitiveMeshes::GenerateCapsule(V, I); });
    }

    CPrimitiveManager& CPrimitiveManager::Get()
    {
        static std::once_flag Flag;
        std::call_once(Flag, []()
        {
            PrimitiveManagerSingleton = NewObject<CPrimitiveManager>();
            PrimitiveManagerSingleton->Initialize();
        });

        return *PrimitiveManagerSingleton;
    }
}
