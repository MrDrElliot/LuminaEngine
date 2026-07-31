#include "pch.h"
#include "MeshResolveCache.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"

namespace Lumina
{
    std::atomic<uint32> FMeshResolveCache::Epoch{1};
    std::atomic<uint32> FMeshResolveCache::PendingGeneration{1};

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
        HandlesByHash.clear();
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
                    // Rebuild when the mesh/material data has moved on (ResolvedEpoch), as well as while
                    // still waiting for it to arrive (!bResolved). Without the epoch test a resolved
                    // entry was frozen forever: assigning a mesh's default materials keeps the key
                    // identical, so every instance in every world kept drawing the old assignment until
                    // something perturbed the override list into a different key.
                    if (!Candidate.bResolved || Candidate.ResolvedEpoch != GetEpoch())
                    {
                        ResolveSurfaces(*Entries[Handle], Mesh, Overrides);
                        if (!Entries[Handle]->bResolved)
                        {
                            MarkPendingWork();
                        }
                    }
                    return Handle;
                }
            }
        }

        const uint32 NewHandle = (uint32)Entries.size();
        Entries.push_back(Memory::New<FResolvedMesh>());
        FResolvedMesh& Entry = *Entries.back();

        Entry.MeshKey  = Mesh;
        Entry.MeshGuid = Mesh->GetGUID();
        Entry.OverrideKey.reserve(Overrides.size());
        for (CMaterialInterface* Override : Overrides)
        {
            Entry.OverrideKey.push_back((const void*)Override);
        }

        ResolveSurfaces(Entry, Mesh, Overrides);
        if (!Entry.bResolved)
        {
            MarkPendingWork();
        }

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

    void FMeshResolveCache::ResolveSurfaces(FResolvedMesh& Out, CMesh* Mesh, const TVector<CMaterialInterface*>& Overrides)
    {
        // IsValid covers lifetime only, so an asset still in its load phase reaches here. Its properties
        // are all at defaults, so resolving now would cache an empty entry; leave it unresolved and the
        // caller re-arms until the data phase lands.
        ++Out.Generation;

        // Stamped up front, including on the early-out below: this entry now reflects the epoch it was
        // last examined at, so a mesh that is still loading is retried through bResolved rather than by
        // re-running the whole resolve on every lookup.
        Out.ResolvedEpoch = GetEpoch();

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
        }

        // MeshletHeaderAddress is 0 until the GPU buffers exist, which can lag the property data.
        Out.bResolved = Out.MeshletHeaderAddress != 0ull && Out.bAllMaterialsReady;
    }
}

