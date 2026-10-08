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
    // One compiled stage of a shader set. Only stages that produced output are stored.
    REFLECT()
    struct RUNTIME_API FMaterialStageBlob
    {
        GENERATED_BODY()

        PROPERTY()
        uint8 Stage = 0;

        PROPERTY()
        TVector<uint32> Spirv;
    };

    // The shader set for one static switch combination, owned by the root so every instance selecting it shares one.
    REFLECT()
    struct RUNTIME_API FMaterialShaderPermutation
    {
        GENERATED_BODY()

        PROPERTY()
        uint64 Key = 0;

        PROPERTY()
        TVector<FMaterialStageBlob> Stages;

        // Library entries minted from Stages, so never serialized.
        FShaderH Entries[(size_t)EMaterialShaderStage::Count] = {};
    };

    // A compiled material graph. It declares the parameters, textures and static switches, and owns the shaders for its default switch values and every permutation an instance has asked for.
    REFLECT()
    class RUNTIME_API CMaterial : public CMaterialInterface
    {
        GENERATED_BODY()

    public:

        void Serialize(FArchive& Ar) override;
        bool IsAsset() const override { return true; }
        void PostCreateCDO() override;
        void PostLoad() override;
        void OnDestroy() override;
        void PostPropertyChange(FProperty* ChangedProperty) override;
        void OnReferencesReplaced() override;

        CMaterial* GetMaterial() const override { return const_cast<CMaterial*>(this); }
        bool SetScalarValue(const FName& Name, const float Value) override;
        bool SetVectorValue(const FName& Name, const FVector4& Value) override;
        bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) override;
        uint64 GetStaticSwitchKey() const override { return GetDefaultStaticSwitchKey(); }
        uint32 GetResolvedTextureSlot(uint32 Index) override { return ResolveTextureSlot(Index); }
        CTexture* GetTextureParameterTexture(const FName& Name, uint32 Index) override;
        bool RefreshTextureBindings(const CTexture* ChangedTexture) override;
        bool RequestTexturesResolved() override;

        // Folds settings the current domain cannot draw back to their defaults.
        void NormalizeRenderStateForDomain();

        // Whether the domain and blend mode produce Stage. A compile clears every other one.
        NODISCARD bool IsStageRequired(EMaterialShaderStage Stage) const;

        // Every required stage of the default set has a library entry, which is what readiness derives from.
        NODISCARD bool HasRequiredStages() const;

        // The default set's entry for Stage, null when this material does not produce it.
        NODISCARD FShaderH GetStage(EMaterialShaderStage Stage) const;

        // Stage at permutation Key, falling back to the default set when Key has none for it.
        NODISCARD FShaderH GetStageForKey(EMaterialShaderStage Stage, uint64 Key) const;

        // Bumped only when a recompile swaps an entry, so anything caching a resolved shader without a resolve cache entry can tell it went stale.
        NODISCARD uint32 GetShaderRevision() const { return ShaderRevision; }

        // Stores the bytecode in the default set and commits its library entry.
        void CommitShaderStage(EMaterialShaderStage Stage, TSpan<const uint32> Spirv);

        // Stores the bytecode without committing, for compile callbacks off the game thread. PostLoad commits it.
        void SetStageBinaries(EMaterialShaderStage Stage, TSpan<const uint32> Spirv);

        // Drops one stage of the default set, such as the masked stages on a masked to opaque recompile.
        void ClearShaderStage(EMaterialShaderStage Stage);

        NODISCARD const TVector<uint32>& GetShaderStageBinaries(EMaterialShaderStage Stage) const;

        // Whether Key is the default permutation or one that has been compiled.
        NODISCARD bool HasPermutation(uint64 Key) const;

        // Stores and commits one stage of permutation Key, adding the permutation when Key is new.
        void CommitPermutationStage(uint64 Key, EMaterialShaderStage Stage, TSpan<const uint32> Spirv);

        // CommitPermutationStage, refused when a recompile has renumbered the switches since Generation.
        bool CommitPermutationStageIfCurrent(uint64 Key, uint32 Generation, EMaterialShaderStage Stage, TSpan<const uint32> Spirv);

        // Permutation Key's own bytecode, with no fallback, so an unbuilt stage reads empty.
        NODISCARD const TVector<uint32>& GetPermutationStageBinaries(uint64 Key, EMaterialShaderStage Stage) const;

        // Drops one permutation, so its recompile cannot keep a stage the new graph no longer emits.
        void ClearPermutation(uint64 Key);

        // Drops every permutation, which a recompile must do because it renumbers switch bits by name.
        void ClearPermutations();

        // Bumped by ClearPermutations, so a compile dispatched before a recompile can refuse to land.
        NODISCARD uint32 GetPermutationGeneration() const { return PermutationGeneration; }

        // The bit ParameterName owns in a permutation key, or kIndexNone when no switch has that name.
        NODISCARD int32 FindStaticSwitchBit(const FName& ParameterName) const;

        // Permutation key for Overrides, where a switch missing from it contributes its authored default.
        NODISCARD uint64 MakeStaticSwitchKey(const THashMap<FName, bool>& Overrides) const;

        // The key the default set was compiled at, every switch at its authored default.
        NODISCARD uint64 GetDefaultStaticSwitchKey() const;

        // Resolves slot Index if it is not already and returns its bindless ID, or the placeholder while it cannot be.
        uint32 ResolveTextureSlot(uint32 Index);

        // Whether a resolved slot binds ChangedTexture. Never resolves to answer, since it runs for every material on a reimport.
        NODISCARD bool ReferencesTexture(const CTexture* ChangedTexture) const;

        static CMaterial* GetDefaultMaterial();
        static CMaterial* GetDefaultTerrainMaterial();
        static void CreateDefaultMaterial();
        static void CreateDefaultTerrainMaterial();

        // Compiles a rooted PBR material from a pixel-inputs snippet. The caller owns the root reference.
        static CMaterial* CreateBuiltinMaterial(const FName& Name, const FString& PixelInputs, TSpan<const FMaterialParameter> InParameters = {});

        // Content hash of every source a material template can reach.
        static uint64 GetShaderTemplateHash();

        // Where a cook writes the template hash, so the shipped game compares against what its materials were built with.
        static constexpr const char* CookedShaderTemplateHashPath = "/Engine/ShaderTemplateHash.txt";

#if USING(WITH_EDITOR)
        // Next loaded material whose bytecode predates the current shader templates.
        static TStrongObjectPtr<CMaterial> PopStaleTemplateMaterial();

        // Asks the editor to compile permutation Key. Idempotent, so every instance may call it freely.
        static void RequestPermutation(CMaterial* Material, uint64 Key);

        // Next queued permutation request, false when none remain.
        static bool PopPermutationRequest(TStrongObjectPtr<CMaterial>& OutMaterial, uint64& OutKey);
#endif

        PROPERTY(Editable, Category = "Material")
        EMaterialType MaterialType = EMaterialType::PBR;

        PROPERTY(Editable, Category = "Material", EditCondition = "MaterialType == PBR || MaterialType == Particle || MaterialType == Terrain")
        EBlendMode BlendMode = EBlendMode::Opaque;

        PROPERTY(Editable, Category = "Material", EditCondition = "MaterialType == PBR || MaterialType == Terrain")
        EMaterialShadingModel ShadingModel = EMaterialShadingModel::Lit;

        // Pixels whose opacity falls below this are discarded.
        PROPERTY(Editable, Category = "Material", EditCondition = "BlendMode == Masked")
        float OpacityMaskClipValue = 0.333f;

        PROPERTY(Editable, Category = "Material|Surface", EditCondition = "MaterialType == PBR")
        bool bCastShadows = true;

        // Drawn into shadow maps only, culled from every camera view. Needs bCastShadows.
        PROPERTY(Editable, Category = "Material|Surface", EditCondition = "MaterialType == PBR")
        bool bShadowOnly = false;

        PROPERTY(Editable, Category = "Material|Surface", EditCondition = "MaterialType == PBR")
        bool bTwoSided = false;

        PROPERTY(Editable, Category = "Material|Surface", EditCondition = "MaterialType == PBR")
        bool bDisableDepthTest = false;

        // Whether DBuffer decals composite onto this surface before it is lit.
        PROPERTY(Editable, Category = "Material|Surface", EditCondition = "MaterialType == PBR")
        bool bReceivesDecals = true;

        // Depth write for Additive and Modulate. The OIT lane accumulates instead and ignores it.
        PROPERTY(Editable, Category = "Material|Surface", EditCondition = "MaterialType == PBR || MaterialType == Particle")
        bool bWriteDepth = false;

        // Declared by the graph, in the slot order the shader compiled.
        PROPERTY()
        TVector<FMaterialParameter> Parameters;

        // The default texture per texture parameter slot.
        PROPERTY()
        TVector<TSoftObjectPtr<CTexture>> Textures;

        // Collections the graph reads, in slot order. Hard refs, since a material sampling one is useless without it.
        PROPERTY()
        TVector<TStrongObjectPtr<CMaterialParameterCollection>> ParameterCollections;

        // Named compile-time branches, ordered by name with their key bits assigned.
        PROPERTY()
        TVector<FMaterialStaticSwitch> StaticSwitches;

        // Grass species a terrain graph scatters, from its GrassOutput node. Empty for everything but terrain.
        PROPERTY()
        TVector<FGrassOutput> GrassOutputs;

        // The default shader set, compiled with every switch at its authored default.
        PROPERTY()
        TVector<FMaterialStageBlob> Stages;

        // Every other switch combination an instance has asked for.
        PROPERTY()
        TVector<FMaterialShaderPermutation> Permutations;

        // GetShaderTemplateHash at the last compile. 0 marks an asset older than the hash, which recompiles once in the editor.
        PROPERTY()
        uint64 CompiledTemplateHash = 0;

        // Strong refs for the texture slots that have been demanded, written by the graph compile and the async loads.
        TVector<TStrongObjectPtr<CTexture>> ResolvedTextures;

        // Guards Textures and ResolvedTextures, which the async load completion writes from a loader thread.
        mutable FRecursiveMutex TextureSlotMutex;

    private:

        FMaterialShaderPermutation* FindPermutation(uint64 Key);
        const FMaterialShaderPermutation* FindPermutation(uint64 Key) const;

        // The library name for one stage, unique per material and permutation.
        FName MakeStageEntryName(EMaterialShaderStage Stage, uint64 Key, bool bPermutation) const;

        // Commits Spirv and swaps it into Entry, keeping exactly one library reference.
        void SwapStageEntry(FShaderH& Entry, const FName& EntryName, EMaterialShaderStage Stage, TSpan<const uint32> Spirv);

        // Surfaces cache the entries this guards, so every bump wakes them.
        void BumpShaderRevision();

        bool HasCompiledStage() const;
        void CommitSerializedShaders();
        void ApplyParameterDefaults();
        void BindTextureSlots();
        void StampRenderFlags();
        void BindParameterCollections();
        void RebuildParameterLookup();

        // The library owns the bytecode once committed, and a cooked build never recompiles.
        void DropCommittedBinaries();

        // Guards Stages, Permutations and DefaultEntries, which a compile writes from a worker per stage.
        mutable FRecursiveMutex ShaderStageMutex;

        // The default set's library entries, indexed by stage.
        FShaderH DefaultEntries[(size_t)EMaterialShaderStage::Count] = {};

        // Starts at 1 so a zeroed cached copy always reads as never seen.
        uint32 ShaderRevision = 1;

        // Runtime only, since a load starts every permutation current.
        uint32 PermutationGeneration = 1;

        // Slots whose async load came back empty. They stay on the placeholder instead of being re-requested.
        uint64 UnresolvableTextureMask = 0;

        // Latched once the async loads are issued, since Extract asks every frame until they land.
        bool bTextureLoadRequested = false;

        THashMap<FName, FMaterialParameter> ParameterLookup;
    };

    // Re-uploads the texture bindings of every material that references ChangedTexture, or all of them when it is null, and returns how many changed.
    RUNTIME_API uint32 RefreshMaterialsReferencingTexture(const CTexture* ChangedTexture);
}
