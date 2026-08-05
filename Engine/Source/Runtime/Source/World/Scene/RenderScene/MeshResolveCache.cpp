#include "RuntimePCH.h"
#include "MeshResolveCache.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"

namespace Lumina
{
    std::atomic<uint32> FMeshResolveCache::Epoch{1};
    std::atomic<uint32> FMeshResolveCache::PendingGeneration{1};
    std::mutex           FMeshResolveCache::PendingMutex;
    TVector<const void*> FMeshResolveCache::PendingInvalidations;

    FMeshResolveCache& FMeshResolveCache::Get()
    {
        static FMeshResolveCache Instance;
        return Instance;
    }

    void FMeshResolveCache::BumpEpoch()
    {
        Epoch.fetch_add(1, std::memory_order_acq_rel);
        MarkPendingWork();
    }

    void FMeshResolveCache::InvalidateDependency(const void* Asset)
    {
        if (Asset == nullptr)
        {
            return;
        }

        {
            FScopeLock Lock(PendingMutex);
            PendingInvalidations.push_back(Asset);
        }

        // The resolve pass early-outs unless this moved, so queueing without it would leave the
        // invalidation sitting there until something unrelated happened to wake the pass up.
        MarkPendingWork();
    }

    void FMeshResolveCache::MarkEntryStale(uint32 Handle)
    {
        if (Handle >= (uint32)Entries.size())
        {
            return;
        }
        Entries[Handle]->bNeedsResolve = true;
        EntryStates[Handle]            = MESH_RESOLVE_STATE_STALE;
    }

    void FMeshResolveCache::RegisterDependencies(uint32 Handle, const FResolvedMesh& Entry)
    {
        for (const void* Dependency : Entry.Dependencies)
        {
            HandlesByDependency[Dependency].push_back(Handle);
        }
    }

    void FMeshResolveCache::UnregisterDependencies(uint32 Handle, const FResolvedMesh& Entry)
    {
        for (const void* Dependency : Entry.Dependencies)
        {
            auto It = HandlesByDependency.find(Dependency);
            if (It == HandlesByDependency.end())
            {
                continue;
            }

            TVector<uint32>& Bucket = It->second;
            for (SIZE_T i = 0; i < Bucket.size(); ++i)
            {
                if (Bucket[i] == Handle)
                {
                    Bucket[i] = Bucket.back();
                    Bucket.pop_back();
                    break;
                }
            }
        }
    }

    void FMeshResolveCache::RebuildEntry(uint32 Handle, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        FResolvedMesh& Entry = *Entries[Handle];

        // The dependency set is rebuilt from scratch below, and a rebuild can legitimately change it -- a
        // material that finished compiling stops falling back to the default one, so the default drops out.
        UnregisterDependencies(Handle, Entry);
        ResolveSurfaces(Entry, Mesh, Overrides);
        Entry.bNeedsResolve = false;
        RegisterDependencies(Handle, Entry);

        EntryStates[Handle] = Entry.bResolved ? (Entry.Generation << 1) : MESH_RESOLVE_STATE_STALE;

        if (!Entry.bResolved)
        {
            MarkPendingWork();
        }
    }

    void FMeshResolveCache::ApplyPendingInvalidations()
    {
        TVector<const void*> Keys;
        {
            FScopeLock Lock(PendingMutex);
            Keys.swap(PendingInvalidations);
        }

        // BumpEpoch is applied here rather than inside itself so that it stays callable from any thread.
        const uint32 CurrentEpoch = GetEpoch();
        if (CurrentEpoch != AppliedEpoch)
        {
            AppliedEpoch = CurrentEpoch;
            for (uint32 Handle = 0, N = (uint32)Entries.size(); Handle < N; ++Handle)
            {
                MarkEntryStale(Handle);
            }
        }
        else
        {
            for (const void* Key : Keys)
            {
                auto It = HandlesByDependency.find(Key);
                if (It == HandlesByDependency.end())
                {
                    continue;
                }
                for (uint32 Handle : It->second)
                {
                    MarkEntryStale(Handle);
                }
            }
        }

        // An entry that could not finish -- GPU buffers not landed, a material still compiling -- retries
        // once per pass. Re-arming HERE rather than letting Resolve() test bResolved is what stops a
        // thousand instances of one unresolved mesh from each re-running the whole resolve every frame.
        for (uint32 Handle = 0, N = (uint32)Entries.size(); Handle < N; ++Handle)
        {
            if (!Entries[Handle]->bResolved)
            {
                MarkEntryStale(Handle);
            }
        }
    }

    uint64 FMeshResolveCache::HashKey(const void* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        size_t Seed = 0;
        Hash::HashCombine(Seed, (uint64)Mesh);
        for (CMaterialInterface* Override : Overrides)
        {
            Hash::HashCombine(Seed, (uint64)Override);
        }
        return (uint64)Seed;
    }

    void FMeshResolveCache::Flush()
    {
        for (FResolvedMesh* Entry : Entries)
        {
            Memory::Delete(Entry);
        }
        Entries.clear();
        EntryStates.clear();
        HandlesByHash.clear();
        HandlesByDependency.clear();
        ++TableGeneration;
    }

    uint32 FMeshResolveCache::Resolve(CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        if (!IsValid(Mesh))
        {
            return INVALID_MESH_RESOLVE_HANDLE;
        }

        const uint64 KeyHash = HashKey(Mesh, Overrides);

        auto It = HandlesByHash.find(KeyHash);
        if (It != HandlesByHash.end())
        {
            for (uint32 Handle : It->second)
            {
                const FResolvedMesh& Candidate = *Entries[Handle];

                if (Candidate.MeshKey != Mesh || Candidate.MeshGuid != Mesh->GetGUID()
                    || Candidate.OverrideKey.size() != Overrides.size())
                {
                    continue;
                }

                bool bSame = true;
                for (size_t i = 0; i < Overrides.size(); ++i)
                {
                    if (Candidate.OverrideKey[i] != (const void*)Overrides[i])
                    {
                        bSame = false;
                        break;
                    }
                }

                if (bSame)
                {
                    // One flag, set by ApplyPendingInvalidations. It has to be a flag rather than a
                    // condition tested here, because a resolved entry must not be frozen forever --
                    // assigning a mesh's default materials keeps the key identical, so every instance in
                    // every world would keep drawing the old assignment -- and it has to be CLEARED by the
                    // rebuild, or the thousand instances sharing this entry each pay for the same rebuild.
                    if (Candidate.bNeedsResolve)
                    {
                        RebuildEntry(Handle, Mesh, Overrides);

                        // Only an EXISTING entry that came out RESOLVED moves this. Consumers holding
                        // state derived from an entry sweep when it does, so two cases must stay off it:
                        // interning a newly added mesh (nothing was derived from it yet), and a failed
                        // attempt at one still streaming (nothing binds an unresolved entry, and those
                        // retry every frame -- which would put the sweep on every frame with them).
                        if (Entries[Handle]->bResolved)
                        {
                            ++TableGeneration;
                        }
                    }
                    return Handle;
                }
            }
        }

        const uint32 NewHandle = (uint32)Entries.size();
        Entries.push_back(Memory::New<FResolvedMesh>());
        EntryStates.push_back(MESH_RESOLVE_STATE_STALE);
        FResolvedMesh& Entry = *Entries.back();

        Entry.MeshKey  = Mesh;
        Entry.MeshGuid = Mesh->GetGUID();
        Entry.OverrideKey.reserve(Overrides.size());
        for (CMaterialInterface* Override : Overrides)
        {
            Entry.OverrideKey.push_back((const void*)Override);
        }

        // TableGeneration deliberately NOT bumped: nothing was resolved against this entry before now, so
        // no consumer holds state derived from it.
        RebuildEntry(NewHandle, Mesh, Overrides);

        HandlesByHash[KeyHash].push_back(NewHandle);
        return NewHandle;
    }

    namespace
    {
        // Shaders a surface must actually have to be VISIBLE, as opposed to merely compiled.
        //
        // Translucency takes the forward/WBOIT path and needs no deferred shader. Everything opaque
        // rasterizes into the VisBuffer and is then shaded by a tile pass that bins one slot per
        // distinct deferred shader -- and that binning SKIPS any material whose deferred shader is
        // null. So an opaque surface missing it rasterizes correctly, occupies the VisBuffer, and is
        // then never shaded: the geometry is "drawn" and completely invisible, indistinguishable from
        // a missing mesh. Same for the VisBuffer geometry shaders, one step earlier.
        bool HasRequiredPassShaders(CMaterialInterface* Material)
        {
            CMaterial* Concrete = IsValid(Material) ? Material->GetMaterial() : nullptr;
            if (Concrete == nullptr)
            {
                return false;
            }

            // Either geometry path is fine; the pass picks whichever is present.
            if (Concrete->GetVisBufferVertexShader() == nullptr && Concrete->GetVisBufferMeshShader() == nullptr)
            {
                return false;
            }

            const EBlendMode Blend = Material->GetBlendMode();
            if (Blend == EBlendMode::Translucent || Blend == EBlendMode::Additive)
            {
                return true;
            }

            return Concrete->GetDeferredShader() != nullptr;
        }
    }

    bool MeshResolve::ResolveSurfaceMaterial(FResolvedSurface& R, CMaterialInterface* RawMaterial)
    {
        bool bReady = true;

        CMaterialInterface* Material = RawMaterial;
        if (IsValid(Material) && Material->GetMaterialType() != EMaterialType::PBR)
        {
            Material = nullptr;
        }

        if (!IsValid(Material) || !IsValid(Material->GetMaterial()) || !Material->IsReadyForRender())
        {
            if (IsValid(RawMaterial) && !RawMaterial->IsReadyForRender())
            {
                bReady = false;
            }
            Material = CMaterial::GetDefaultMaterial();
        }

        // Second gate. IsReadyForRender reports that the material finished COMPILING, not that the
        // shaders the passes bind exist -- the two disagree while a material is part-way through, and a
        // surface cached in that window is resolved, "ready", and permanently invisible. Fall back to
        // the default material so the mesh at least draws, and report not-ready so the resolve retries
        // and swaps the real material in once its shaders land.
        if (!HasRequiredPassShaders(Material))
        {
            bReady   = false;
            Material = CMaterial::GetDefaultMaterial();
        }

        // Third gate, same shape as the second. A MASTER bound directly needs its own default textures,
        // which are soft and resolved on demand -- an instance does not come through here for them,
        // because it demands only the parent slots it does not override when it builds its own block.
        //
        // Non-blocking on purpose: this runs on a worker fiber inside Extract, so the load is kicked
        // async and the surface draws with the default material until it lands, at which point
        // InvalidateDependency wakes it. Instances are skipped entirely -- their block is already whole.
        if (Material != nullptr && Material->GetMaterial() == Material)
        {
            if (!static_cast<CMaterial*>(Material)->RequestTexturesResolved())
            {
                bReady   = false;
                Material = CMaterial::GetDefaultMaterial();
            }
        }

        CMaterial* ConcreteMaterial = Material->GetMaterial();

        const EBlendMode BlendMode    = Material->GetBlendMode();
        const bool       bTranslucent = BlendMode == EBlendMode::Translucent || BlendMode == EBlendMode::Additive;
        const bool       bMasked      = BlendMode == EBlendMode::Masked;
        const bool       bAdditive    = BlendMode == EBlendMode::Additive;
        const bool       bTwoSided    = bTranslucent || Material->IsTwoSided();

        R.VertexShader                   = Material->GetVertexShader();
        R.PixelShader                    = Material->GetPixelShader();
        R.MeshShader                     = ConcreteMaterial ? ConcreteMaterial->GetMeshShader() : nullptr;
        R.VisBufferMeshShader            = ConcreteMaterial ? ConcreteMaterial->GetVisBufferMeshShader() : nullptr;
        R.VisBufferVertexShader          = ConcreteMaterial ? ConcreteMaterial->GetVisBufferVertexShader() : nullptr;
        R.MaskedVisBufferPixelShader     = ConcreteMaterial ? ConcreteMaterial->GetMaskedVisBufferPixelShader() : nullptr;
        R.MaskedVisBufferPixelShaderPrim = ConcreteMaterial ? ConcreteMaterial->GetMaskedVisBufferPixelShaderPrim() : nullptr;
        R.DeferredShader                 = ConcreteMaterial ? ConcreteMaterial->GetDeferredShader() : nullptr;

        // Batch by the master material so instances sharing it collapse into one draw. This is why a
        // dynamic mesh still batches with every other mesh using the same material: the key is the
        // material, never the geometry.
        R.MaterialID            = (uint64)ConcreteMaterial;
        R.MaterialIdx           = (uint16)Material->GetMaterialIndex();
        R.bMaterialCastsShadows = Material->DoesCastShadows();

        EInstanceFlags MaterialFlags = EInstanceFlags::None;
        if (bTranslucent) { MaterialFlags |= EInstanceFlags::Translucent; }
        if (bMasked)      { MaterialFlags |= EInstanceFlags::Masked; }
        if (bTwoSided)    { MaterialFlags |= EInstanceFlags::TwoSided; }
        R.MaterialFlags = MaterialFlags;

        R.BatchKey = FDrawBatchKey
        {
            .MaterialID   = R.MaterialID,
            .bTranslucent = (bTranslucent ? 1u : 0u),
            .bMasked      = (bMasked      ? 1u : 0u),
            .bAdditive    = (bAdditive    ? 1u : 0u),
            .bTwoSided    = (bTwoSided    ? 1u : 0u),
        };

        return bReady;
    }

    namespace
    {
        // Linear scan over a list holding the mesh plus at most two materials per surface, so it stays in
        // the low tens even for a heavily-sectioned mesh.
        void AddDependency(FResolvedMesh& Out, const void* Dependency)
        {
            if (Dependency == nullptr)
            {
                return;
            }
            for (const void* Existing : Out.Dependencies)
            {
                if (Existing == Dependency)
                {
                    return;
                }
            }
            Out.Dependencies.push_back(Dependency);
        }
    }

    void FMeshResolveCache::ResolveSurfaces(FResolvedMesh& Out, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        // IsValid covers lifetime only, so an asset still in its load phase reaches here. Its properties
        // are all at defaults, so resolving now would cache an empty entry; leave it unresolved and the
        // caller re-arms until the data phase lands.
        ++Out.Generation;

        // Rebuilt from scratch, including on the early-out below: an entry that resolved against the
        // default material while the real one was compiling must stop depending on the default once it
        // swaps over, or every future default-material edit would re-resolve it for nothing.
        //
        // The mesh is a dependency of every entry unconditionally -- an unloaded one is the case that
        // most needs waking up, because its GPU buffers landing is what makes it drawable at all.
        Out.Dependencies.clear();
        Out.Dependencies.push_back((const void*)Mesh);

        if (Mesh->HasAnyFlag(OF_NeedsLoad))
        {
            Out.Surfaces.clear();
            Out.MeshletHeaderAddress = 0;
            Out.bAllMaterialsReady   = false;
            Out.bResolved            = false;
            return;
        }

        const FMeshResource& Resource = Mesh->GetMeshResource();

        const FAABB& LocalBounds = Mesh->GetAABB();
        Out.LocalCenter = (LocalBounds.Min + LocalBounds.Max) * 0.5f;
        // Degenerate bounds (geometry not resident) collapse to a zero sphere rather than a NaN radius.
        Out.LocalRadius = (LocalBounds.Max.x < LocalBounds.Min.x)
            ? 0.0f
            : Math::Length(LocalBounds.Max - Out.LocalCenter);

        Out.MeshletHeaderAddress = Mesh->GetMeshBuffers().MeshletHeaderBuffer;
        Out.bAllMaterialsReady   = true;

        Out.Surfaces.clear();
        Out.Surfaces.reserve(Resource.GeometrySurfaces.size());

        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            FResolvedSurface& R = Out.Surfaces.emplace_back();

            R.NumLODs = Surface.NumLODs;

            for (uint32 LOD = 0; LOD < MAX_MESH_LODS; ++LOD)
            {
                R.LODMeshletOffset[LOD]  = Surface.LODMeshletOffset[LOD];
                R.LODMeshletCount[LOD]   = Surface.LODMeshletCount[LOD];
                // Thresholds are non-negative, so squaring preserves their ordering.
                const float Threshold = Surface.LODScreenThreshold[LOD];
                R.LODScreenThresholdSq[LOD] = Threshold * Threshold;
            }

            // -1 means unassigned; widening keeps it out of range so it falls through to the default.
            const size_t Slot = (size_t)Surface.MaterialIndex;
            CMaterialInterface* RawMaterial = nullptr;
            if (Slot < Overrides.size())
            {
                RawMaterial = Overrides[Slot];
            }
            if (!RawMaterial)
            {
                RawMaterial = Mesh->GetMaterialAtSlot(Slot);
            }

            if (!MeshResolve::ResolveSurfaceMaterial(R, RawMaterial))
            {
                Out.bAllMaterialsReady = false;
            }

            // Both, and for different reasons. The AUTHORED material is what has to wake this entry when
            // it finishes compiling -- until then the surface is drawing the default one and R.MaterialID
            // does not name it at all. The CONCRETE master is what a material recompile invalidates, and
            // for an instance that is a different asset from the authored one.
            AddDependency(Out, (const void*)RawMaterial);
            AddDependency(Out, (const void*)(uintptr_t)R.MaterialID);
        }

        // MeshletHeaderAddress is 0 until the GPU buffers exist, which can lag the property data.
        Out.bResolved = Out.MeshletHeaderAddress != 0ull && Out.bAllMaterialsReady;
    }
}

