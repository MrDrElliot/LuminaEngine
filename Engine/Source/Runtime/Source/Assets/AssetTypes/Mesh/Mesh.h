#pragma once

#include "Containers/String.h"
#include "Core/Math/AABB.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/MeshData.h"
#include "Memory/SmartPtr.h"
#include "Mesh.generated.h"

namespace Lumina
{
    class CMaterialInterface;
}

namespace Lumina
{
    namespace MeshBuffers
    {
        // Uploads the geometry and writes its header, for a resource with or without a CMesh, keeping the CPU streams and resolve entries as they are.
        RUNTIME_API void CreateForResource(FMeshResource& Resource);

        // Republishes the distance field and impostor textures on the same header slot, so cached instances need no new address.
        RUNTIME_API void RefreshTextures(FMeshResource& Resource);
    }

    REFLECT()
    class RUNTIME_API CMesh : public CObject
    {
        GENERATED_BODY()

        friend class CMeshImporter;

    public:

        void Serialize(FArchive& Ar) override;
        void PostLoad() override;
        void OnReferencesReplaced() override;
        void OnDestroy() override;

        // Script code that swaps meshes every frame keeps the old one up until the new one can draw.
        FUNCTION()
        bool IsReadyForRender() const;

        // The meshlets are on the GPU, whether or not the materials are, which is all a particle or emission shape reads.
        bool IsGeometryResident() const;

        void GenerateBoundingBox();
        void GenerateGPUBuffers();

        bool HasDistanceField() const;

        // Rebuilds the field from the baked meshlets with DistanceFieldSettings, or drops it when they are off, leaving the package clean.
        void BuildDistanceField();

        bool HasImpostor() const;

        // Replaces the baked impostor and publishes it on the live header. Leaves the package clean for the caller to dirty.
        void SetImpostor(FMeshImpostor&& Impostor);

        // Rebuilds the meshlets and cluster hierarchy from the baked finest level, so no source file is needed.
        bool RebuildVirtualGeometry();

        uint32 GetNumMaterials() const { return (uint32)Materials.Num(); }
        CMaterialInterface* GetMaterialAtSlot(int32 Slot) const;
        void SetMaterialAtSlot(int32 Slot, CMaterialInterface* NewMaterial);

        FORCEINLINE FMeshResource& GetMeshResource() const { return *MeshResources.get(); }
        FORCEINLINE const FMeshResource* TryGetMeshResource() const { return MeshResources.get(); }

        // The staged resource while one waits to publish, so an edit builds on the newest geometry.
        FORCEINLINE const FMeshResource& GetNewestMeshResource() const { return PendingMeshResources ? *PendingMeshResources : *MeshResources; }

        // Stages the resource and has the renderer publish it once its upload has landed, or publishes at once with no renderer.
        void SetMeshResource(TUniquePtr<FMeshResource>&& NewResource);

        // Uploads the resource's geometry and holds it beside the current one, which keeps drawing until PublishPendingMeshResource.
        void StageMeshResource(TUniquePtr<FMeshResource>&& NewResource);

        // Swaps the staged resource in once its upload batch is complete. True when nothing is left pending.
        bool PublishPendingMeshResource();

        // Leaves meshlets and GPU buffers to an open FMeshBuildBatchScope, which then stages the resource like SetMeshResource.
        void SetMeshResourceBatched(TUniquePtr<FMeshResource>&& NewResource);

        // Counts how often published geometry was replaced, so data built from the old geometry can tell it is stale.
        FORCEINLINE uint32 GetGeometryVersion() const { return GeometryVersion; }

        // The two halves a batch runs, the first safely in parallel with other meshes and the second on the game thread.
        void BuildDeferredMeshlets();
        void UploadDeferredBuffers();

        FORCEINLINE const FMeshResource::FMeshBuffers& GetMeshBuffers() const { return MeshResources->MeshBuffers; }

        virtual bool IsSkinned() const { return false; }

        FUNCTION()
        FORCEINLINE const SAABB& GetAABB() const { return BoundingBox; }

        // The box the renderer culls against, in the space the mesh is drawn in rather than the one its vertices are stored in.
        virtual SAABB GetCullBounds() const { return BoundingBox; }

        template<typename TCallable>
        void ForEachSurface(TCallable&& Lambda) const;

        PROPERTY(Editable, NoResize, NoReorder, Category = "Materials", DisplayName = "Slots")
        TVector<TStrongObjectPtr<CMaterialInterface>> Materials;

        PROPERTY(Hidden, Category = "AABB")
        SAABB BoundingBox;

        // Multiplies this mesh's LOD errors, so 2 switches twice as far away and 0.5 lets card foliage coarsen sooner.
        PROPERTY(Editable, Category = "Level of Detail", ClampMin = 0.05f, ClampMax = 4.0f)
        float LODDistanceScale = 1.0f;

        // Seeded from the import dialog and applied by the mesh editor's Build Distance Field, so a field changes without the source file.
        PROPERTY(Editable, Category = "Distance Field", DisplayName = "Settings")
        SDistanceFieldBuildSettings DistanceFieldSettings;

        // The file this mesh was last imported from, which Reimport From File opens on, empty for procedural meshes.
        PROPERTY(Hidden)
        FString SourcePath;

    private:

        // Creates the GPU buffers and drops the import-time scratch they were built from.
        static void UploadGeometry(FMeshResource& Resource);

        void ReplaceMeshResources(TUniquePtr<FMeshResource>&& NewResource);

        TUniquePtr<FMeshResource> MeshResources;

        uint32 GeometryVersion = 0;

        // A replacement whose upload has not landed; the live resource keeps drawing until it does.
        TUniquePtr<FMeshResource> PendingMeshResources;
    };

    template <typename TCallable>
    void CMesh::ForEachSurface(TCallable&& Lambda) const
    {
        uint32 Count = 0;
        for (const FGeometrySurface& Surface : MeshResources->GeometrySurfaces)
        {
            Forward<TCallable>(Lambda)(Surface, Count);
            ++Count;
        }
    }
}
