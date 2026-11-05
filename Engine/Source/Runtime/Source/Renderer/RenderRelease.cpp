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
        Pending.Add(Item);
    }

    void FRenderReleaseQueue::BeginExtract()
    {
        FScopeLock Lock(Mutex);
        ++ExtractGeneration;
    }

    void FRenderReleaseQueue::EndRender()
    {
        static thread_local TVector<FRenderRelease> Ready;
        Ready.Reset();

        {
            FScopeLock Lock(Mutex);

            // The extract that was just rendered is the one BeginExtract opened this frame.
            RenderedGeneration = ExtractGeneration;

            for (int32 i = 0; i < Pending.Num(); )
            {
                if (Pending[i].Generation < RenderedGeneration)
                {
                    Ready.Add(Pending[i].Release);
                    Pending[i] = Pending.Last();
                    Pending.Pop();
                }
                else
                {
                    ++i;
                }
            }
        }

        // Outside the lock, since ReleaseNow reaches into two subsystems that take their own locks.
        for (FRenderRelease& Item : Ready)
        {
            ReleaseNow(Item);
        }

        // Parked storage keeps its capacity, but the released records must not outlive this call.
        Ready.Reset();
    }

    void FRenderReleaseQueue::FlushAll()
    {
        TVector<FRenderRelease> Ready;
        {
            FScopeLock Lock(Mutex);
            for (FPending& Item : Pending)
            {
                Ready.Add(Item.Release);
            }
            Pending.Reset();
        }

        for (FRenderRelease& Item : Ready)
        {
            ReleaseNow(Item);
        }
    }

    uint32 FRenderReleaseQueue::NumPending() const
    {
        FScopeLock Lock(Mutex);
        return (uint32)Pending.Num();
    }

    void FRenderReleaseQueue::ReleaseNow(FRenderRelease& Item)
    {
        // A token posted late in teardown can outlive the manager and must release quietly.
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
            // What the gate bought is that no frame recorded from here on can name it.
            Textures::Release(Item.Texture);
        }

        if (Item.Buffer.Gpu != 0)
        {
            Retire(Item.Buffer);
            Item.Buffer = {};
        }
    }

    void RetireAfterExtract(const FGPUAllocation& Block)
    {
        if (Block.Gpu == 0)
        {
            return;
        }

        FRenderRelease Release;
        Release.Buffer = Block;

        if (FRenderManager* RenderManager = TryRender())
        {
            RenderManager->GetReleaseQueue().Post(Release);
            return;
        }

        // No renderer means no extract can be holding it, so the GPU fence is the whole contract.
        FRenderReleaseQueue::ReleaseNow(Release);
    }
}
