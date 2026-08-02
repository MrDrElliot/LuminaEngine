#pragma once
#include <atomic>
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

        // Epoch this entry's Surfaces were built at. An entry is interned by (mesh, override list), so a
        // change to the MESH's own material slots -- or to a material asset itself -- leaves the key
        // identical while making the contents wrong. Comparing this against the global epoch is what
        // turns BumpEpoch into an actual invalidation instead of just a per-component re-lookup.
        uint32                      ResolvedEpoch = ~0u;

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

        // Stamp each component compares against; a mismatch means "re-resolve me".
        static FORCEINLINE uint32 GetEpoch() { return Epoch.load(std::memory_order_acquire); }

        // Invalidates every cached resolve; entries re-resolve lazily, once each.
        static void BumpEpoch();
        
        static FORCEINLINE uint32 GetPendingGeneration() { return PendingGeneration.load(std::memory_order_acquire); }
        static FORCEINLINE void MarkPendingWork() { PendingGeneration.fetch_add(1, std::memory_order_acq_rel); }

        // Only safe between frames; handles held by components go stale.
        void Flush();
    
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
