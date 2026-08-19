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
    class CMaterialInstance;
    class CMaterial;
}

namespace Lumina
{
    namespace MeshBuffers
    {
        // Creates the meshlet/bounds/vertex/triangle buffers plus the header that indexes them, and stores
        // the addresses on Resource.MeshBuffers. Split out of CMesh because the dynamic-mesh component owns
        // a resource without owning a CMesh -- and unlike CMesh::GenerateGPUBuffers this does NOT bump the
        // resolve epoch or drop the CPU scratch, which are the asset path's concerns.
        RUNTIME_API void CreateForResource(FMeshResource& Resource);

        // Re-uploads Resource.DistanceField into a fresh volume texture and rewrites the meshlet header
        // IN PLACE with its new heap slot. The header address is preserved, so cached header addresses
        // stay valid and no resolve-cache invalidation is needed. No-op when the mesh has no header yet.
        RUNTIME_API void RefreshDistanceField(FMeshResource& Resource);
    }

    REFLECT()
    class RUNTIME_API CMesh : public CObject
    {
        GENERATED_BODY()
        
        friend class CMeshImporter;
        
    public:
        
        using Super::Serialize;
        void Serialize(FArchive& Ar) override;
        void PostLoad() override;
        void OnDestroy() override;

        bool IsReadyForRender() const;

        void GenerateBoundingBox();
        void GenerateGPUBuffers();

        /** True when a field is baked and resident. */
        bool HasDistanceField() const;

        /** (Re)voxelize this mesh from its baked meshlets using DistanceFieldSettings, then refresh the
         *  GPU buffers so the new volume and its heap slot reach the shader. Clears the field instead
         *  when the settings are disabled. Does NOT mark the package dirty -- the caller decides that.
         *
         *  Runs the build synchronously on the calling thread (parallel internally), which for a default
         *  48^3 field is well under a second, but scales cubically with Resolution. */
        void BuildDistanceField();

        uint32 GetNumMaterials() const { return (uint32)Materials.size(); }
        CMaterialInterface* GetMaterialAtSlot(size_t Slot) const;
        void SetMaterialAtSlot(size_t Slot, CMaterialInterface* NewMaterial);
        
        FORCEINLINE const FGeometrySurface& GetSurface(size_t Slot) const { return MeshResources->GeometrySurfaces[Slot]; }
        FORCEINLINE FMeshResource& GetMeshResource() const { return *MeshResources.get(); }

        void SetMeshResource(TUniquePtr<FMeshResource>&& NewResource);
        
        FORCEINLINE const FMeshResource::FMeshBuffers& GetMeshBuffers() const { return MeshResources->MeshBuffers; }
        
        virtual bool IsSkinned() const { return false; }
        
        FUNCTION()
        FORCEINLINE const FAABB& GetAABB() const { return BoundingBox; }
        
        template<typename TCallable>
        void ForEachSurface(TCallable&& Lambda) const;


        
        
        PROPERTY(Editable, NoResize, NoReorder, Category = "Materials")
        TVector<TObjectPtr<CMaterialInterface>> Materials;

        PROPERTY(Category = "AABB")
        FAABB BoundingBox;

        /** Build inputs for this mesh's signed distance field. Edited here rather than only at import so
         *  a field can be added, retuned, or dropped without round-tripping the source file; the mesh
         *  editor's Build Distance Field action applies them. Seeded from the import dialog. */
        PROPERTY(Editable, Category = "Distance Field")
        SDistanceFieldBuildSettings DistanceFieldSettings;

        /** File this mesh was last imported from, so "Reimport From File..." can open on it. Empty for
         *  procedurally generated meshes and for assets imported before this was recorded. Matches the
         *  SourcePath that CTexture / CFont / CAudioStream already keep. */
        PROPERTY()
        FString SourcePath;
        
    private:
        
        TUniquePtr<FMeshResource> MeshResources;
    };


    
    template <typename TCallable>
    void CMesh::ForEachSurface(TCallable&& Lambda) const
    {
        uint32 Count = 0;
        for (const FGeometrySurface& Surface : MeshResources->GeometrySurfaces)
        {
            std::forward<TCallable>(Lambda)(Surface, Count);
            ++Count;
        }
    }
}
