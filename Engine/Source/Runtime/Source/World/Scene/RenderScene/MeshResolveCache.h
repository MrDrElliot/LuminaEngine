#pragma once

#include "Renderer/ShaderHandle.h"
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
        FShaderH PixelShader = {};
        FShaderH VertexShader = {};
        FShaderH MeshShaderShadow = {};
        FShaderH MeshShaderBase = {};
        FShaderH VisBufferMeshShader = {};
        FShaderH VisBufferMeshShaderMasked = {};
        FShaderH MaskedVisBufferPixelShader = {};
        FShaderH DeferredShader = {};
        FShaderH MomentPixelShader = {};

        FDrawBatchKey   BatchKey    = {};

        EInstanceFlags  MaterialFlags = EInstanceFlags::None;
        uint64          MaterialID    = 0;
        uint16          MaterialIdx   = 0;
        bool            bMaterialCastsShadows = true;

        uint32  NumLODs                             = 1;
        uint32  LODMeshletOffset[MAX_MESH_LODS]     = {};
        uint32  LODMeshletCount[MAX_MESH_LODS]      = {};
        float   LODScreenThresholdSq[MAX_MESH_LODS] = {};

        // Mesh-local world size of one UV tile; 0 = unknown. Carried through to FSurfaceBinding for the
        // texture streamer. See FGeometrySurface::TexelFactor.
        float   TexelFactor                         = 0.0f;

        // What this resolve was taken FROM, so it can report its own staleness. The seven FShaderEntry*
        // above are pointers into a content-keyed library: a recompile that changes bytecode mints a NEW
        // entry, leaving these silently pointing at superseded code. They stay pointers because
        // FDrawBatchKey is keyed on them -- that is what lets material instances compiling to identical
        // SPIR-V share one batch and one draw -- so freshness is carried alongside instead.
        CMaterialInterface* SourceMaterial        = nullptr;
        uint32              SourceShaderRevision  = 0;
        // Stamped alongside the pointer because a destroyed CObject's ADDRESS can be handed to a new one,
        // and IsValid() cannot tell the difference. Without this, a recycled slot would be read for a
        // revision belonging to an unrelated material -- a silent wrong answer either way it lands.
        FGuid               SourceMaterialGuid    = {};
    };

    namespace MeshResolve
    {
        /** Records what Surface was resolved FROM, so it can later report its own staleness. Paired with
         *  IsSurfaceStale -- the two must agree on the key, so neither open-codes it. */
        RUNTIME_API void StampSurfaceSource(FResolvedSurface& Surface, CMaterialInterface* RawMaterial);

        /** True when Surface's cached shader entries can no longer be trusted because its source material
         *  has recompiled, or been destroyed and its address reused. Cheap: one pointer chase, a GUID
         *  compare and a uint compare -- no resolve. */
        RUNTIME_API bool IsSurfaceStale(const FResolvedSurface& Surface);

        RUNTIME_API bool ResolveSurfaceMaterial(FResolvedSurface& Out, CMaterialInterface* RawMaterial);
    }

    // Interned per (mesh, material assignment), so every instance of a mesh shares one entry.
    struct FResolvedMesh
    {
        TVector<FResolvedSurface>   Surfaces;
        
        FVector3                    LocalCenter;
        float                       LocalRadius = 0.0f;

        uint32                      MeshletHeaderSlot = 0;
        
        const void*                 MeshKey = nullptr;
        FGuid                       MeshGuid;
        TVector<const void*>        OverrideKey;

        uint32                      Generation = 0;

        TVector<const void*>        Dependencies;

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

        FORCEINLINE uint32 GetEntryState(uint32 Handle) const
        {
            return Handle < (uint32)EntryStates.size() ? EntryStates[Handle] : MESH_RESOLVE_STATE_STALE;
        }

        FORCEINLINE uint32 GetTableGeneration() const { return TableGeneration; }

        // Stamp each component compares against; a mismatch means "re-resolve me".
        static FORCEINLINE uint32 GetEpoch() { return Epoch.load(std::memory_order_acquire); }

        static void BumpEpoch();

        static void InvalidateDependency(const void* Asset);

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
