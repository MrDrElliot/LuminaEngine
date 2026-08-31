#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "MeshResolveCache.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Renderer/MeshletHeaderSlab.h"

namespace Lumina
{
    std::atomic<uint32> FMeshResolveCache::Epoch{1};
    std::atomic<uint32> FMeshResolveCache::PendingGeneration{1};
    FMutex           FMeshResolveCache::PendingMutex;
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
                    if (Candidate.bNeedsResolve)
                    {
                        RebuildEntry(Handle, Mesh, Overrides);

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

        RebuildEntry(NewHandle, Mesh, Overrides);

        HandlesByHash[KeyHash].push_back(NewHandle);
        return NewHandle;
    }

    namespace
    {
        // Shaders a surface must actually have to be VISIBLE, as opposed to merely compiled.
        bool HasRequiredPassShaders(CMaterialInterface* Material)
        {
            CMaterial* Concrete = IsValid(Material) ? Material->GetMaterial() : nullptr;
            if (Concrete == nullptr)
            {
                return false;
            }

            // There is one geometry path, so its stage is required outright.
            if (Concrete->GetVisBufferMeshShader() == nullptr)
            {
                return false;
            }

            if (Material->IsMomentResolved() || Material->IsUnorderedBlend())
            {
                return true;
            }

            return Concrete->GetDeferredShader() != nullptr;
        }
    }

    void MeshResolve::StampSurfaceSource(FResolvedSurface& Surface, CMaterialInterface* RawMaterial)
    {
        Surface.SourceMaterial       = RawMaterial;
        Surface.SourceMaterialGuid   = IsValid(RawMaterial) ? RawMaterial->GetGUID() : FGuid();
        Surface.SourceShaderRevision = (IsValid(RawMaterial) && IsValid(RawMaterial->GetMaterial()))
                                     ? RawMaterial->GetMaterial()->GetShaderRevision() : 0u;
    }

    bool MeshResolve::IsSurfaceStale(const FResolvedSurface& Surface)
    {
        if (!IsValid(Surface.SourceMaterial))
        {
            // Nothing was requested (or it died); the assignment hash covers a pointer change.
            return false;
        }

        // A different asset occupies the slot now, so the revision there means nothing.
        if (Surface.SourceMaterial->GetGUID() != Surface.SourceMaterialGuid)
        {
            return true;
        }

        const CMaterial* Concrete = Surface.SourceMaterial->GetMaterial();
        const uint32 Live = IsValid(Concrete) ? Concrete->GetShaderRevision() : 0u;

        return Live != Surface.SourceShaderRevision;
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

        // Second gate. IsReadyForRender reports that the material finished COMPILING.
        if (!HasRequiredPassShaders(Material))
        {
            bReady   = false;
            Material = CMaterial::GetDefaultMaterial();
        }

        if (Material != nullptr && !Material->RequestTexturesResolved())
        {
            bReady   = false;
            Material = CMaterial::GetDefaultMaterial();
        }

        CMaterial* ConcreteMaterial = Material->GetMaterial();

        const EBlendMode BlendMode    = Material->GetBlendMode();
        const bool       bMasked      = BlendMode == EBlendMode::Masked;
        const bool       bAdditive    = BlendMode == EBlendMode::Additive;
        const bool       bModulate    = BlendMode == EBlendMode::Modulate;
        // Everything the forward lane draws, whether the moments resolve it or a blend composites it.
        const bool       bTranslucent = Material->IsMomentResolved() || Material->IsUnorderedBlend();
        // Translucency forces two-sided, or a translucent surface reads as a hole from behind.
        const bool       bTwoSided    = bTranslucent || Material->IsTwoSided();

        // The default key resolves to the master's own stage, so a switchless material pays nothing.
        const uint64 SwitchKey = Material->GetStaticSwitchKey();
        auto Stage = [ConcreteMaterial, SwitchKey](EMaterialShaderStage InStage)
        {
            return ConcreteMaterial ? ConcreteMaterial->GetStageForKey(InStage, SwitchKey) : FShaderH{};
        };

        R.PixelShader                = Stage(EMaterialShaderStage::Pixel);
        R.VertexShader               = Stage(EMaterialShaderStage::Vertex);
        R.MeshShaderShadow           = Stage(EMaterialShaderStage::MeshShadow);
        R.MeshShaderBase             = Stage(EMaterialShaderStage::MeshBase);
        R.VisBufferMeshShader        = Stage(EMaterialShaderStage::VisBufferMesh);
        R.VisBufferMeshShaderMasked  = Stage(EMaterialShaderStage::VisBufferMeshMasked);
        R.MaskedVisBufferPixelShader = Stage(EMaterialShaderStage::MaskedVisBufferPixel);
        R.MeshShaderShadowMasked     = Stage(EMaterialShaderStage::MeshShadowMasked);
        R.ShadowMaskedPixelShader    = Stage(EMaterialShaderStage::ShadowMaskedPixel);
        R.DeferredShader             = Stage(EMaterialShaderStage::Deferred);
        R.MomentPixelShader          = Stage(EMaterialShaderStage::MomentPixel);

        // Recorded from the RAW request, so a not-ready material that later compiles is noticed.
        MeshResolve::StampSurfaceSource(R, RawMaterial);

        R.MaterialID            = (uint64)ConcreteMaterial;
        R.MaterialIdx           = (uint16)Material->GetMaterialIndex();
        R.bMaterialCastsShadows = Material->DoesCastShadows();

        // Casting is what shadow-only draws, so the two together are the only useful combination.
        const bool bShadowOnly = Material->IsShadowOnly() && R.bMaterialCastsShadows;

        EInstanceFlags MaterialFlags = EInstanceFlags::None;
        if (bTranslucent) { MaterialFlags |= EInstanceFlags::Translucent; }
        if (bMasked)      { MaterialFlags |= EInstanceFlags::Masked; }
        if (bTwoSided)    { MaterialFlags |= EInstanceFlags::TwoSided; }
        if (bShadowOnly)  { MaterialFlags |= EInstanceFlags::ShadowOnly; }
        R.MaterialFlags = MaterialFlags;

        // The MBOIT passes and the unordered blend pass are the only binders of MeshShaderBase / PixelShader.
        const bool bForwardShaded = bTranslucent;

        // A commutative blend is already order-independent, so those batches skip the moments.
        const bool bMomentGenerated = Material->IsMomentResolved();

        // Only the unordered lane can honour it; the MBOIT lane reads depth and never writes.
        const bool bWriteDepth = Material->WritesDepth() && Material->IsUnorderedBlend();

        R.BatchKey = FDrawBatchKey
        {
            // Only the shaders the batch's OWN passes bind, or it splits on something nothing reads.
            .VisBufferMeshShader        = R.VisBufferMeshShader,
            .VisBufferMeshShaderMasked  = bMasked        ? R.VisBufferMeshShaderMasked  : FShaderH{},
            .MaskedVisBufferPixelShader = bMasked        ? R.MaskedVisBufferPixelShader : FShaderH{},
            .MeshShaderBase             = bForwardShaded ? R.MeshShaderBase             : FShaderH{},
            .MeshShaderShadow           = R.MeshShaderShadow,
            .MeshShaderShadowMasked     = bMasked        ? R.MeshShaderShadowMasked     : FShaderH{},
            .ShadowMaskedPixelShader    = bMasked        ? R.ShadowMaskedPixelShader    : FShaderH{},
            .PixelShader                = bForwardShaded  ? R.PixelShader                : FShaderH{},
            .MomentPixelShader          = bMomentGenerated ? R.MomentPixelShader         : FShaderH{},
            .bTranslucent = (bTranslucent ? 1u : 0u),
            .bMasked      = (bMasked      ? 1u : 0u),
            .bAdditive    = (bAdditive    ? 1u : 0u),
            .bModulate    = (bModulate    ? 1u : 0u),
            .bWriteDepth  = (bWriteDepth  ? 1u : 0u),
            .bTwoSided    = (bTwoSided    ? 1u : 0u),
        };

        return bReady;
    }

    namespace
    {
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
        ++Out.Generation;

        Out.Dependencies.clear();
        Out.Dependencies.push_back((const void*)Mesh);

        if (Mesh->HasAnyFlag(OF_NeedsLoad))
        {
            Out.Surfaces.clear();
            Out.MeshletHeaderSlot = 0;
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

        Out.MeshletHeaderSlot = Mesh->GetMeshBuffers().MeshletHeaderSlot;
        Out.bAllMaterialsReady   = true;

        Out.Surfaces.clear();
        Out.Surfaces.reserve(Resource.GeometrySurfaces.size());

        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            FResolvedSurface& R = Out.Surfaces.emplace_back();

            R.NumLODs     = Surface.NumLODs;
            R.TexelFactor = Surface.TexelFactor;

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

            AddDependency(Out, (const void*)RawMaterial);
            AddDependency(Out, (const void*)(uintptr_t)R.MaterialID);

            // MaterialID names the SUBSTITUTED material, so a fallback surface is not keyed on its real master.
            if (IsValid(RawMaterial))
            {
                AddDependency(Out, (const void*)RawMaterial->GetMaterial());
            }
        }

        // MeshletHeaderSlot is 0 until the GPU buffers exist, which can lag the property data.
        Out.bResolved = Out.MeshletHeaderSlot != MeshletHeaderSlab::kNullSlot && Out.bAllMaterialsReady;
    }
}
