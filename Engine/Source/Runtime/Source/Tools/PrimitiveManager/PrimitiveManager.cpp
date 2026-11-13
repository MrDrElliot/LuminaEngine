#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "PrimitiveManager.h"

#include "Core/Engine/Engine.h"
#include "Core/Object/Package/Package.h"
#include "Renderer/MeshDistanceField.h"
#include "Tools/Import/ImportHelpers.h"
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
                                                 bool bDistanceField,
                                                 auto&& Generate)
        {
            TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();

            // Scatters the interleaved build array into the resource's structure-of-arrays streams.
            TVector<FSourceVertex> GeneratedVerts;
            Generate(GeneratedVerts, Resource->Indices);
            Resource->ReserveVertices(GeneratedVerts.Num());
            for (const FSourceVertex& V : GeneratedVerts)
            {
                Resource->AppendVertex(V);
            }

            FGeometrySurface Surface;
            Surface.ID = SurfaceID;
            Surface.IndexCount = (uint32)Resource->Indices.Num();
            Surface.StartIndex = 0;
            Surface.MaterialIndex = 0;
            Resource->GeometrySurfaces.Add(Surface);

            OutMesh = NewObject<CStaticMesh>(TransientPackage, ObjectName, FGuid::NewDeterministic(DeterministicTag));
            OutMesh->Materials.SetNum(1);

            // Baked before staging, since the live resource only publishes once its upload lands, and the field rides that upload.
            if (bDistanceField && !Global::GIsHeadless)
            {
                OutMesh->DistanceFieldSettings.bEnabled   = true;
                OutMesh->DistanceFieldSettings.Resolution = 32;
                Import::Mesh::GenerateMeshlets(*Resource);
                DistanceField::Build(*Resource, OutMesh->DistanceFieldSettings, Resource->DistanceField);
            }
            OutMesh->SetMeshResource(Move(Resource));
        };

        // The plane is a single sheet, which a signed field cannot describe.
        BuildPrimitive(CubeMesh,     "EngineCubeMesh",     "CubeMesh",     "Engine.PrimitiveMesh.Cube",     true,  PrimitiveMeshes::GenerateCube);
        BuildPrimitive(SphereMesh,   "EngineSphereMesh",   "SphereMesh",   "Engine.PrimitiveMesh.Sphere",   true,  [](auto& V, auto& I) { PrimitiveMeshes::GenerateSphere(V, I); });
        BuildPrimitive(PlaneMesh,    "EnginePlaneMesh",    "PlaneMesh",    "Engine.PrimitiveMesh.Plane",    false, PrimitiveMeshes::GeneratePlane);
        BuildPrimitive(CylinderMesh, "EngineCylinderMesh", "CylinderMesh", "Engine.PrimitiveMesh.Cylinder", true,  [](auto& V, auto& I) { PrimitiveMeshes::GenerateCylinder(V, I); });
        BuildPrimitive(ConeMesh,     "EngineConeMesh",     "ConeMesh",     "Engine.PrimitiveMesh.Cone",     true,  [](auto& V, auto& I) { PrimitiveMeshes::GenerateCone(V, I); });
        BuildPrimitive(CapsuleMesh,  "EngineCapsuleMesh",  "CapsuleMesh",  "Engine.PrimitiveMesh.Capsule",  true,  [](auto& V, auto& I) { PrimitiveMeshes::GenerateCapsule(V, I); });
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
