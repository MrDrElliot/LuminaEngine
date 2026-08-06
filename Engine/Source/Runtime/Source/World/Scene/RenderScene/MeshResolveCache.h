#pragma once
#include <atomic>
#include <mutex>
#include "Containers/Array.h"
#include "Core/Math/AABB.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/MeshData.h"
#include "Renderer/RHIFwd.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    class CMesh;
    class CMaterialInterface;

    // Shader entries are FShaderLibrary-owned and process-immortal, so holding them is safe.
    struct FResolvedSurface
    {
        const FShaderEntry* VertexShader                   = nullptr;
        const FShaderEntry* PixelShader                    = nullptr;
        const FShaderEntry* MeshShader                     = nullptr;
        const FShaderEntry* VisBufferMeshShader            = nullptr;
        const FShaderEntry* VisBufferMeshShaderMasked      = nullptr;
        const FShaderEntry* VisBufferVertexShader          = nullptr;
        const FShaderEntry* MaskedVisBufferPixelShader     = nullptr;
        const FShaderEntry* MaskedVisBufferPixelShaderPrim = nullptr;
        const FShaderEntry* DeferredShader                 = nullptr;

        FDrawBatchKey   BatchKey    = {};

        EInstanceFlags  MaterialFlags = EInstanceFlags::None;
        uint64          MaterialID    = 0;
        uint16          MaterialIdx   = 0;
        bool            bMaterialCastsShadows = true;

        uint32  NumLODs                             = 1;
        uint32  LODMeshletOffset[MAX_MESH_LODS]     = {};
        uint32  LODMeshletCount[MAX_MESH_LODS]      = {};
        float   LODScreenThresholdSq[MAX_MESH_LODS] = {};
    };

    namespace MeshResolve
    {
        RUNTIME_API bool ResolveSurfaceMaterial(FResolvedSurface& Out, CMaterialInterface* RawMaterial);
    }

    // Interned per (mesh, material assignment), so every instance of a mesh shares one entry.
    struct FResolvedMesh
    {
        TVector<FResolvedSurface>   Surfaces;
        
        FVector3                    LocalCenter;
        float                       LocalRadius = 0.0f;

        uint64                      MeshletHeaderAddress = 0;
        
        const void*                 MeshKey = nullptr;
        FGuid                       MeshGuid;
        TVector<const void*>        OverrideKey;

        // Bumped every time Surfaces is rebuilt. Consumers that cache anything derived from a surface
        // (the render scene's batch/draw bindings) compare this instead of the surface addresses, which
        // move when the vector reallocates.
        uint32                      Generation = 0;

        /**
         * Assets whose change makes this entry wrong: the mesh, plus every material each surface resolved
         * against -- the AUTHORED one as well as the concrete master it fell back to while that was still
         * compiling. Registered in FMeshResolveCache::HandlesByDependency.
         *
         * An entry is interned by (mesh, override list), so a change to the MESH's own material slots, or
         * to a material asset itself, leaves the key identical while making the contents wrong. This is
         * what lets that be detected by asset instead of by a global epoch: "this mesh finished uploading"
         * now invalidates the handful of entries using it rather than the entire table.
         */
        TVector<const void*>        Dependencies;

        // Set when a dependency changed; the next Resolve() of this entry rebuilds Surfaces. Cleared by
        // the rebuild, so N instances of one mesh cost one rebuild rather than N.
        bool                        bNeedsResolve = true;

        // False while a slot's material is still compiling.
        bool                        bAllMaterialsReady = false;
        bool                        bResolved = false;
    };

    // Resolves the material/geometry data the mesh gather needs, once per change instead of per frame.
    class RUNTIME_API FMeshResolveCache
    {
    public:

        static FMeshResolveCache& Get();
        
        FMeshResolveCache() = default;
        FMeshResolveCache(const FMeshResolveCache&) = default;
        FMeshResolveCache(FMeshResolveCache&&) = default;
        FMeshResolveCache& operator = (const FMeshResolveCache&) = default;
        FMeshResolveCache& operator = (FMeshResolveCache&&) = default;
        ~FMeshResolveCache() { Flush(); }


        // Game thread only; mutates the table.
        uint32 Resolve(CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);

        FORCEINLINE const FResolvedMesh& GetEntry(uint32 Handle) const { return *Entries[Handle]; }
        FORCEINLINE bool IsValidHandle(uint32 Handle) const { return Handle < (uint32)Entries.size(); }
        // Handles are dense indices, so this doubles as the size of any per-handle side table.
        FORCEINLINE uint32 NumEntries() const { return (uint32)Entries.size(); }

        /**
         * Dense mirror of each entry's staleness token, indexed by handle.
         *
         * Entries are heap-boxed, so reading Generation off one is a dependent load into cold memory --
         * and the staleness gate this serves runs once per mesh component per frame. Mirrored here it is
         * one indexed load from an array sized by DISTINCT MESHES, which stays resident.
         */
        FORCEINLINE uint32 GetEntryState(uint32 Handle) const
        {
            return Handle < (uint32)EntryStates.size() ? EntryStates[Handle] : MESH_RESOLVE_STATE_STALE;
        }

        /**
         * Bumped only when an EXISTING entry is rebuilt.
         *
         * Interning a brand new mesh does not move it, because a new entry cannot invalidate anything that
         * was already resolved. That distinction is the whole point: it is what keeps "a mesh was added"
         * off the primitive set's O(primitives) resolve sweep.
         */
        FORCEINLINE uint32 GetTableGeneration() const { return TableGeneration; }

        // Stamp each component compares against; a mismatch means "re-resolve me".
        static FORCEINLINE uint32 GetEpoch() { return Epoch.load(std::memory_order_acquire); }

        /**
         * Invalidates EVERY cached resolve. The nuclear option -- it re-resolves every mesh component in
         * every world and re-binds every primitive drawing them, so it belongs to editor-wide events
         * (an asset saved from a tool) and nothing else. Prefer InvalidateDependency.
         */
        static void BumpEpoch();

        /**
         * Marks every entry that resolved against this asset for rebuild. Any thread: the key is queued
         * and applied by ApplyPendingInvalidations on the game thread, because the table itself is not
         * thread-safe and asset GPU uploads do not run on the game thread.
         *
         * Cost is O(entries using the asset) -- typically one -- instead of BumpEpoch's O(whole scene).
         */
        static void InvalidateDependency(const void* Asset);

        // Game thread, once at the top of the resolve pass. Applies queued InvalidateDependency keys, any
        // BumpEpoch that landed since the last call, and re-arms entries that could not finish resolving.
        void ApplyPendingInvalidations();

        static FORCEINLINE uint32 GetPendingGeneration() { return PendingGeneration.load(std::memory_order_acquire); }
        static FORCEINLINE void MarkPendingWork() { PendingGeneration.fetch_add(1, std::memory_order_acq_rel); }

        // Only safe between frames; handles held by components go stale.
        void Flush();

    private:

        static uint64 HashKey(const void* Mesh, const TVector<CMaterialInterface*>& Overrides);
        void ResolveSurfaces(FResolvedMesh& Out, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);

        // Rebuilds one entry in place and republishes its dependency registration and staleness token.
        void RebuildEntry(uint32 Handle, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides);
        void RegisterDependencies(uint32 Handle, const FResolvedMesh& Entry);
        void UnregisterDependencies(uint32 Handle, const FResolvedMesh& Entry);
        void MarkEntryStale(uint32 Handle);

        // Handles are indices; entries are boxed so growth never moves one a frame holds a reference to.
        TVector<FResolvedMesh*>             Entries;
        THashMap<uint64, TVector<uint32>>   HandlesByHash;

        // Parallel to Entries. See GetEntryState.
        TVector<uint32>                     EntryStates;

        // Asset -> the entries that resolved against it. Buckets hold one handle per distinct material
        // assignment of one mesh, so they stay in the low single digits.
        THashMap<const void*, TVector<uint32>> HandlesByDependency;

        uint32                      TableGeneration = 1;
        uint32                      AppliedEpoch = 0;

        static std::atomic<uint32>  Epoch;
        static std::atomic<uint32>  PendingGeneration;

        // Guards PendingInvalidations only. Contended only by asset loads, which are rare next to frames.
        static std::mutex           PendingMutex;
        static TVector<const void*> PendingInvalidations;
    };
}
