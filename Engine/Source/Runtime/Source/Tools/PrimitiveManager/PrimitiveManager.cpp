#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "PrimitiveManager.h"

#include "Core/Engine/Engine.h"
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

        auto BuildPrimitive = [TransientPackage](TStrongObjectPtr<CStaticMesh>& OutMesh,
                                                 const char* ObjectName,
                                                 const char* SurfaceID,
                                                 const char* DeterministicTag,
                                                 auto&& Generate)
        {
            TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();

            // Scatters the interleaved build array into the resource's structure-of-arrays streams.
            TVector<FSourceVertex> GeneratedVerts;
            Generate(GeneratedVerts, Resource->Indices);
            Resource->ReserveVertices(GeneratedVerts.size());
            for (const FSourceVertex& V : GeneratedVerts)
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

        // Coarse fields are enough for the distance-field sun shadows that blockouts and demo scenes cast with.
        auto BuildPrimitiveDistanceField = [](CStaticMesh* Mesh)
        {
            if (GIsHeadless || Mesh == nullptr)
            {
                return;
            }
            Mesh->DistanceFieldSettings.bEnabled   = true;
            Mesh->DistanceFieldSettings.Resolution = 32;
            Mesh->BuildDistanceField();
        };

        BuildPrimitive(CubeMesh,     "EngineCubeMesh",     "CubeMesh",     "Engine.PrimitiveMesh.Cube",     PrimitiveMeshes::GenerateCube);
        BuildPrimitive(SphereMesh,   "EngineSphereMesh",   "SphereMesh",   "Engine.PrimitiveMesh.Sphere",   [](auto& V, auto& I) { PrimitiveMeshes::GenerateSphere(V, I); });
        BuildPrimitive(PlaneMesh,    "EnginePlaneMesh",    "PlaneMesh",    "Engine.PrimitiveMesh.Plane",    PrimitiveMeshes::GeneratePlane);
        BuildPrimitive(CylinderMesh, "EngineCylinderMesh", "CylinderMesh", "Engine.PrimitiveMesh.Cylinder", [](auto& V, auto& I) { PrimitiveMeshes::GenerateCylinder(V, I); });
        BuildPrimitive(ConeMesh,     "EngineConeMesh",     "ConeMesh",     "Engine.PrimitiveMesh.Cone",     [](auto& V, auto& I) { PrimitiveMeshes::GenerateCone(V, I); });
        BuildPrimitive(CapsuleMesh,  "EngineCapsuleMesh",  "CapsuleMesh",  "Engine.PrimitiveMesh.Capsule",  [](auto& V, auto& I) { PrimitiveMeshes::GenerateCapsule(V, I); });

        // The plane is a single sheet, which a signed field cannot describe.
        for (CStaticMesh* Mesh : { CubeMesh.Get(), SphereMesh.Get(), CylinderMesh.Get(), ConeMesh.Get(), CapsuleMesh.Get() })
        {
            BuildPrimitiveDistanceField(Mesh);
        }
    }

    CPrimitiveManager& CPrimitiveManager::Get()
    {
        static FOnceFlag Flag;
        CallOnce(Flag, []()
        {
            PrimitiveManagerSingleton = NewObject<CPrimitiveManager>();
            PrimitiveManagerSingleton->Initialize();
        });

        return *PrimitiveManagerSingleton;
    }
}
