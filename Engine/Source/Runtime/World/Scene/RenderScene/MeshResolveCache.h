#pragma once
#include "Containers/Array.h"
#include "Core/Math/AABB.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/MeshData.h"
#include "Renderer/RHIFwd.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include <atomic>

namespace Lumina
{
    class CMesh;
    class CMaterialInterface;

    constexpr uint32 INVALID_MESH_RESOLVE_HANDLE = ~0u;

    // Shader entries are FShaderLibrary-owned and process-immortal, so holding them is safe.
    struct FResolvedSurface
    {
        const FShaderEntry* VertexShader                   = nullptr;
        const FShaderEntry* PixelShader                    = nullptr;
        const FShaderEntry* MeshShader                     = nullptr;
        const FShaderEntry* VisBufferMeshShader            = nullptr;
        const FShaderEntry* VisBufferVertexShader          = nullptr;
        const FShaderEntry* MaskedVisBufferPixelShader     = nullptr;
        const FShaderEntry* MaskedVisBufferPixelShaderPrim = nullptr;
        const FShaderEntry* DeferredShader                 = nullptr;

        FDrawBatchKey   BatchKey    = {};
        FDrawKey        DrawKey     = {};

        EInstanceFlags  MaterialFlags = EInstanceFlags::None;
        uint64          MaterialID    = 0;
        uint16          MaterialIdx   = 0;
        bool            bMaterialCastsShadows = true;

        uint32  NumLODs                             = 1;
        uint32  LODMeshletOffset[MAX_MESH_LODS]     = {};
        uint32  LODMeshletCount[MAX_MESH_LODS]      = {};
        // NOTE: do not cache meshlet vertex extents here. A mesh can resolve while its surfaces are
        // populated but MeshletData is not yet resident, and the zero would be cached for the entity's
        // lifetime. Pre-skin sizing reads the live meshlet table at extract instead.
        // Squared so LOD selection compares DistSq against Threshold^2 * RadiusSq, with no per-entity sqrt.
        float   LODScreenThresholdSq[MAX_MESH_LODS] = {};
    };

    // Interned per (mesh, material assignment), so every instance of a mesh shares one entry.
    struct FResolvedMesh
    {
        TVector<FResolvedSurface>   Surfaces;

        // Local bounding sphere rather than the AABB: transforming a sphere is cheaper than rebuilding a
        // world AABB, and stays tight under rotation instead of inflating by up to sqrt(3).
        FVector3                    LocalCenter;
        float                       LocalRadius = 0.0f;

        uint64                      MeshletHeaderAddress = 0;

        // Re-checked on hash hit so a collision can't return another mesh's materials.
        const void*                 MeshKey = nullptr;
        TVector<const void*>        OverrideKey;

        // False while a slot's material is still compiling.
        bool                        bAllMaterialsReady = false;

        // The single "this entry is finished" test: mesh data loaded, GPU meshlet header built, and every
        // material ready. Anything less is a transient state, so the owner keeps re-resolving. Materials
        // alone are not enough -- an unloaded mesh has no surfaces, so the material loop trivially passes
        // and would otherwise cache an empty entry as complete, forever.
        bool                        bResolved = false;
    };

    // Resolves the material/geometry data the mesh gather needs, once per change instead of per frame.
    class RUNTIME_API FMeshResolveCache
    {
    public:

        static FMeshResolveCache& Get();

        // Game thread only; mutates the table.
        uint32 Resolve(CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);

        FORCEINLINE const FResolvedMesh& GetEntry(uint32 Handle) const { return *Entries[Handle]; }
        FORCEINLINE bool IsValidHandle(uint32 Handle) const { return Handle < (uint32)Entries.size(); }

        // Stamp each component compares against; a mismatch means "re-resolve me".
        static FORCEINLINE uint32 GetEpoch() { return Epoch.load(std::memory_order_acquire); }

        // Invalidates every cached resolve; entries re-resolve lazily, once each.
        static void BumpEpoch();

        // Bumped whenever anything needs (re-)resolving. This is a generation counter and NOT a flag a
        // consumer may clear: the cache is process-wide but the resolve pre-pass is per render scene, and
        // there is one scene per world -- the level editor plus a world for every open asset editor,
        // thumbnail capture and PIE session. A shared bool was consumed by whichever scene ticked first,
        // so every other world silently skipped its pre-pass and its components kept an invalid handle.
        // Each scene compares this against its own last-seen value, so no scene can consume another's work.
        static FORCEINLINE uint32 GetPendingGeneration() { return PendingGeneration.load(std::memory_order_acquire); }
        static FORCEINLINE void MarkPendingWork() { PendingGeneration.fetch_add(1, std::memory_order_acq_rel); }

        // Only safe between frames; handles held by components go stale.
        void Flush();

        ~FMeshResolveCache() { Flush(); }

    private:

        static uint64 HashKey(const void* Mesh, const TVector<CMaterialInterface*>& Overrides);
        void ResolveSurfaces(FResolvedMesh& Out, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);

        // Handles are indices; entries are boxed so growth never moves one a frame holds a reference to.
        TVector<FResolvedMesh*>             Entries;
        THashMap<uint64, TVector<uint32>>   HandlesByHash;

        static std::atomic<uint32>  Epoch;
        static std::atomic<uint32>  PendingGeneration;
    };
}
