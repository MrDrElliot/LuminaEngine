#include "RuntimePCH.h"
#include "MaterialInterface.h"

#include "Core/Engine/Engine.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"
#include "Log/Log.h"

namespace Lumina
{
    float CMaterialInterface::GetScalarValue(const FName& Name, float Default)
    {
        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Scalar, Name, Param) || Param.Index >= MAX_SCALARS)
        {
            return Default;
        }

        // This level's block, so a chained instance reports the value it actually draws with.
        const FMaterialUniforms* Uniforms = GetMaterialUniforms();
        return Uniforms != nullptr ? Uniforms->Scalars[Param.Index] : Default;
    }

    FVector4 CMaterialInterface::GetVectorValue(const FName& Name, FVector4 Default)
    {
        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Vector, Name, Param) || Param.Index >= MAX_VECTORS)
        {
            return Default;
        }

        const FMaterialUniforms* Uniforms = GetMaterialUniforms();
        return Uniforms != nullptr ? Uniforms->Vectors[Param.Index] : Default;
    }

    CTexture* CMaterialInterface::GetTextureValue(const FName& Name)
    {
        FMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Texture, Name, Param))
        {
            return nullptr;
        }

        return GetTextureParameterTexture(Name, Param.Index);
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

    void CMaterialInterface::RegisterChild(CMaterialInterface* Child)
    {
        if (Child == nullptr || Child == this)
        {
            return;
        }

        // Children of one parent PostLoad concurrently on worker fibers (the parallel leaf-first wave).
        FScopeLock Lock(ChildrenMutex);
        for (CMaterialInterface* Existing : Children)
        {
            if (Existing == Child)
            {
                return;
            }
        }
        Children.push_back(Child);
    }

    void CMaterialInterface::UnregisterChild(CMaterialInterface* Child)
    {
        if (Child == nullptr)
        {
            return;
        }

        FScopeLock Lock(ChildrenMutex);
        for (auto It = Children.begin(); It != Children.end(); ++It)
        {
            if (*It == Child)
            {
                Children.erase(It);
                return;
            }
        }
    }

    uint32 CMaterialInterface::GetResolvedTextureSlot(uint32 Index)
    {
        return RHI::Textures::DefaultResourceID();
    }

    void CMaterialInterface::UploadUniformField(uint32 ByteOffset, const void* Data, uint32 ByteSize)
    {
        // A slot is only ever handed out by the material manager, so holding one implies a renderer.
        if (MaterialIndex != -1)
        {
            Render().GetMaterialManager().UpdateMaterialUniformRange((uint32)MaterialIndex, ByteOffset, Data, ByteSize);
        }
    }

    void CMaterialInterface::PropagateParameterToChildren(EMaterialParameterType Type, const FName& Name,
        uint16 Index, uint32 Depth)
    {
        if (Depth >= MaxChainDepth)
        {
            return;
        }

        TVector<CMaterialInterface*> Snapshot;
        {
            FScopeLock Lock(ChildrenMutex);
            Snapshot = Children;
        }

        for (CMaterialInterface* Child : Snapshot)
        {
            if (Child == nullptr || Child->GetParentMaterial() != this)
            {
                continue;
            }

            // A child that overrides this parameter keeps its own value, and so does everything under it.
            if (Child->InheritParameterValue(Type, Name, Index))
            {
                Child->PropagateParameterToChildren(Type, Name, Index, Depth + 1);
            }
        }
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
            LOG_ERROR("Material '{}': instance chain deeper than {} levels, or cyclic; refresh stopped.",
                GetName(), MaxChainDepth);
            return;
        }

        // Holding every level's lock down the chain is a lock-order hazard for nothing.
        TVector<CMaterialInterface*> Snapshot;
        {
            FScopeLock Lock(ChildrenMutex);
            Snapshot = Children;
        }

        for (CMaterialInterface* Child : Snapshot)
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

        TVector<CMaterialInterface*> Snapshot;
        {
            FScopeLock Lock(ChildrenMutex);
            Snapshot = Children;
        }

        for (CMaterialInterface* Child : Snapshot)
        {
            if (Child != nullptr && Child->GetParentMaterial() == this)
            {
                Child->RefreshInheritedTextureSlots();
                Child->PropagateInheritedTextureSlots(Depth + 1);
            }
        }
    }
}
