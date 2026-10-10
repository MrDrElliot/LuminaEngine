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
    // One material-tagged slice of the index buffer.
    struct FDynamicMeshSection
    {
        int32 MaterialSlot = 0;
        int32 StartIndex   = 0;
        int32 IndexCount   = 0;
    };

    // CPU-side streams for a mesh built from data, consumed by BuildMeshResource.
    struct FDynamicMeshBuildData
    {
        TVector<FVector3>            Positions;
        TVector<FVector3>            Normals;   // unpacked; octahedral-packed when the resource is built
        TVector<FVector2>            UVs;
        TVector<uint32>              Colors;    // RGBA8 packed
        TVector<uint32>              Indices;
        TVector<FDynamicMeshSection> Sections;
    };

    struct FMeshBuildOptions
    {
        uint32 MaxLODs             = 1;
        bool   bGenerateTangents   = false;
        bool   bMeshletConeCulling = false;
        bool   bOptimizeMeshlets   = true;
        bool   bFastMeshletBuild   = true;
    };

    // Packs the streams into a resource ready for meshlet generation, deriving normals when absent. Consumes positions, indices and colors.
    RUNTIME_API TUniquePtr<FMeshResource> BuildMeshResource(FDynamicMeshBuildData& Data, const FMeshBuildOptions& Options);
    
    struct FDynamicMeshRenderData
    {
        FMeshResource               Resource;
        TVector<FResolvedSurface>   Surfaces;

        // Tight local box, kept alongside the sphere rather than derived from it: the sphere circumscribes
        // the box, so rebuilding a box from the radius inflates every axis by up to sqrt(3).
        FVector3                    LocalMin             = FVector3(0.0f);
        FVector3                    LocalMax             = FVector3(0.0f);

        FVector3                    LocalCenter          = FVector3(0.0f);
        float                       LocalRadius          = 0.0f;
        uint32                      MeshletHeaderSlot = 0;

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
        FUNCTION()
        CMaterialInterface* GetMaterialForSlot(uint32 Slot) const;

        /** Installs a transient instance over this slot so its parameters diverge for this component only. */
        FUNCTION()
        CMaterialInstance* CreateDynamicMaterialInstance(uint32 Slot);

        /** World-local bounds of the committed mesh (empty until the first Commit). */
        FUNCTION()
        FAABB GetAABB() const;

        /** Declare a sub-range of the index buffer that draws with one material slot. Optional: with no
         *  sections, Commit() makes a single section covering every index on slot 0. */
        FUNCTION()
        void AddSection(int32 MaterialSlot, int32 StartIndex, int32 IndexCount);

        /** Drop all staged data and the built mesh, returning the component to an empty state. */
        FUNCTION()
        void ClearMesh();

        /** Finalize the staged data: generate meshlets/LODs and upload the GPU buffers. Returns false if
         *  there is nothing renderable (no positions or no indices). Call after setting the streams.
         *
         *  Callable from any worker thread, and concurrently across DIFFERENT components -- which is the
         *  point, since this is the expensive call and chunked geometry commits many of them at once. The
         *  build stages are component-local, the nested ParallelFor re-enters the scheduler safely, and the
         *  RHI alloc/upload path is already built for multi-threaded submission.
         *
         *  A single component is still owned by ONE thread at a time: the stream setters, AddSection and
         *  ClearMesh are unsynchronized, so staging and committing the same component from two threads is
         *  a data race. What IS synchronized is the handoff to the renderer -- see PublishRenderData. */
        FUNCTION()
        bool Commit();

        // Commit's build, run at once whatever batch is open; the batch itself calls this for every pending commit.
        bool CommitNow();

        // Set by a Commit made while a mesh build batch was open, and cleared when the batch builds it.
        bool bCommitPending = false;

        /** True once Commit() has produced a renderable mesh. */
        FUNCTION()
        bool IsBuilt() const;

        /** Number of vertices in the staged (pre-Commit) or committed mesh. */
        FUNCTION()
        int32 GetVertexCount() const;

        /** Number of triangles in the staged (pre-Commit) or committed mesh. */
        FUNCTION()
        int32 GetTriangleCount() const;

        // Bulk stream setters. Counts are element counts, so 3 floats per vertex for positions and
        // normals, 2 for UVs, 4 or 1 for float and packed colors, and 1 for an index.

        FUNCTION()
        void SetPositionsData(const float* Data, int32 FloatCount);

        FUNCTION()
        void SetNormalsData(const float* Data, int32 FloatCount);

        FUNCTION()
        void SetUVsData(const float* Data, int32 FloatCount);

        FUNCTION()
        void SetColorsFloatData(const float* Data, int32 FloatCount);

        FUNCTION()
        void SetColorsPackedData(const uint32* Data, int32 Count);

        FUNCTION()
        void SetIndicesData(const uint32* Data, int32 Count);

        // Takes every stream and section without copying, replacing whatever was staged.
        void SetBuildData(FDynamicMeshBuildData&& Data);

        /** How many LOD levels Commit() builds, 1 meaning LOD 0 only. Each extra level is another full
         *  meshopt_simplify pass over the WHOLE LOD-0 index range (levels are not cascaded), plus a
         *  full-size scratch allocation, so cost is roughly linear in this. Clamped to MAX_MESH_LODS.
         *
         *  Defaults to 1 because this component exists for geometry that is rebuilt often, where the build
         *  lands on the frame that rebuilds it and the LOD ramp only pays off for meshes also viewed at
         *  range. Raise it for procedural geometry that is built once and seen from far away. */
        PROPERTY(Editable, Category = "Rendering")
        int32 MaxLODs = 1;

        /** Build per-meshlet normal cones so the GPU can cull backfacing clusters. Costs twice at build
         *  time: it selects meshopt's cone-weighted clustering (the expensive mode) and adds a bounds
         *  solve per meshlet. Off, the culling sphere comes from the per-meshlet AABB pass that runs
         *  anyway, and the meshlet is tagged as having no cone -- frustum and occlusion culling are
         *  unaffected, only backface-cluster rejection is given up.
         *
         *  Defaults off here (imported assets keep it on): they build once, this rebuilds constantly, and
         *  the saving is largest on the near-planar geometry -- terrain, voxel surfaces -- that cone
         *  culling helps least anyway. Turn it on for closed, high-curvature procedural meshes. */
        PROPERTY(Editable, Category = "Rendering")
        bool bMeshletConeCulling = false;

        /** Reorder each meshlet's triangles for the hardware vertex cache. Cheap next to the cone solve
         *  and it helps every draw, so it stays on by default; exposed for rebuild-bound meshes that want
         *  the last of the build cost back. */
        PROPERTY(Editable, Category = "Rendering")
        bool bOptimizeMeshlets = true;

        // Scans a cache-optimized index buffer into meshlets instead of clustering them; looser bounds cull less.
        PROPERTY(Editable, Category = "Rendering")
        bool bFastMeshletBuild = true;

        // Real UV-space tangents at Commit; off substitutes a cheap basis that only normal maps care about.
        PROPERTY(Editable, Category = "Rendering")
        bool bGenerateTangents = true;

        /** Keeps the CPU meshlet streams after Commit uploads them, which a dynamic mesh collider builds from; turn it off for a render-only mesh. */
        PROPERTY(Editable, Category = "Rendering")
        bool bKeepCPUMeshletData = true;

        /// Takes a ref on the currently published data (null until the first successful Commit). Every
        /// reader outside this component goes through this rather than touching the pointer: Commit can
        /// swap it from a worker, and copying a shared_ptr while another thread reassigns that same
        /// shared_ptr object is a data race, not a ref-count that happens to work out.
        ///
        /// The returned data is immutable from the renderer's side and outlives the swap, so a gather can
        /// hold it across a commit. Commit finishes and drops scratch BEFORE publishing for that reason.
        TSharedPtr<FDynamicMeshRenderData> LoadRenderData() const;

        /// Acquire-load of RenderDataVersion, paired with the release in PublishRenderData: a reader that
        /// observes a new version is guaranteed to observe the pointer that goes with it.
        uint32 LoadRenderDataVersion() const;

        /// Atomically swaps in fully-built data and releases the version bump. Null clears.
        void PublishRenderData(TSharedPtr<FDynamicMeshRenderData> NewData);

        /// Bumped every time RenderData is replaced or cleared.
        ///
        /// This component has no registry access, so it cannot signal the render scene's dirty channel --
        /// it gets POLLED instead, once per component per frame. That poll used to hash the entity into the
        /// primitive table and compare the stored surface pointer, which is a hash probe plus two random
        /// memory accesses for every dynamic mesh in the world, every frame, whether or not anything moved.
        /// Comparing these two adjacent fields instead keeps the poll a dense sequential scan.
        ///
        /// A counter rather than the old pointer compare also closes a real hole: the allocator can hand a
        /// fresh FDynamicMeshRenderData back at the address the previous one just freed, and the pointer
        /// compare would read that as "unchanged". Transient, never serialized.
        uint32 RenderDataVersion = 0;

        /// Last RenderDataVersion the render scene's primitive sync observed. Owned by that pass, not by
        /// this component. Equal to RenderDataVersion means "the scene is up to date with this mesh".
        /// Transient, never serialized.
        uint32 SyncedRenderDataVersion = 0;

        // Last RenderDataVersion whose materials the resolve pass examined. Transient, never serialized.
        uint32 ResolvedRenderDataVersion = 0;

        /// Re-runs the material half of the resolve against the current MaterialOverrides. Called by the
        /// render scene's resolve pass while a material is still compiling, and after an override changes.
        void RefreshResolvedMaterials();

    private:

        FDynamicMeshBuildData& EnsureBuildData();

        /// Resolve the material half of Data's surfaces against the current overrides. Takes the data
        /// rather than reading the member so Commit can run it on the not-yet-published snapshot.
        void ResolveMaterialsInto(FDynamicMeshRenderData& Data) const;

        /// Private: reachable only through LoadRenderData / PublishRenderData, which is what keeps the
        /// cross-thread accesses atomic. Nothing may mutate it once published.
        TSharedPtr<FDynamicMeshRenderData> RenderData;

        TSharedPtr<FDynamicMeshBuildData> BuildData;

        // Cached at Commit so the count getters stay valid after the CPU scratch streams are dropped on upload.
        int32 CommittedVertexCount   = 0;
        int32 CommittedTriangleCount = 0;
    };
}
