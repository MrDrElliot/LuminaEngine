#pragma once

#include "Renderer/ShaderHandle.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "MaterialInterface.h"
#include "Containers/HashTable.h"
#include "Renderer/MaterialTypes.h"
#include "MaterialInstance.generated.h"

namespace Lumina
{
    class CMaterial;
    class CTexture;
}

namespace Lumina
{
    /** Per-parameter override on a material instance; only divergent parameters are stored. */
    REFLECT()
    struct RUNTIME_API FMaterialParameterOverride
    {
        GENERATED_BODY()

        PROPERTY()
        FName ParameterName;

        PROPERTY()
        EMaterialParameterType Type = EMaterialParameterType::Scalar;

        /** When false the override is retained (value kept) but not applied; the parent value shows instead. */
        PROPERTY()
        bool bEnabled = true;

        PROPERTY()
        float Scalar = 0.0f;

        PROPERTY()
        FVector4 Vector = FVector4(0.0f);

        PROPERTY()
        TObjectPtr<CTexture> Texture;
    };

    /** A static switch an instance flips; absence is the inherit state, so no enabled flag is needed. */
    REFLECT()
    struct RUNTIME_API FMaterialStaticSwitchOverride
    {
        GENERATED_BODY()

        PROPERTY()
        FName ParameterName;

        PROPERTY()
        bool bValue = false;
    };


    REFLECT()
    class RUNTIME_API CMaterialInstance : public CMaterialInterface
    {
        GENERATED_BODY()
    public:

        CMaterialInstance();

        /** A transient instance parented to Parent, with its own GPU slot, seeded from Parent's values. */
        static CMaterialInstance* CreateDynamic(CMaterialInterface* Parent);

        /** A dynamic instance has no package, and must stay out of the registry and the save path. */
        bool IsAsset() const override { return GetPackage() != nullptr; }

        CMaterialInterface* GetParentMaterial() const override { return Material.Get(); }

        /** Reparents, with a cycle and depth guard, and rebuilds this subtree. False if it was rejected. */
        bool SetParentMaterial(CMaterialInterface* NewParent);

        CMaterial* GetMaterial() const override;
        bool SetScalarValue(const FName& Name, const float Value) override;
        bool SetVectorValue(const FName& Name, const FVector4& Value) override;
        bool SetTextureValue(const FName& Name, CTexture* TextureValue) override;
        bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) override;
        FMaterialUniforms* GetMaterialUniforms() override { return &MaterialUniforms; }

        /** The parent's parameter list, which an instance never diverges from -- it only overrides VALUES.
            Read straight from the parent rather than mirrored here: a private copy had to be re-synced on
            every rebuild, and every rebuild ran off the back of a single SetScalarValue. */
        const TVector<FMaterialParameter>& GetMaterialParams() const;

        FShaderH GetVertexShader() const override;
        FShaderH GetPixelShader() const override;

        EMaterialType GetMaterialType() const override;
        bool DoesCastShadows() const override;
        bool IsTwoSided() const override;
        bool IsTranslucent() override;
        bool IsMasked() override;
        bool IsAdditive() override;
        bool IsOpaque() override;
        bool IsMomentResolved() override;
        bool IsUnorderedBlend() override;
        bool ReceivesDecals() const override;
        bool WritesDepth() const override;
        bool IsShadowOnly() const override;
        bool IsUnlit() override;
        bool DisableDepthTest() override;
        EBlendMode GetBlendMode() override;
        EMaterialShadingModel GetShadingModel() override;
        float GetOpacityMaskClipValue() override;
        

        void PostLoad() override;
        void OnDestroy() override;

        /** Editing the shading-model override has to re-stamp and re-upload the flags to be visible. */
        void PostPropertyChange(FProperty* ChangedProperty) override;

        /** Idempotent, and not just a PostLoad concern: an instance built at runtime never registers there. */
        void EnsureRegisteredWithParent();

        /** Reset uniforms to parent defaults and re-apply every override. */
        void RebuildUniformsFromOverrides();

        /** Re-derive uniforms from the (possibly recompiled) parent and push them to this instance's GPU slot.
            Called by the parent after a recompile; without the upload the instance keeps stale GPU uniforms. */
        void RefreshFromParent() override;

        /** Copy the parent's texture slots for every slot this instance does not override, and push the block
            if anything moved. Deliberately narrower than RefreshFromParent: no parameter rebuild and no
            synchronous resolve, so the parent can call it from the async texture-load completion, which is
            not on the game thread. */
        void RefreshInheritedTextureSlots() override;

        uint32 GetResolvedTextureSlot(uint32 Index) override;

        CTexture* GetTextureParameterTexture(const FName& Name, uint32 Index) override;

        bool InheritParameterValue(EMaterialParameterType Type, const FName& Name, uint16 Index) override;

        uint64 GetStaticSwitchKey() const override;

        /** Flips a named switch onto a different permutation; false when the root declares no such switch. */
        bool SetStaticSwitchValue(const FName& Name, bool bValue);

        /** This level's override, else the nearest ancestor's, else the root's authored default. */
        NODISCARD bool GetStaticSwitchValue(const FName& Name) const;

        NODISCARD bool HasStaticSwitchOverride(const FName& Name) const;

        /** Drops the override so this level inherits the switch again. */
        void RemoveStaticSwitchOverride(const FName& Name);

        /** Switch values overridden anywhere up this chain, written root-first so a nearer level wins. */
        void GatherStaticSwitchValues(THashMap<FName, bool>& OutValues, uint32 Depth = 0) const;


        /** Whether an enabled texture override supplies slot Index. False for a slot the parent binds without
            exposing a parameter for it (a plain Texture Sample node), which an instance can only inherit. */
        bool IsTextureSlotOverridden(uint32 Index) const;

        /** Bit i set = an enabled texture override supplies slot i. Hoist this out of any loop over slots:
            it is one pass over the (short) override list, where the per-slot query is a search for the
            parameter naming that slot. MAX_TEXTURES is 24, so a uint32 covers every slot. */
        NODISCARD uint32 GetOverriddenTextureMask() const;

        /** True only when an override exists for the parameter AND is enabled (the checkbox state). */
        bool IsOverrideEnabled(const FName& Name) const;
        
        void SetOverrideEnabled(const FName& Name, bool bEnabled);
        const FMaterialParameterOverride* FindOverride(const FName& Name) const;

        bool RefreshTextureBindings(const CTexture* ChangedTexture) override;

        bool RequestTexturesResolved() override;
        /** Drop a parameter's override entirely (discards its stored value). */
        void RemoveOverride(const FName& Name);

        /** Immediate parent: a base material, or another instance. Assign through SetParentMaterial. */
        PROPERTY(ReadOnly, Category = "Material")
        TObjectPtr<CMaterialInterface> Material;
        
        PROPERTY(Editable, Category = "Material|Shading")
        bool bOverrideShadingModel = false;

        PROPERTY(Editable, Category = "Material|Shading")
        EMaterialShadingModel ShadingModelOverride = EMaterialShadingModel::Lit;

        PROPERTY()
        TVector<FMaterialParameterOverride>     Overrides;

        /** Switches this level diverges on, inherited down the chain like a parameter override. */
        PROPERTY()
        TVector<FMaterialStaticSwitchOverride>  StaticSwitchOverrides;
        
    protected:

        void UpdateMaterialUniforms() override;

    private:

        /** Re-request, invalidate this level's resolves, and push both down the subtree. */
        void OnStaticSwitchesChanged();

        /** Depth-first re-request and invalidate over descendants, excluding this level. */
        void PropagateStaticSwitchChange(uint32 Depth = 0);

        /** Editor-only. Asks the root to build the permutation this level's key selects, if it has not. */
        void RequestStaticSwitchPermutation();

        FMaterialUniforms                       MaterialUniforms;
    };
}
