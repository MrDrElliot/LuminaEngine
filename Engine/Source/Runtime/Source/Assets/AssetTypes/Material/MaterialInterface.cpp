#include "RuntimePCH.h"
#include "MaterialInterface.h"

#include "Renderer/TextureStreamingManager.h"

namespace Lumina
{
    void CMaterialInterface::PublishStreamingTextures()
    {
        FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
        if (Streaming == nullptr)
        {
            return;
        }

        // A material with no GPU slot is not in any draw call, so no coverage will ever be reported against
        // it and there is nothing to key the mapping on. Note the dirty flag is deliberately NOT cleared on
        // this path (or the null-manager one above) -- the slot is assigned later, and a publish that never
        // happened must stay owed.
        if (MaterialIndex < 0)
        {
            return;
        }

        TVector<CTexture*> Collected;
        CollectStreamingTextures(Collected);

        Streaming->UpdateMaterialTextures((uint32)MaterialIndex, Collected);

        bStreamingTexturesDirty = false;
    }

    void CMaterialInterface::MarkStreamingTexturesDirty()
    {
        bStreamingTexturesDirty = true;

        // Queued rather than published here: this is called from async load completions and from
        // SetMaterialIndex, neither of which is a safe point to walk ResolvedTextures. The streamer drains
        // the queue on the game thread at the top of its Update.
        if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
        {
            Streaming->QueueMaterialPublish(this);
        }
    }

    void CMaterialInterface::PublishStreamingTexturesIfDirty()
    {
        if (bStreamingTexturesDirty)
        {
            PublishStreamingTextures();
        }
    }

    void CMaterialInterface::QueueStreamingPublishIfDirty()
    {
        if (!bStreamingTexturesDirty)
        {
            return;
        }

        if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
        {
            Streaming->QueueMaterialPublish(this);
        }
    }

    void CMaterialInterface::ForgetStreamingTextures()
    {
        if (MaterialIndex < 0)
        {
            return;
        }

        if (FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet())
        {
            Streaming->ForgetMaterial((uint32)MaterialIndex);
        }
    }
}
