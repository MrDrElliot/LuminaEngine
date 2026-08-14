#include "RuntimePCH.h"
#include "MaterialInterface.h"

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

    void CMaterialInterface::RefreshSubtree()
    {
        RefreshFromParent();
        PropagateToChildren();
    }

    void CMaterialInterface::PropagateToChildren(uint32 Depth)
    {
        // What terminates a cycle: the parent is serialized, so a corrupt asset can present one anyway.
        if (Depth >= MaxChainDepth)
        {
            LOG_ERROR("Material '{}': instance chain deeper than {} levels, or cyclic; refresh stopped.",
                GetName(), MaxChainDepth);
            return;
        }

        // Snapshot then unlock: holding every level's lock down the chain is a lock-order hazard for nothing.
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
