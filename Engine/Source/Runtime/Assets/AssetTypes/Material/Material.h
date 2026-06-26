#pragma once

#include "MaterialInterface.h"
#include "Containers/Array.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
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
        const FShaderEntry* GetVertexShader() const override;
        const FShaderEntry* GetPixelShader() const override;

        // Mesh-shader variant of the geometry stage (MeshletMesh.slang). Null when unavailable; the
        // renderer uses it only when r.MeshShaders is on and the device supports VK_EXT_mesh_shader.
        const FShaderEntry* GetMeshShader() const { return MeshShader; }

        // VisBuffer geometry stage; per-material for WPO. The VisBuffer pass uses the mesh variant when the
        // device supports mesh shaders, else the vertex-emulation variant -- VisBuffer never requires either.
        const FShaderEntry* GetVisBufferMeshShader() const { return VisBufferMeshShader; }
        const FShaderEntry* GetVisBufferVertexShader() const { return VisBufferVertexShader; }

        // Masked-only VisBuffer PIXEL shaders: run the opacity graph and alpha-clip cut-out texels BEFORE they
        // write VisID/depth (VisBufferMaskedPixel.slang). The GEOMETRY stage is the SAME shared VisBuffer VS/mesh
        // as opaque -- the VISBUFFER_MASKED spec constant makes it emit the interpolants the masked PS reads.
        // Flat = VS-emulation path; Prim = mesh-shader path (SV_PrimitiveID). Null for non-masked materials.
        const FShaderEntry* GetMaskedVisBufferPixelShader() const { return MaskedVisBufferPixelShader; }
        const FShaderEntry* GetMaskedVisBufferPixelShaderPrim() const { return MaskedVisBufferPixelShaderPrim; }

        // Deferred material pixel shader (DeferredMaterial.slang): reconstructs surface from the VisBuffer
        // triangle ID and shades. The deferred pass binds it per opaque material.
        const FShaderEntry* GetDeferredShader() const { return DeferredShader; }

        static CMaterial* GetDefaultMaterial();
        static CMaterial* GetDefaultTerrainMaterial();

        static void CreateDefaultMaterial();
        static void CreateDefaultTerrainMaterial();

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

        PROPERTY()
        TVector<TObjectPtr<CTexture>>           Textures;

        PROPERTY()
        TVector<uint32>                         PixelShaderBinaries;

        PROPERTY()
        TVector<uint32>                         VertexShaderBinaries;

        /** Mesh-shader geometry stage (MeshletMesh.slang); empty if mesh shaders weren't compiled. */
        PROPERTY()
        TVector<uint32>                         MeshShaderBinaries;

        /** VisBuffer geometry stage (MeshletVisBuffer.slang); empty if not compiled. */
        PROPERTY()
        TVector<uint32>                         VisBufferMeshShaderBinaries;

        /** VisBuffer geometry stage, vertex-emulation fallback (MeshletVisBufferVS.slang). */
        PROPERTY()
        TVector<uint32>                         VisBufferVertexShaderBinaries;

        /** Masked-only VisBuffer pixel stage, VS path (VisBufferMaskedPixel.slang): opacity clip; empty for non-masked. */
        PROPERTY()
        TVector<uint32>                         MaskedVisBufferPixelShaderBinaries;

        /** Masked-only VisBuffer pixel stage, mesh path (VisBufferMaskedPixel.slang + VISBUFFER_PRIMID). */
        PROPERTY()
        TVector<uint32>                         MaskedVisBufferPixelShaderPrimBinaries;

        /** Deferred material pixel stage (DeferredMaterial.slang); empty if not compiled. */
        PROPERTY()
        TVector<uint32>                         DeferredShaderBinaries;

        PROPERTY()
        TVector<FMaterialParameter>             Parameters;

        FMaterialUniforms                       MaterialUniforms;

        // Library entries keyed by asset GUID; recompiles refresh them in place.
        const FShaderEntry*                     VertexShader = nullptr;
        const FShaderEntry*                     PixelShader = nullptr;
        const FShaderEntry*                     MeshShader = nullptr;
        const FShaderEntry*                     VisBufferMeshShader = nullptr;
        const FShaderEntry*                     VisBufferVertexShader = nullptr;
        const FShaderEntry*                     MaskedVisBufferPixelShader = nullptr;
        const FShaderEntry*                     MaskedVisBufferPixelShaderPrim = nullptr;
        const FShaderEntry*                     DeferredShader = nullptr;

    protected:
        
        void UpdateMaterialUniforms() override;

    private:

        void RebuildParameterLookup();

        /** Instance back-references; instances unregister in OnDestroy so raw pointers are safe. */
        TVector<CMaterialInstance*>             Instances;

        THashMap<FName, FMaterialParameter>     ParameterLookup;
    };
    
}
