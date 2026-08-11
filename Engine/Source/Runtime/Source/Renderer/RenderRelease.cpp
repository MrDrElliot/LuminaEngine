#include "RuntimePCH.h"
#include "RenderRelease.h"

#include "MaterialManager.h"
#include "RenderManager.h"
#include "Core/Threading/Thread.h"

namespace Lumina::RHI
{
    void FRenderReleaseQueue::Post(const FRenderRelease& Release)
    {
        FScopeLock Lock(Mutex);

        FPending Item;
        Item.Release    = Release;
        Item.Generation = ExtractGeneration;
        Pending.push_back(Item);
    }

    void FRenderReleaseQueue::BeginExtract()
    {
        FScopeLock Lock(Mutex);
        ++ExtractGeneration;
    }

    void FRenderReleaseQueue::EndRender()
    {
        TVector<FRenderRelease> Ready;

        {
            FScopeLock Lock(Mutex);

            // The extract that was just rendered is the one BeginExtract opened this frame.
            RenderedGeneration = ExtractGeneration;

            for (size_t i = 0; i < Pending.size(); )
            {
                if (Pending[i].Generation < RenderedGeneration)
                {
                    Ready.push_back(Pending[i].Release);
                    Pending[i] = Pending.back();
                    Pending.pop_back();
                }
                else
                {
                    ++i;
                }
            }
        }

        // Outside the lock: ReleaseNow reaches into the material manager and the RHI retire queue, both of
        // which take their own locks, and neither ordering should have to be reasoned about.
        for (FRenderRelease& Item : Ready)
        {
            ReleaseNow(Item);
        }
    }

    void FRenderReleaseQueue::FlushAll()
    {
        TVector<FRenderRelease> Ready;
        {
            FScopeLock Lock(Mutex);
            for (FPending& Item : Pending)
            {
                Ready.push_back(Item.Release);
            }
            Pending.clear();
        }

        for (FRenderRelease& Item : Ready)
        {
            ReleaseNow(Item);
        }
    }

    uint32 FRenderReleaseQueue::NumPending() const
    {
        FScopeLock Lock(Mutex);
        return (uint32)Pending.size();
    }

    void FRenderReleaseQueue::ReleaseNow(FRenderRelease& Item)
    {
        // TryRender, not Render: a token posted late in teardown can outlive the manager, and it must
        // release quietly rather than assert. The RHI half below stands on its own either way.
        if (Item.MaterialSlot != -1)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RenderManager->GetMaterialManager().RemoveMaterialSlot((uint32)Item.MaterialSlot);
            }
            Item.MaterialSlot = -1;
        }

        if (Item.Texture.IsValid())
        {
            // Unchanged from the direct path -- this still unbinds the slot immediately and fences the
            // image through Core::Retire. What gate 1 bought is that no frame recorded from here on can
            // name it, which is exactly the guarantee the immediate unbind could not provide on its own.
            Textures::Release(Item.Texture);
        }
    }
}
