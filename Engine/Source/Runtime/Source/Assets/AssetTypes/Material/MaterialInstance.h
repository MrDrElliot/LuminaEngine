#pragma once

#include "Renderer/ShaderHandle.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "MaterialInterface.h"
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


    REFLECT()
    class RUNTIME_API CMaterialInstance : public CMaterialInterface
    {
        GENERATED_BODY()
    public:

        CMaterialInstance();

        bool IsAsset() const override { return true; }

        CMaterial* GetMaterial() const override;
        bool SetScalarValue(const FName& Name, const float Value) override;
        bool SetVectorValue(const FName& Name, const FVector4& Value) override;
        bool SetTextureValue(const FName& Name, CTexture* TextureValue);
        bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) override;
        FMaterialUniforms* GetMaterialUniforms() override { return &MaterialUniforms; }
        const TVector<FMaterialParameter>& GetMaterialParams() const { return Parameters; }

        FShaderH GetVertexShader() const override;
        FShaderH GetPixelShader() const override;

        EMaterialType GetMaterialType() const override;
        bool DoesCastShadows() const override;
        bool IsTwoSided() const override;
        bool IsTranslucent() override;
        bool IsMasked() override;
        bool IsAdditive() override;
        bool IsOpaque() override;
        bool IsUnlit() override;
        bool DisableDepthTest() override;
        EBlendMode GetBlendMode() override;
        EMaterialShadingModel GetShadingModel() override;
        float GetOpacityMaskClipValue() override;
        

        void PostLoad() override;
        void OnDestroy() override;

        /** Editing the shading-model override has to re-stamp and re-upload the flags to be visible. */
        void PostPropertyChange(FProperty* ChangedProperty) override;

        /** Reset uniforms to parent defaults and re-apply every override. */
        void RebuildUniformsFromOverrides();

        /** Re-derive uniforms from the (possibly recompiled) parent and push them to this instance's GPU slot.
            Called by the parent after a recompile; without the upload the instance keeps stale GPU uniforms. */
        void RefreshFromParent();

        /** Copy the parent's texture slots for every slot this instance does not override, and push the block
            if anything moved. Deliberately narrower than RefreshFromParent: no parameter rebuild and no
            synchronous resolve, so the parent can call it from the async texture-load completion, which is
            not on the game thread. */
        void RefreshInheritedTextureSlots();

        /** Whether an enabled texture override supplies slot Index. False for a slot the parent binds without
            exposing a parameter for it (a plain Texture Sample node), which an instance can only inherit. */
        bool IsTextureSlotOverridden(uint32 Index) const;

        /** True only when an override exists for the parameter AND is enabled (the checkbox state). */
        bool IsOverrideEnabled(const FName& Name) const;
        /** Enable/disable a parameter's override. Disabling RETAINS the stored value; enabling with no existing
            entry seeds one from the parent's current value. */
        void SetOverrideEnabled(const FName& Name, bool bEnabled);
        /** The stored override for a parameter (retained even while disabled), or null. */
        const FMaterialParameterOverride* FindOverride(const FName& Name) const;

        bool RefreshTextureBindings(const CTexture* ChangedTexture) override;

        bool RequestTexturesResolved() override;
        /** Drop a parameter's override entirely (discards its stored value). */
        void RemoveOverride(const FName& Name);

        PROPERTY(ReadOnly, Category = "Material")
        TObjectPtr<CMaterial> Material;

        /**
         * Override the parent's shading model without recompiling anything. The model travels in the
         * material's runtime flags (EMaterialGPUFlags bits 3-5), so an instance can simply stamp a
         * different one -- no new shader, no new PSO.
         *
         * Two limits worth knowing, both from the parent's compiled shader rather than this flag:
         *
         *  - A parent whose own model is Unlit compiled its shaders with the UNLIT define, which strips
         *    the lighting path outright. An instance of it cannot become Lit again. Going the other way
         *    (Lit parent -> Unlit instance) works, because the lit shader tests the model at runtime.
         *
         *  - Clearcoat only shows if the parent's graph actually drives the Clearcoat pins. On a graph
         *    that leaves them unconnected the coat strength is 0, so the override is a no-op rather than
         *    an error.
         */
        PROPERTY(Editable, Category = "Material|Shading")
        bool bOverrideShadingModel = false;

        PROPERTY(Editable, Category = "Material|Shading")
        EMaterialShadingModel ShadingModelOverride = EMaterialShadingModel::Lit;

        PROPERTY()
        TVector<FMaterialParameterOverride>     Overrides;
        
    protected:
        
        void UpdateMaterialUniforms() override;
        
    private:
        
        TVector<FMaterialParameter>             Parameters;
        FMaterialUniforms                       MaterialUniforms;
    };
}
