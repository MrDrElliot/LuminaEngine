#pragma once

#include "Renderer/ShaderHandle.h"

#include "MaterialInterface.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
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
    class CMaterialParameterCollection;
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
        MeshShadowMasked,           // MeshletMesh.slang + MESHLET_MESH_MASKED_SHADOW; masked materials only
        ShadowMaskedPixel,          // ShadowMaskedPixel.slang; masked materials only

        Count,
    };

    /** One compiled stage of a permutation; only stages that produced output are stored. */
    REFLECT()
    struct RUNTIME_API FMaterialStageBlob
    {
        GENERATED_BODY()

        PROPERTY()
        uint8 Stage = 0;

        /** Editor saves leave this empty when SourceHash is set; PostLoad refills it from the project shader cache. */
        PROPERTY()
        TVector<uint32> Spirv;

        /** Hash of the graph code this stage was built from, excluding the templates; 0 when no graph built it. */
        PROPERTY()
        uint64 SourceHash = 0;
    };

    /** One switch combination's shader set, owned by the master so two instances selecting it share one. */
    REFLECT()
    struct RUNTIME_API FMaterialShaderPermutation
    {
        GENERATED_BODY()

        PROPERTY()
        uint64 Key = 0;

        PROPERTY()
        TVector<FMaterialStageBlob> Stages;

        // Minted from Stages by PostLoad, so never serialized.
        FShaderH Entries[(size_t)EMaterialShaderStage::Count] = {};
    };

    REFLECT()
    class RUNTIME_API CMaterial : public CMaterialInterface
    {
        GENERATED_BODY()

    public:

        CMaterial();

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
        FShaderH GetPixelShader() const override;
        FShaderH GetVertexShader() const override;

        // This master's own entry for Stage; null when the domain or blend mode does not produce it.
        FShaderH GetStage(EMaterialShaderStage Stage) const { return StageEntries[(size_t)Stage]; }

        // The shadow geometry stage emits position only; carrying interpolants there halved cascade occupancy.
        FShaderH GetMeshShaderShadow() const { return GetStage(EMaterialShaderStage::MeshShadow); }
        FShaderH GetMeshShaderBase() const { return GetStage(EMaterialShaderStage::MeshBase); }
        FShaderH GetVisBufferMeshShader() const { return GetStage(EMaterialShaderStage::VisBufferMesh); }
        FShaderH GetVisBufferMeshShaderMasked() const { return GetStage(EMaterialShaderStage::VisBufferMeshMasked); }
        FShaderH GetMaskedVisBufferPixelShader() const { return GetStage(EMaterialShaderStage::MaskedVisBufferPixel); }
        FShaderH GetMeshShaderShadowMasked() const { return GetStage(EMaterialShaderStage::MeshShadowMasked); }
        FShaderH GetShadowMaskedPixelShader() const { return GetStage(EMaterialShaderStage::ShadowMaskedPixel); }
        FShaderH GetDeferredShader() const { return GetStage(EMaterialShaderStage::Deferred); }

        /** Bumped whenever a recompile actually swaps a shader-library entry, and only then -- the library
            is content-keyed, so a recompile producing identical SPIR-V returns the same entry and does not
            move this. Anything caching a resolved FShaderEntry* can compare it to know its copy went stale.
            Consumers that own an FMeshResolveCache entry do not need it (dependency invalidation covers
            them); SDynamicMeshComponent has no cache entry and does. */
        uint32 GetShaderRevision() const { return ShaderRevision; }

        static CMaterial* GetDefaultMaterial();
        static CMaterial* GetDefaultTerrainMaterial();

        static void CreateDefaultMaterial();
        static void CreateDefaultTerrainMaterial();

        /** Copy Spirv into the stage's serialized blob (no-op self-copy safe) and (re)commit its
            shader library entry. Shared by PostLoad and the editor material compile. */
        void CommitShaderStage(EMaterialShaderStage Stage, TSpan<const uint32> Spirv, uint64 SourceHash = 0);

        /** The blob half of CommitShaderStage only; PostLoad mints the entry. For compile callbacks off the game thread. */
        void SetStageBinaries(EMaterialShaderStage Stage, TSpan<const uint32> Spirv, uint64 SourceHash = 0);

        /** Drop a stage's binaries and library pointer (e.g. masked-only stages on a masked->opaque recompile). */
        void ClearShaderStage(EMaterialShaderStage Stage);

        /** Whether the domain and blend mode produce Stage; the compile clears every other one. */
        NODISCARD bool IsStageRequired(EMaterialShaderStage Stage) const;

        /** Every required stage has a library entry, which is what ready-for-render is derived from. */
        NODISCARD bool HasRequiredStages() const;

        const TVector<uint32>& GetShaderStageBinaries(EMaterialShaderStage Stage) const;

        /** Shader for Stage at permutation Key, falling back to this master's own stage when Key has none. */
        NODISCARD FShaderH GetStageForKey(EMaterialShaderStage Stage, uint64 Key) const;

        /** Whether Key is either the default permutation or one that has been compiled. */
        NODISCARD bool HasPermutation(uint64 Key) const;

        /** CommitShaderStage for one permutation, adding the permutation when Key is new. */
        void CommitPermutationStage(uint64 Key, EMaterialShaderStage Stage, TSpan<const uint32> Spirv, uint64 SourceHash = 0);

        /** CommitPermutationStage, dropped when Generation is no longer the one the key was minted under. */
        bool CommitPermutationStageIfCurrent(uint64 Key, uint32 Generation, EMaterialShaderStage Stage, TSpan<const uint32> Spirv,
            uint64 SourceHash = 0);

        /** Whether Key's permutation is stored but holds a stage the shader cache could not refill. */
        NODISCARD bool IsPermutationMissingBinaries(uint64 Key) const;

        /** Bumped by ClearPermutations, so a compile dispatched before a recompile can refuse to land. */
        NODISCARD uint32 GetPermutationGeneration() const { return PermutationGeneration; }

        /** Permutation Key's own bytecode for Stage, with no fallback, so an unbuilt stage reads empty. */
        NODISCARD const TVector<uint32>& GetPermutationStageBinaries(uint64 Key, EMaterialShaderStage Stage) const;

        /** Drops one permutation, so a recompile of it cannot leave a stage the new graph no longer emits. */
        void ClearPermutation(uint64 Key);

        /** Drops every permutation, which a recompile must do because it renumbers switch bits by name. */
        void ClearPermutations();

        /** Content hash of the material shader template sources */
        static uint64 GetShaderTemplateHash();

        // Bump when FMaterialCompiler emits different code for an unchanged graph, or the cache serves stale shaders.
        static constexpr uint64 MaterialCodegenVersion = 1;

        /** Project shader cache key for a stage built from SourceHash against the current templates and codegen. */
        static uint64 MakeShaderCacheKey(uint64 SourceHash);

        // Where a cook writes the template hash, so the shipped game compares against what its materials were built with.
        static constexpr const char* CookedShaderTemplateHashPath = "/Engine/ShaderTemplateHash.txt";

#if USING(WITH_EDITOR)
        /** Next asset material whose serialized stages predate the current shader templates  */
        static TObjectPtr<CMaterial> PopStaleTemplateMaterial();

        /** Ask the editor to compile Key's permutation; idempotent, so an instance may call it freely. */
        static void RequestPermutation(CMaterial* Material, uint64 Key);

        /** Rebuild a stored permutation the shader cache could not refill; the saved asset already holds it. */
        static void RequestPermutationRebuild(CMaterial* Material, uint64 Key);

        /** Next queued permutation request, or false when none remain. */
        static bool PopPermutationRequest(TObjectPtr<CMaterial>& OutMaterial, uint64& OutKey, bool& bOutRebuild);
#endif

        EMaterialType GetMaterialType() const override { return MaterialType; }
        bool DoesCastShadows() const override { return bCastShadows; }
        bool IsTwoSided() const override { return bTwoSided; }
        bool IsOITResolved() override { return BlendMode == EBlendMode::Translucent || BlendMode == EBlendMode::AlphaComposite; }
        bool IsUnorderedBlend() override { return BlendMode == EBlendMode::Additive || BlendMode == EBlendMode::Modulate; }
        bool ReceivesDecals() const override { return bReceivesDecals; }
        bool WritesDepth() const override { return bWriteDepth; }
        bool IsShadowOnly() const override { return bShadowOnly; }
        EBlendMode GetBlendMode() override { return BlendMode; }
        EMaterialShadingModel GetShadingModel() override { return ShadingModel; }

        void PostPropertyChange(FProperty* ChangedProperty) override;

        /** Folds settings the current domain cannot draw back to their defaults; see MaterialDomain. */
        void NormalizeRenderStateForDomain();

        PROPERTY(Editable)
        EMaterialType MaterialType = EMaterialType::PBR;

        PROPERTY(Editable, EditCondition = "MaterialType == PBR || MaterialType == Particle || MaterialType == Terrain")
        EBlendMode BlendMode = EBlendMode::Opaque;

        PROPERTY(Editable, EditCondition = "MaterialType == PBR || MaterialType == Terrain")
        EMaterialShadingModel ShadingModel = EMaterialShadingModel::Lit;

        PROPERTY(Editable, EditCondition = "MaterialType == PBR")
        bool bCastShadows = true;

        /** Drawn into shadow maps only; every camera view culls it. Needs bCastShadows to do anything. */
        PROPERTY(Editable, EditCondition = "MaterialType == PBR")
        bool bShadowOnly = false;

        PROPERTY(Editable, EditCondition = "MaterialType == PBR")
        bool bTwoSided = false;

        PROPERTY(Editable, EditCondition = "MaterialType == PBR")
        bool bDisableDepthTest = false;

        /** Whether DBuffer decals composite onto this surface before it is lit. */
        PROPERTY(Editable, EditCondition = "MaterialType == PBR")
        bool bReceivesDecals = true;

        /** Depth write for Additive and Modulate. Ignored by the OIT lane, which accumulates instead. */
        PROPERTY(Editable, EditCondition = "MaterialType == PBR || MaterialType == Particle")
        bool bWriteDepth = false;

        /** Masked blend threshold; pixels below are discarded. */
        PROPERTY(Editable, EditCondition = "BlendMode == Masked")
        float OpacityMaskClipValue = 0.333f;

        /** Default texture binding per texture-parameter index.  */
        PROPERTY()
        TVector<TSoftObjectPtr<CTexture>>       Textures;

        /** Strong refs for the slots that have actually been demanded */
        TVector<TObjectPtr<CTexture>>           ResolvedTextures;

        // Guards Textures and ResolvedTextures; the async load completion writes them from a loader thread.
        mutable FRecursiveMutex                 TextureSlotMutex;

        /** Slots whose async load came back empty; they stay on the placeholder instead of being re-requested. */
        uint64                                  UnresolvableTextureMask = 0;

        /** Set once RequestTexturesResolved has issued its async loads, so the per-frame render-path
         *  demand does not re-queue the same textures every frame until the first load lands. */
        bool                                    bTextureLoadRequested = false;

        /** Resolves slot Index if it has not been already and returns its bindless resource ID, or the
         *  placeholder ID while it cannot be resolved. */
        uint32 ResolveTextureSlot(uint32 Index);

        uint32 GetResolvedTextureSlot(uint32 Index) override { return ResolveTextureSlot(Index); }

        CTexture* GetTextureParameterTexture(const FName& Name, uint32 Index) override;

        /** Non-blocking demand for the render path.  */
        bool RequestTexturesResolved() override;

        /** Compiled stages, only the ones that produced output, in the same shape a permutation stores. */
        PROPERTY()
        TVector<FMaterialStageBlob>             Stages;

        // Guards Stages, Permutations and StageEntries, which one compile writes from a worker per stage.
        mutable FRecursiveMutex                 ShaderStageMutex;

        PROPERTY()
        TVector<FMaterialParameter>             Parameters;

        /** Collections this graph reads, in the slot order the shader compiled. Hard refs, since a
            collection is tiny and a material that samples one is useless without it. */
        PROPERTY()
        TVector<TObjectPtr<CMaterialParameterCollection>> ParameterCollections;

        /** Named static switches this graph declares, ordered by name with BitIndex assigned. */
        PROPERTY()
        TVector<FMaterialStaticSwitch>          StaticSwitches;

        /** Grass species this terrain graph scatters, collected from its GrassOutput node at compile.
            Empty for every material that has no such node, which is all of them but terrain. */
        PROPERTY()
        TVector<FGrassOutput>                   GrassOutputs;

        /** Compiled non-default permutations, keyed by MakeStaticSwitchKey; empty unless switches exist. */
        PROPERTY()
        TVector<FMaterialShaderPermutation>     Permutations;

        /** Bit ParameterName owns in a permutation key, or INDEX_NONE when this material has no such switch. */
        NODISCARD int32 FindStaticSwitchBit(const FName& ParameterName) const;

        /** Permutation key for Overrides; a switch absent from it contributes its authored default. */
        NODISCARD uint64 MakeStaticSwitchKey(const THashMap<FName, bool>& Overrides) const;

        /** The key this master's own shaders were compiled at, which is every switch at its default. */
        NODISCARD uint64 GetDefaultStaticSwitchKey() const;

        uint64 GetStaticSwitchKey() const override { return GetDefaultStaticSwitchKey(); }

        /** GetShaderTemplateHash() value the stage binaries were last compiled against. Editor saves write it only
            while a stage keeps SPIR-V no graph can rebuild; stages with a SourceHash are checked via the cache key. */
        PROPERTY()
        uint64 CompiledTemplateHash = 0;

        FMaterialUniforms                       MaterialUniforms;

        // Library entries keyed by asset GUID, indexed by EMaterialShaderStage; recompiles refresh them in place.
        FShaderH                                StageEntries[(size_t)EMaterialShaderStage::Count] = {};

        // See GetShaderRevision. Starts at 1 so a zeroed cached copy always reads as "never seen".
        uint32                                  ShaderRevision = 1;

        void BumpShaderRevision();

        // See GetPermutationGeneration. Runtime only, since a load starts every permutation current.
        uint32                                  PermutationGeneration = 1;

        bool RefreshTextureBindings(const CTexture* ChangedTexture) override;

        /** Whether this material binds ChangedTexture in any of its texture slots. */
        NODISCARD bool ReferencesTexture(const CTexture* ChangedTexture) const;


    protected:

        void UpdateMaterialUniforms() override;

    private:

        void RebuildParameterLookup();

        THashMap<FName, FMaterialParameter>     ParameterLookup;
    };

    /** Re-uploads the texture bindings of every material that references ChangedTexture (null = all of
     *  them), and returns how many were touched. */
    RUNTIME_API uint32 RefreshMaterialsReferencingTexture(const CTexture* ChangedTexture);
}
