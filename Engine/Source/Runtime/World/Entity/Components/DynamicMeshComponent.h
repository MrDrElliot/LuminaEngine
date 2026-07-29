#pragma once


#include "MeshComponent.h"
#include "Core/Math/AABB.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Renderer/MeshData.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "DynamicMeshComponent.generated.h"

namespace Lumina
{
    struct FDynamicMeshBuildData;
    
    struct FDynamicMeshRenderData
    {
        FMeshResource               Resource;
        TVector<FResolvedSurface>   Surfaces;

        FVector3                    LocalCenter          = FVector3(0.0f);
        float                       LocalRadius          = 0.0f;
        uint64                      MeshletHeaderAddress = 0;

        // Materials can still be compiling when Commit runs; the resolve pass re-runs the material half
        // until they settle, exactly as the asset path does.
        bool                        bAllMaterialsReady   = false;
    };

    // A mesh built entirely from data at runtime (from C# or C++) rather than loaded from an asset.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API CACHE_ALIGN SDynamicMeshComponent : SMeshComponent
    {
        GENERATED_BODY()

        // The render path resolves materials through this (override beats the built mesh's slot).
        CMaterialInterface* GetMaterialForSlot(size_t Slot) const;

        /** World-local bounds of the committed mesh (empty until the first Commit). */
        FUNCTION(Script)
        FAABB GetAABB() const;

        /** Declare a sub-range of the index buffer that draws with one material slot. Optional: with no
         *  sections, Commit() makes a single section covering every index on slot 0. */
        FUNCTION(Script)
        void AddSection(int32 MaterialSlot, int32 StartIndex, int32 IndexCount);

        /** Drop all staged data and the built mesh, returning the component to an empty state. */
        FUNCTION(Script)
        void ClearMesh();

        /** Finalize the staged data: generate meshlets/LODs and upload the GPU buffers. Returns false if
         *  there is nothing renderable (no positions or no indices). Call after setting the streams. */
        FUNCTION(Script)
        bool Commit();

        /** True once Commit() has produced a renderable mesh. */
        FUNCTION(Script)
        bool IsBuilt() const;

        /** Number of vertices in the staged (pre-Commit) or committed mesh. */
        FUNCTION(Script)
        int32 GetVertexCount() const;

        /** Number of triangles in the staged (pre-Commit) or committed mesh. */
        FUNCTION(Script)
        int32 GetTriangleCount() const;

        // Bulk stream setters (called by the C# span exports in DotNetDynamicMesh.cpp; not script-bound
        // directly because member functions can't take spans). Counts are element counts: positions/normals
        // are 3 floats/vertex, UVs 2, colors 4 (float) or 1 (packed RGBA8), indices 1.
        void SetPositionsData(const float* Data, int32 FloatCount);
        void SetNormalsData(const float* Data, int32 FloatCount);
        void SetUVsData(const float* Data, int32 FloatCount);
        void SetColorsFloatData(const float* Data, int32 FloatCount);
        void SetColorsPackedData(const uint32* Data, int32 Count);
        void SetIndicesData(const uint32* Data, int32 Count);

        /** How many LOD levels Commit() builds, 1 meaning LOD 0 only. Each extra level is another full
         *  simplify pass over the whole index range, so a mesh that is rebuilt often (voxel chunks,
         *  procedural terrain) usually wants 1-2; geometry that is built once and viewed at range wants
         *  more. Clamped to MAX_MESH_LODS. */
        PROPERTY(Editable, Category = "Rendering")
        int32 MaxLODs = (int32)MAX_MESH_LODS;

        /** Generate proper MikkTSpace tangents at Commit. This is the slowest single stage of a commit and
         *  it does not parallelize across a mesh with one section, so turning it off is the biggest saving
         *  available for procedural geometry. Off substitutes a cheap arbitrary tangent basis: shading stays
         *  valid, but any material sampling a normal map will orient it arbitrarily. Leave on if this mesh
         *  uses normal maps. */
        PROPERTY(Editable, Category = "Rendering")
        bool bGenerateTangents = true;

        /// Committed geometry + resolved surfaces. Null until the first successful Commit.
        TSharedPtr<FDynamicMeshRenderData> RenderData;

        /// Re-runs the material half of the resolve against the current MaterialOverrides. Called by the
        /// render scene's resolve pass while a material is still compiling, and after an override changes.
        void RefreshResolvedMaterials();

    private:

        FDynamicMeshBuildData& EnsureBuildData();

        TSharedPtr<FDynamicMeshBuildData> BuildData;

        // Cached at Commit so the count getters stay valid after the CPU scratch streams are dropped on upload.
        int32 CommittedVertexCount   = 0;
        int32 CommittedTriangleCount = 0;
    };
}
