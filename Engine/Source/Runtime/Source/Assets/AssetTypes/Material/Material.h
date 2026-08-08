#pragma once

#include "MaterialInterface.h"
#include "Containers/Array.h"
#include "Core/Threading/Thread.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/RenderResource.h"
#include "Material.generated.h"

namespace Lumina
{
    class CTexture;
    class CMaterialInstance;
}

namespace Lumina
{
    // Compiled shader stages a material carries. Each stage pairs a serialized SPIR-V blob with a
    // shader library entry; the (blob member, entry member, GUID suffix, type) tuple per stage lives
    // in a single table in Material.cpp that PostLoad and the editor graph compile both walk, so
    // adding a stage means touching exactly one place.
    enum class EMaterialShaderStage : uint8
    {
        Pixel,
        // Non-meshlet domains only: UI, PostProcess, Decal and Terrain raster through a real vertex
        // shader. PBR geometry has no vertex stage at all -- it is task + mesh, end to end.
        Vertex,
        MeshShadow,                 // MeshletMesh.slang (depth-only output); shadow geometry
        MeshBase,                   // MeshletMesh.slang + MESHLET_MESH_BASE; translucent / additive geometry
        VisBufferMesh,              // MeshletVisBuffer.slang (opaque, position-only output)
        VisBufferMeshMasked,        // MeshletVisBuffer.slang + VISBUFFER_MASKED_GEOM; masked materials only
        MaskedVisBufferPixel,       // VisBufferMaskedPixel.slang + VISBUFFER_PRIMID; masked materials only
        Deferred,                   // DeferredMaterial.slang

        Count,
    };

    REFLECT()
    class RUNTIME_API CMaterial : public CMaterialInterface
    {
        GENERATED_BODY()

    public:

        CMaterial();

        void RegisterInstance(CMaterialInstance* Instance);
        void UnregisterInstance(CMaterialInstance* Instance);

        /** Refresh registered instance uniforms after a recompile. */
        void NotifyInstancesParentChanged();

        void Serialize(FArchive& Ar) override;
        bool IsAsset() const override { return true; }
        void PostCreateCDO() override;
        void PostLoad() override;
        void OnDestroy() override;
        
        bool SetScalarValue(const FName& Name, const float Value) override;
        bool SetVectorValue(const FName& Name, const FVector4& Value) override;
        bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) override;
        FMaterialUniforms* GetMaterialUniforms() override { return &MaterialUniforms; }
        
        CMaterial* GetMaterial() const override;
        const FShaderEntry* GetPixelShader() const override;
        const FShaderEntry* GetVertexShader() const override;

        // Geometry stages. Every one is a MESH shader fed by the shared amplification stage -- there is no
        // vertex path for meshlet geometry. All are per-material because the vertex graph's World Position
        // Offset is compiled into them.
        //
        // Shadow emits position only; base emits the full interpolant set for forward-shaded translucency.
        // Carrying interpolants through the shadow pass reserved 124B per vertex of mesh output storage for
        // a consumer that reads SV_Position, which is what held cascade occupancy at 2.0 warps/cycle.
        const FShaderEntry* GetMeshShaderShadow() const { return MeshShaderShadow; }
        const FShaderEntry* GetMeshShaderBase() const { return MeshShaderBase; }

        const FShaderEntry* GetVisBufferMeshShader() const { return VisBufferMeshShader; }
        // Masked-only VisBuffer geometry: emits the interpolants the masked PS needs. Null for opaque
        // materials, which use the position-only GetVisBufferMeshShader above.
        const FShaderEntry* GetVisBufferMeshShaderMasked() const { return VisBufferMeshShaderMasked; }

        // Masked-only VisBuffer PIXEL shader: runs the opacity graph and alpha-clips cut-out texels BEFORE
        // they write VisID/depth, so they cannot stamp occluding depth. Null for non-masked materials.
        const FShaderEntry* GetMaskedVisBufferPixelShader() const { return MaskedVisBufferPixelShader; }

        // Deferred material pixel shader (DeferredMaterial.slang): reconstructs surface from the VisBuffer
        // triangle ID and shades. The deferred pass binds it per opaque material.
        const FShaderEntry* GetDeferredShader() const { return DeferredShader; }

        static CMaterial* GetDefaultMaterial();
        static CMaterial* GetDefaultTerrainMaterial();

        static void CreateDefaultMaterial();
        static void CreateDefaultTerrainMaterial();

        /** Copy Spirv into the stage's serialized blob (no-op self-copy safe) and (re)commit its
            shader library entry. Shared by PostLoad and the editor material compile. */
        void CommitShaderStage(EMaterialShaderStage Stage, TSpan<const uint32> Spirv);

        /** Drop a stage's binaries and library pointer (e.g. masked-only stages on a masked->opaque recompile). */
        void ClearShaderStage(EMaterialShaderStage Stage);

        const TVector<uint32>& GetShaderStageBinaries(EMaterialShaderStage Stage) const;

        /** Content hash of the material shader template sources (Shaders/MaterialShader + Shaders/Includes),
            computed once per run. Serialized per material as CompiledTemplateHash so stale binaries are
            detectable after template edits. */
        static uint64 GetShaderTemplateHash();

#if USING(WITH_EDITOR)
        /** Next asset material whose serialized stages predate the current shader templates (queued during
            PostLoad); null when none remain. The editor drains this and recompiles from the saved graph. */
        static TObjectPtr<CMaterial> PopStaleTemplateMaterial();
#endif

        EMaterialType GetMaterialType() const override { return MaterialType; }
        bool DoesCastShadows() const override { return bCastShadows; }
        bool IsTwoSided() const override { return bTwoSided; }
        bool IsTranslucent() override { return BlendMode == EBlendMode::Translucent; }
        bool IsMasked() override { return BlendMode == EBlendMode::Masked; }
        bool IsAdditive() override { return BlendMode == EBlendMode::Additive; }
        bool IsOpaque() override { return BlendMode == EBlendMode::Opaque; }
        bool IsUnlit() override { return ShadingModel == EMaterialShadingModel::Unlit; }
        bool DisableDepthTest() override { return bDisableDepthTest; }
        EBlendMode GetBlendMode() override { return BlendMode; }
        EMaterialShadingModel GetShadingModel() override { return ShadingModel; }
        float GetOpacityMaskClipValue() override { return OpacityMaskClipValue; }

        PROPERTY(Editable)
        EMaterialType MaterialType;

        PROPERTY(Editable)
        EBlendMode BlendMode = EBlendMode::Opaque;

        PROPERTY(Editable)
        EMaterialShadingModel ShadingModel = EMaterialShadingModel::Lit;

        PROPERTY(Editable)
        bool bCastShadows = true;

        PROPERTY(Editable)
        bool bTwoSided = false;

        PROPERTY(Editable)
        bool bDisableDepthTest = false;

        /** Masked blend threshold; pixels below are discarded. */
        PROPERTY(Editable)
        float OpacityMaskClipValue = 0.333f;

        /** Default texture binding per texture-parameter index.
         *
         *  SOFT deliberately. These are only DEFAULTS: an instance that overrides a texture parameter
         *  never reads the master's entry for it, so a hard reference would load the texture, upload it,
         *  and hold a bindless slot for something nothing samples -- for every instance-only material in
         *  the project. Resolution is per-slot and on demand through ResolveTextureSlot; an unresolved
         *  slot reads the 1x1 placeholder and the block is re-pushed once the real texture lands.
         *
         *  Soft also reclassifies the cook edge (Hard -> Soft), so a default still ships when something
         *  reaches it but no longer forces itself into the referring material's chunk. */
        PROPERTY()
        TVector<TSoftObjectPtr<CTexture>>       Textures;

        /** Strong refs for the slots that have actually been demanded, parallel-indexed with Textures.
         *  Not a PROPERTY: this is the runtime cache the soft refs resolve into, and holding the strong
         *  ref here is precisely what keeps a demanded default resident. Entries stay null until asked
         *  for, which is the whole point of the change. */
        TVector<TObjectPtr<CTexture>>           ResolvedTextures;

        /** Set once RequestTexturesResolved has issued its async loads, so the per-frame render-path
         *  demand does not re-queue the same textures every frame until the first load lands. */
        bool                                    bTextureLoadRequested = false;

        /** Resolves slot Index if it has not been already and returns its bindless resource ID, or the
         *  placeholder ID while it cannot be resolved. Idempotent, and cheap after the first call.
         *
         *  Loads synchronously, so call it from a load or editor context -- NOT from the render extract,
         *  which runs on worker fibers where blocking on disk I/O would stall a frame. The render-side
         *  demand path goes through RequestTexturesResolved instead. */
        uint32 ResolveTextureSlot(uint32 Index);

        /** Non-blocking demand for the render path. Returns true when every slot is already resolved;
         *  otherwise kicks a one-shot async load for the missing ones and returns false, leaving the
         *  caller to treat this material as not-ready.
         *
         *  Deliberately shaped to reuse the fallback that already exists for a still-compiling material:
         *  the surface draws with the default material for a few frames, and the load completion calls
         *  FMeshResolveCache::InvalidateDependency to wake it. Blocking here instead would stall a
         *  worker fiber on disk I/O in the middle of Extract. */
        bool RequestTexturesResolved() override;

        PROPERTY()
        TVector<uint32>                         PixelShaderBinaries;

        /** Vertex stage for the non-meshlet domains (UI / PostProcess / Decal / Terrain). PBR geometry
            has none -- it is task + mesh, end to end. */
        PROPERTY()
        TVector<uint32>                         VertexShaderBinaries;

        /** Shadow geometry stage (MeshletMesh.slang, depth-only output). */
        PROPERTY()
        TVector<uint32>                         MeshShaderShadowBinaries;

        /** Translucent / additive geometry stage (MeshletMesh.slang + MESHLET_MESH_BASE). */
        PROPERTY()
        TVector<uint32>                         MeshShaderBaseBinaries;

        /** VisBuffer geometry stage (MeshletVisBuffer.slang); empty if not compiled. */
        PROPERTY()
        TVector<uint32>                         VisBufferMeshShaderBinaries;

        /** VisBuffer geometry, masked variant (MeshletVisBuffer.slang + VISBUFFER_MASKED_GEOM); empty for non-masked. */
        TVector<uint32>                         VisBufferMeshShaderMaskedBinaries;

        /** Masked-only VisBuffer pixel stage (VisBufferMaskedPixel.slang + VISBUFFER_PRIMID). */
        PROPERTY()
        TVector<uint32>                         MaskedVisBufferPixelShaderBinaries;

        /** Deferred material pixel stage (DeferredMaterial.slang); empty if not compiled. */
        PROPERTY()
        TVector<uint32>                         DeferredShaderBinaries;

        PROPERTY()
        TVector<FMaterialParameter>             Parameters;

        /** GetShaderTemplateHash() value the stage binaries were last compiled against. 0 = legacy asset
            (predates hashing) -> treated as stale, so it auto-recompiles once in the editor and heals on save. */
        PROPERTY()
        uint64 CompiledTemplateHash = 0;

        FMaterialUniforms                       MaterialUniforms;

        // Library entries keyed by asset GUID; recompiles refresh them in place.
        const FShaderEntry*                     PixelShader = nullptr;
        const FShaderEntry*                     VertexShader = nullptr;
        const FShaderEntry*                     MeshShaderShadow = nullptr;
        const FShaderEntry*                     MeshShaderBase = nullptr;
        const FShaderEntry*                     VisBufferMeshShader = nullptr;
        const FShaderEntry*                     VisBufferMeshShaderMasked = nullptr;
        const FShaderEntry*                     MaskedVisBufferPixelShader = nullptr;
        const FShaderEntry*                     DeferredShader = nullptr;

        bool RefreshTextureBindings(const CTexture* ChangedTexture) override;

        /** Whether this material binds ChangedTexture in any of its texture slots. */
        NODISCARD bool ReferencesTexture(const CTexture* ChangedTexture) const;

    protected:

        void UpdateMaterialUniforms() override;

    private:

        void RebuildParameterLookup();

        /** Instance back-references; instances unregister in OnDestroy so raw pointers are safe. Guarded by
            InstancesMutex: instances of one master Register concurrently during the parallel PostLoad wave. */
        TVector<CMaterialInstance*>             Instances;
        FMutex                                  InstancesMutex;

        THashMap<FName, FMaterialParameter>     ParameterLookup;
    };

    /** Re-uploads the texture bindings of every material that references ChangedTexture (null = all of
     *  them), and returns how many were touched.
     *
     *  Masters run before instances: an instance rebuilds its uniform block by copying its parent's, so
     *  refreshing them the other way round would copy the stale parent block and then overwrite the
     *  instance's own correct value with it.
     *
     *  Deliberately global rather than a per-tool refresh: a material whose editor is closed is still being
     *  rendered in the world, and it is exactly as wrong as one that happens to be open. */
    RUNTIME_API uint32 RefreshMaterialsReferencingTexture(const CTexture* ChangedTexture);
}
