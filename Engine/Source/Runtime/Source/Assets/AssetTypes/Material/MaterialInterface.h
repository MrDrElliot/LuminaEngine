#pragma once

#include "Renderer/ShaderHandle.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "Core/Threading/Thread.h"
#include "Renderer/RHIFwd.h"
#include "Renderer/Vertex.h"
#include "MaterialInterface.generated.h"

namespace Lumina
{
    struct FMaterialUniforms;
    class CMaterial;
    class CTexture;
    struct FMaterialParameter;
    enum class EMaterialParameterType : uint8;
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
    };

    REFLECT()
    enum class EBlendMode : uint8
    {
        Opaque,
        Masked,
        Translucent,
        Additive,
    };

    /**
     * How a surface is lit. Values MUST match EShadingModel in GBuffer.slang -- they are packed into the
     * GBuffer flags byte and read back by the lighting pass.
     *
     * Note the ordering: Lit is the shader's Default (0), so a material that never sets this behaves as it
     * always has.
     */
    REFLECT()
    enum class EMaterialShadingModel : uint8
    {
        Lit       = 0,
        Unlit     = 1,

        /** Adds a clear dielectric layer over the base: car paint, lacquer, varnish. */
        Clearcoat = 2,
    };

    REFLECT()
    class RUNTIME_API CMaterialInterface : public CObject
    {
        GENERATED_BODY()
    public:

        /** Immediate parent in the instance chain; null on a base material, which is always the root. */
        virtual CMaterialInterface* GetParentMaterial() const { return nullptr; }

        /** The root base material, which is the only level that DECLARES parameters, textures and stages. */
        virtual CMaterial* GetMaterial() const { return nullptr; }
        virtual bool SetVectorValue(const FName& Name, const FVector4& Value) { return false; }
        virtual bool SetScalarValue(const FName& Name, const float Value) { return false; }
        virtual bool GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param) { return false; }
        virtual FMaterialUniforms* GetMaterialUniforms() { return nullptr; }

        int32 GetMaterialIndex() const { return MaterialIndex; }

        void SetMaterialIndex(int32 Index) { MaterialIndex = Index; }

        virtual FShaderH GetVertexShader() const { return {}; }
        virtual FShaderH GetPixelShader() const { return {}; }

        virtual EMaterialType GetMaterialType() const { return EMaterialType::None; }

        virtual bool DoesCastShadows() const { return false; }
        virtual bool IsTwoSided() const { return false; }
        virtual bool IsTranslucent() { return false; }
        virtual bool IsMasked() { return false; }
        virtual bool IsAdditive() { return false; }
        virtual bool IsOpaque() { return true; }
        virtual bool IsUnlit() { return false; }
        virtual bool DisableDepthTest() { return false; }
        virtual EBlendMode GetBlendMode() { return EBlendMode::Opaque; }
        virtual EMaterialShadingModel GetShadingModel() { return EMaterialShadingModel::Lit; }
        virtual float GetOpacityMaskClipValue() { return 0.333f; }

        void SetReadyForRender(bool bReady) { bReadyForRender.store(bReady, std::memory_order_release); }
        bool IsReadyForRender() const { return bReadyForRender.load(std::memory_order_acquire); }

        /** Longest parent chain allowed. Resolution is linear in depth and every level costs a GPU slot. */
        static constexpr uint32 MaxChainDepth = 8;

        /** Idempotent. Children of one parent register concurrently during the parallel PostLoad wave. */
        void RegisterChild(CMaterialInterface* Child);
        void UnregisterChild(CMaterialInterface* Child);

        /** THIS level only. Re-derive values from the parent and push them to this material's GPU slot. */
        virtual void RefreshFromParent() { }

        /** Copy the parent's texture slots for every slot this level does not override. */
        virtual void RefreshInheritedTextureSlots() { }

        /** Bindless resource ID this level's resolved block holds for texture slot Index. */
        virtual uint32 GetResolvedTextureSlot(uint32 Index);

        /** The texture actually bound to parameter Name at slot Index, found by walking up the chain. */
        virtual CTexture* GetTextureParameterTexture(const FName& Name, uint32 Index) { return nullptr; }

        /** Refreshes this level and then every descendant. */
        void RefreshSubtree();

        /** Depth-first RefreshFromParent over every descendant, excluding this level. */
        void PropagateToChildren(uint32 Depth = 0);

        /** Depth-first RefreshInheritedTextureSlots over every descendant. */
        void PropagateInheritedTextureSlots(uint32 Depth = 0);

        /** Re-reads this material's texture ResourceIDs into its uniform block and re-uploads it, if it
         *  binds ChangedTexture (null = refresh unconditionally). Returns whether it did.
         *
         *  The bindless index itself is stable across a re-cook (RHI::Textures::Recreate repoints the slot
         *  rather than allocating a new one), so this is NOT about a moved slot. It is about the two cases
         *  where the baked value is simply WRONG: a texture that had no valid ResourceID when the block was
         *  written -- an asset that failed to cook, or was not resident yet -- was baked as the fallback and
         *  stays there forever, and a texture reference that was null at bake time never got written at all. */
        virtual bool RefreshTextureBindings(const CTexture* ChangedTexture) { return false; }

        /** True when every texture this material samples is GPU-resident. False kicks async loads for the
         *  ones that are not and asks the caller to fall back to the default material for now; the load
         *  completion calls FMeshResolveCache::InvalidateDependency to wake the surface.
         *
         *  Non-blocking: the resolve gate runs on a worker fiber inside Extract. */
        virtual bool RequestTexturesResolved() { return true; }

        /** Tell the texture streamer which textures this material's GPU slot samples.
         *
         *  The renderer reports screen coverage per material slot (an integer it already has in the draw
         *  path) and knows nothing about textures; this mapping is what turns that into per-texture
         *  residency demand. A stale mapping means a visible texture is never demanded and stays at its
         *  inline tail.
         *
         *  GAME THREAD ONLY -- it walks ResolvedTextures, which the async load completion writes. Every
         *  other thread marks/queues instead and lets the streamer's drain call this. */
    protected:

        virtual void UpdateMaterialUniforms() { }


        std::atomic_bool        bReadyForRender;

        int32                   MaterialIndex = -1;

        /** Instances parented to this level. Raw pointers; a child unregisters in its OnDestroy. */
        TVector<CMaterialInterface*>    Children;
        FMutex                          ChildrenMutex;
    };
}
