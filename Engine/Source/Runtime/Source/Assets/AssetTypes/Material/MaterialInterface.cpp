#include "RuntimePCH.h"
#include "MaterialInterface.h"

#include "Core/Engine/Engine.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"
#include "Log/Log.h"

namespace Lumina
{
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
