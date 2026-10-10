#include "RuntimePCH.h"
#include "MaterialInterface.h"
#include "Material.h"

#include "Core/Engine/Engine.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"
#include "Log/Log.h"

namespace Lumina
{
    const char* MaterialDomain::ToString(EMaterialType Type)
    {
        switch (Type)
        {
        case EMaterialType::None:        return "None";
        case EMaterialType::PBR:         return "PBR";
        case EMaterialType::PostProcess: return "PostProcess";
        case EMaterialType::UI:          return "UI";
        case EMaterialType::Terrain:     return "Terrain";
        case EMaterialType::Decal:       return "Decal";
        case EMaterialType::Particle:    return "Particle";
        case EMaterialType::LightFunction: return "LightFunction";
        }
        return "Unknown";
    }

    CMaterialInterface::CMaterialInterface()
    {
        Memory::Memzero(&MaterialUniforms, sizeof(FMaterialUniforms));
    }

    void CMaterialInterface::RegisterChild(CMaterialInterface* Child)
    {
        if (Child == nullptr || Child == this)
        {
            return;
        }

        FScopeLock Lock(ChildrenMutex);
        if (!Algo::Contains(Children, Child))
        {
            Children.push_back(Child);
        }
    }

    void CMaterialInterface::UnregisterChild(CMaterialInterface* Child)
    {
        FScopeLock Lock(ChildrenMutex);
        if (auto It = Algo::Find(Children, Child); It != Children.end())
        {
            Children.erase(It);
        }
    }

    TVector<CMaterialInterface*> CMaterialInterface::SnapshotChildren()
    {
        FScopeLock Lock(ChildrenMutex);
        return Children;
    }

    void CMaterialInterface::RefreshSubtree()
    {
        RefreshFromParent();
        PropagateToChildren();
    }

    void CMaterialInterface::PropagateToChildren(uint32 Depth)
    {
        // The parent is serialized, so a corrupt asset can present a cycle anyway.
        if (Depth >= MaxChainDepth)
        {
            LOG_ERROR("Material '{}': instance chain deeper than {} levels, or cyclic; refresh stopped.", GetName(), MaxChainDepth);
            return;
        }

        for (CMaterialInterface* Child : SnapshotChildren())
        {
            if (Child != nullptr && Child->GetParentMaterial() == this)
            {
                Child->RefreshFromParent();
                Child->PropagateToChildren(Depth + 1);
            }
        }
    }

    void CMaterialInterface::PropagateInheritedTextureSlots(uint32 Depth)
    {
        if (Depth >= MaxChainDepth)
        {
            return;
        }

        for (CMaterialInterface* Child : SnapshotChildren())
        {
            if (Child != nullptr && Child->GetParentMaterial() == this)
            {
                Child->RefreshInheritedTextureSlots();
                Child->PropagateInheritedTextureSlots(Depth + 1);
            }
        }
    }

    void CMaterialInterface::PropagateParameterToChildren(EMaterialParameterType Type, const FName& Name, uint16 Index, uint32 Depth)
    {
        if (Depth >= MaxChainDepth)
        {
            return;
        }

        for (CMaterialInterface* Child : SnapshotChildren())
        {
            // A child that overrides the parameter keeps its value, and so does everything under it.
            if (Child != nullptr && Child->GetParentMaterial() == this && Child->InheritParameterValue(Type, Name, Index))
            {
                Child->PropagateParameterToChildren(Type, Name, Index, Depth + 1);
            }
        }
    }

    float CMaterialInterface::GetScalarValue(const FName& Name, float Default)
    {
        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Scalar, Name, Param) || Param.Index >= MAX_SCALARS)
        {
            return Default;
        }

        // This level's block, so a chained instance reports the value it actually draws with.
        return MaterialUniforms.Scalars[Param.Index];
    }

    FVector4 CMaterialInterface::GetVectorValue(const FName& Name, FVector4 Default)
    {
        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Vector, Name, Param) || Param.Index >= MAX_VECTORS)
        {
            return Default;
        }
        return MaterialUniforms.Vectors[Param.Index];
    }

    CTexture* CMaterialInterface::GetTextureValue(const FName& Name)
    {
        FMaterialParameter Param;
        return GetParameterValue(EMaterialParameterType::Texture, Name, Param) ? GetTextureParameterTexture(Name, Param.Index) : nullptr;
    }

    bool CMaterialInterface::HasScalarParameter(const FName& Name)
    {
        FMaterialParameter Param;
        return GetParameterValue(EMaterialParameterType::Scalar, Name, Param);
    }

    bool CMaterialInterface::HasVectorParameter(const FName& Name)
    {
        FMaterialParameter Param;
        return GetParameterValue(EMaterialParameterType::Vector, Name, Param);
    }

    bool CMaterialInterface::HasTextureParameter(const FName& Name)
    {
        FMaterialParameter Param;
        return GetParameterValue(EMaterialParameterType::Texture, Name, Param);
    }

    void CMaterialInterface::AcquireOrUploadMaterialSlot()
    {
        if (MaterialIndex != -1)
        {
            UploadMaterialUniforms();
            return;
        }

        // Headless has no material table, and every consumer already treats -1 as no GPU parameters.
        if (FRenderManager* RenderManager = TryRender())
        {
            RenderManager->GetMaterialManager().AddMaterial(this);
        }
    }

    void CMaterialInterface::ReleaseMaterialSlot()
    {
        if (MaterialIndex == -1)
        {
            return;
        }

        // A material outliving the renderer releases quietly rather than asserting.
        if (FRenderManager* RenderManager = TryRender())
        {
            RHI::FRenderRelease Release;
            Release.MaterialSlot = MaterialIndex;
            RenderManager->GetReleaseQueue().Post(Release);
        }
        MaterialIndex = -1;
    }

    void CMaterialInterface::UploadMaterialUniforms()
    {
        if (MaterialIndex != -1)
        {
            Render().GetMaterialManager().UpdateMaterialUniforms(&MaterialUniforms, (uint32)MaterialIndex);
        }
    }

    void CMaterialInterface::UploadUniformField(uint32 ByteOffset, const void* Data, uint32 ByteSize)
    {
        if (MaterialIndex != -1)
        {
            Render().GetMaterialManager().UpdateMaterialUniformRange((uint32)MaterialIndex, ByteOffset, Data, ByteSize);
        }
    }

    void CMaterialInterface::WriteScalarSlot(uint32 Index, float Value)
    {
        if (Index < MAX_SCALARS)
        {
            MaterialUniforms.Scalars[Index] = Value;
            UploadUniformField(ScalarFieldOffset(Index), &MaterialUniforms.Scalars[Index], sizeof(float));
        }
    }

    void CMaterialInterface::WriteVectorSlot(uint32 Index, const FVector4& Value)
    {
        if (Index < MAX_VECTORS)
        {
            MaterialUniforms.Vectors[Index] = Value;
            UploadUniformField(VectorFieldOffset(Index), &MaterialUniforms.Vectors[Index], sizeof(FVector4));
        }
    }

    void CMaterialInterface::WriteTextureSlot(uint32 Index, uint32 ResourceID)
    {
        if (Index < MAX_TEXTURES)
        {
            MaterialUniforms.Textures[Index] = ResourceID;
            UploadUniformField(TextureFieldOffset(Index), &MaterialUniforms.Textures[Index], sizeof(uint32));
        }
    }

    EMaterialType CMaterialInterface::GetMaterialType() const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr ? Root->MaterialType : EMaterialType::None;
    }

    EBlendMode CMaterialInterface::GetBlendMode() const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr ? Root->BlendMode : EBlendMode::Opaque;
    }

    EMaterialShadingModel CMaterialInterface::GetShadingModel() const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr ? Root->ShadingModel : EMaterialShadingModel::Lit;
    }

    bool CMaterialInterface::DoesCastShadows() const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr && Root->bCastShadows;
    }

    bool CMaterialInterface::IsTwoSided() const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr && Root->bTwoSided;
    }

    bool CMaterialInterface::ReceivesDecals() const
    {
        const CMaterial* Root = GetMaterial();
        return Root == nullptr || Root->bReceivesDecals;
    }

    bool CMaterialInterface::WritesDepth() const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr && Root->bWriteDepth;
    }

    bool CMaterialInterface::IsShadowOnly() const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr && Root->bShadowOnly;
    }

    bool CMaterialInterface::IsOITResolved() const
    {
        const EBlendMode Mode = GetBlendMode();
        return Mode == EBlendMode::Translucent || Mode == EBlendMode::AlphaComposite;
    }

    bool CMaterialInterface::IsUnorderedBlend() const
    {
        const EBlendMode Mode = GetBlendMode();
        return Mode == EBlendMode::Additive || Mode == EBlendMode::Modulate;
    }

    FShaderH CMaterialInterface::GetShader(EMaterialShaderStage Stage) const
    {
        const CMaterial* Root = GetMaterial();
        return Root != nullptr ? Root->GetStageForKey(Stage, GetStaticSwitchKey()) : FShaderH{};
    }

    bool CMaterialInterface::IsUsableInDomain(EMaterialType Domain) const
    {
        return GetMaterial() != nullptr && GetMaterialType() == Domain && IsReadyForRender();
    }

    bool CMaterialInterface::ResolveDomainShaders(EMaterialType Domain, FShaderH& OutVertex, FShaderH& OutPixel) const
    {
        OutVertex = {};
        OutPixel  = {};

        if (!MaterialDomain::UsesVertexStage(Domain) || !IsUsableInDomain(Domain))
        {
            return false;
        }

        const FShaderH Vertex = GetVertexShader();
        const FShaderH Pixel  = GetPixelShader();
        if (Vertex == nullptr || Pixel == nullptr)
        {
            return false;
        }

        OutVertex = Vertex;
        OutPixel  = Pixel;
        return true;
    }

    uint32 CMaterialInterface::GetResolvedTextureSlot(uint32 Index)
    {
        return RHI::Textures::DefaultResourceID();
    }
}
