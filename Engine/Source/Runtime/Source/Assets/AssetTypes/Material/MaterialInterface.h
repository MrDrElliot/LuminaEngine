#pragma once

#include "Renderer/ShaderHandle.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "Core/Threading/Thread.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/RHIFwd.h"
#include "Renderer/Vertex.h"
#include "MaterialInterface.generated.h"

namespace Lumina
{
    class CMaterial;
    class CTexture;
}

namespace Lumina
{
    REFLECT()
    enum class EMaterialType : uint8
    {
        None,
        PBR,
        PostProcess,
        UI,
        Terrain,
        Decal,

        // Sprite particles, expanded to billboard quads and shaded unlit into HDR.
        Particle,

        // A mask a light projects, rendered into the light-function atlas and multiplied into its radiance.
        LightFunction,
    };

    REFLECT()
    enum class EBlendMode : uint8
    {
        Opaque,
        Masked,
        Translucent,
        Additive,

        // Multiplies the scene by the material color, which is commutative and so needs no sorting.
        Modulate,

        // Alpha blending over premultiplied color, resolved through OIT like Translucent.
        AlphaComposite,
    };

    // Packed into the GBuffer flags and read back by lighting, so the values match EShadingModel in GBuffer.slang.
    REFLECT()
    enum class EMaterialShadingModel : uint8
    {
        Lit       = 0,
        Unlit     = 1,

        // A clear dielectric layer over the base, as on car paint, lacquer and varnish.
        Clearcoat = 2,

        // Thin two-sided translucency for leaves and grass, paid for with the emissive channels.
        Foliage   = 3,
    };

    // Every compiled stage a material can carry, each named by a row of the stage table in Material.cpp.
    enum class EMaterialShaderStage : uint8
    {
        Pixel,
        Vertex,                     // Non-meshlet domains only, since PBR geometry is mesh shaded end to end.
        MeshShadow,                 // MeshletMesh.slang, depth only.
        MeshBase,                   // MeshletMesh.slang with MESHLET_MESH_BASE, for blended geometry.
        VisBufferMesh,              // MeshletVisBuffer.slang, position only.
        VisBufferMeshMasked,        // MeshletVisBuffer.slang with VISBUFFER_MASKED_GEOM.
        MaskedVisBufferPixel,       // VisBufferMaskedPixel.slang, the alpha test for the visibility buffer.
        Deferred,                   // DeferredMaterial.slang.
        MeshShadowMasked,           // MeshletMesh.slang with MESHLET_MESH_MASKED_SHADOW.
        ShadowMaskedPixel,          // ShadowMaskedPixel.slang, the alpha test for shadow maps.

        Count,
    };

    // What each domain consumes of a material, asked by the editor, the graph compile and the passes alike.
    namespace MaterialDomain
    {
        constexpr bool IsMeshlet(EMaterialType Type)        { return Type == EMaterialType::PBR; }
        constexpr bool UsesVertexStage(EMaterialType Type)  { return Type != EMaterialType::PBR && Type != EMaterialType::None; }
        constexpr bool IsFullscreen(EMaterialType Type)
        {
            return Type == EMaterialType::PostProcess || Type == EMaterialType::UI || Type == EMaterialType::LightFunction;
        }

        constexpr bool SupportsBlendMode(EMaterialType Type, EBlendMode Mode)
        {
            switch (Type)
            {
            case EMaterialType::PBR:
            case EMaterialType::Particle: return true;
            case EMaterialType::Terrain:  return Mode == EBlendMode::Opaque || Mode == EBlendMode::Masked;
            default:                      return Mode == EBlendMode::Opaque;
            }
        }

        // Lit through ShadeSurface, which reads the model off the material flags at runtime.
        constexpr bool SupportsShadingModel(EMaterialType Type)       { return Type == EMaterialType::PBR || Type == EMaterialType::Terrain; }

        // Shadow casting, two-sidedness, decal receipt and depth test only mean something on a mesh surface.
        constexpr bool SupportsSurfaceRenderState(EMaterialType Type) { return Type == EMaterialType::PBR; }
        constexpr bool SupportsDepthWrite(EMaterialType Type)         { return Type == EMaterialType::PBR || Type == EMaterialType::Particle; }

        RUNTIME_API const char* ToString(EMaterialType Type);
    }

    // A base material or an instance of one. Parameters, textures and shaders are declared by the root, and every level owns a GPU uniform block.
    REFLECT()
    class RUNTIME_API CMaterialInterface : public CObject
    {
        GENERATED_BODY()
    public:

        CMaterialInterface();

        // Longest parent chain allowed, since resolution is linear in depth and every level costs a GPU slot.
        static constexpr uint32 MaxChainDepth = 8;

        // The immediate parent, null on a base material.
        FUNCTION()
        virtual CMaterialInterface* GetParentMaterial() const { return nullptr; }

        // The root base material.
        FUNCTION()
        virtual CMaterial* GetMaterial() const { return nullptr; }

        // Idempotent, since children of one parent register concurrently during the parallel PostLoad wave.
        void RegisterChild(CMaterialInterface* Child);
        void UnregisterChild(CMaterialInterface* Child);

        // Refreshes this level and then every descendant.
        void RefreshSubtree();

        // Depth-first RefreshFromParent over every descendant, excluding this level.
        void PropagateToChildren(uint32 Depth = 0);

        // Depth-first RefreshInheritedTextureSlots over every descendant.
        void PropagateInheritedTextureSlots(uint32 Depth = 0);

        // Pushes one parameter's value down the subtree, skipping every branch that overrides it.
        void PropagateParameterToChildren(EMaterialParameterType Type, const FName& Name, uint16 Index, uint32 Depth = 0);

        // Re-derives this level from its parent and uploads the result.
        virtual void RefreshFromParent() { }

        // Copies the parent's texture slots into every slot this level does not override.
        virtual void RefreshInheritedTextureSlots() { }

        // Adopts the parent's value for one parameter, returning false when this level overrides it.
        virtual bool InheritParameterValue(EMaterialParameterType Type, const FName& Name, uint16 Index) { return false; }

        FUNCTION()
        virtual bool SetVectorValue(const FName& Name, const FVector4& Value) { return false; }

        FUNCTION()
        virtual bool SetScalarValue(const FName& Name, const float Value) { return false; }

        // Only an instance can diverge on a texture, so a base material refuses this.
        FUNCTION()
        virtual bool SetTextureValue(const FName& Name, CTexture* Value) { return false; }

        // This level's effective value, or Default when the root declares no such parameter.
        FUNCTION()
        float GetScalarValue(const FName& Name, float Default = 0.0f);

        FUNCTION()
        FVector4 GetVectorValue(const FName& Name, FVector4 Default = FVector4(0.0f));

        // The texture bound at this level for Name, found by walking up the chain.
        FUNCTION()
        CTexture* GetTextureValue(const FName& Name);

        FUNCTION()
        bool HasScalarParameter(const FName& Name);

        FUNCTION()
        bool HasVectorParameter(const FName& Name);

        FUNCTION()
        bool HasTextureParameter(const FName& Name);

        // The root's declaration of Name, which every level shares.
        virtual bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) { return false; }

        FMaterialUniforms* GetMaterialUniforms() { return &MaterialUniforms; }
        const FMaterialUniforms* GetMaterialUniforms() const { return &MaterialUniforms; }

        int32 GetMaterialIndex() const { return MaterialIndex; }
        void SetMaterialIndex(int32 Index) { MaterialIndex = Index; }

        EMaterialType GetMaterialType() const;
        EBlendMode GetBlendMode() const;
        virtual EMaterialShadingModel GetShadingModel() const;
        bool DoesCastShadows() const;
        bool IsTwoSided() const;

        // False keeps DBuffer decals off the surface, which skin, glass, foliage and water all want.
        bool ReceivesDecals() const;

        // Depth write for the unordered blend passes, which the OIT lane cannot honor.
        bool WritesDepth() const;

        // Casts into shadow maps but is culled from every camera view, for invisible shadow proxies.
        bool IsShadowOnly() const;

        // Accumulated into the weighted blended OIT targets rather than blended in draw order.
        bool IsOITResolved() const;

        // Blends straight into scene color with a commutative operator, so it needs no sorting.
        bool IsUnorderedBlend() const;

        // The permutation this level selects, derived per call so a root recompile cannot leave it stale.
        virtual uint64 GetStaticSwitchKey() const { return 0; }

        // Stage at this level's permutation, falling back to the root's default set.
        FShaderH GetShader(EMaterialShaderStage Stage) const;
        FShaderH GetVertexShader() const { return GetShader(EMaterialShaderStage::Vertex); }
        FShaderH GetPixelShader() const { return GetShader(EMaterialShaderStage::Pixel); }

        // Ready to draw and rooted in a material compiled for Domain. PBR resolves through FMeshResolveCache instead.
        bool IsUsableInDomain(EMaterialType Domain) const;

        // IsUsableInDomain plus both raster stages at this level's permutation, leaving the outputs null on failure.
        bool ResolveDomainShaders(EMaterialType Domain, FShaderH& OutVertex, FShaderH& OutPixel) const;

        void SetReadyForRender(bool bReady) { bReadyForRender.store(bReady, std::memory_order_release); }

        // An instance also needs its whole parent chain ready, since the shaders live at the root.
        virtual bool IsReadyForRender() const { return bReadyForRender.load(std::memory_order_acquire); }

        // The bindless ID this level's block holds for texture slot Index.
        virtual uint32 GetResolvedTextureSlot(uint32 Index);

        // The texture bound to parameter Name at slot Index, found by walking up the chain.
        virtual CTexture* GetTextureParameterTexture(const FName& Name, uint32 Index) { return nullptr; }

        // Re-bakes texture IDs when this level binds ChangedTexture, or always when it is null, so a slot baked before residency heals.
        virtual bool RefreshTextureBindings(const CTexture* ChangedTexture) { return false; }

        // Non-blocking, since it runs on an Extract worker. False kicks async loads and asks the caller to draw the default material for now.
        virtual bool RequestTexturesResolved() { return true; }

    protected:

        // Takes a GPU slot when this level has none, otherwise uploads the whole block to it.
        void AcquireOrUploadMaterialSlot();

        // Hands the slot back to the renderer's release queue, before the index can be recycled.
        void ReleaseMaterialSlot();

        // Uploads the whole block. A slot is only ever handed out by the material manager, so holding one implies a renderer.
        void UploadMaterialUniforms();

        // Uploads one field of the block that has already been written.
        void UploadUniformField(uint32 ByteOffset, const void* Data, uint32 ByteSize);

        // Write one slot of the block and upload it, ignoring indices past the budget.
        void WriteScalarSlot(uint32 Index, float Value);
        void WriteVectorSlot(uint32 Index, const FVector4& Value);
        void WriteTextureSlot(uint32 Index, uint32 ResourceID);

        // A snapshot, so no lock is held down the chain while each child refreshes.
        TVector<CMaterialInterface*> SnapshotChildren();

        FMaterialUniforms               MaterialUniforms;
        int32                           MaterialIndex = -1;
        std::atomic_bool                bReadyForRender { false };

        // Raw pointers, since a child unregisters itself in its OnDestroy.
        TVector<CMaterialInterface*>    Children;
        FMutex                          ChildrenMutex;
    };
}
