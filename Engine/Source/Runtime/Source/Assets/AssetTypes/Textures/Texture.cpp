#include "RuntimePCH.h"
#include "Texture.h"
#include "Core/Object/Class.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"

namespace Lumina
{
    void CTexture::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        Super::Serialize(Ar);

        if (!TextureResource)
        {
            TextureResource = MakeUnique<FTextureResource>();
        }

        Ar << *TextureResource.get();
    }

    void CTexture::PreLoad()
    {
        if (TextureResource == nullptr)
        {
            TextureResource = MakeUnique<FTextureResource>();
        }
    }

    void CTexture::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Textures");

        const FTextureResource::FDescription& Desc = TextureResource->ImageDescription;

        // Named after the asset so a GPU crash report identifies which texture a faulting address
        // belongs to. Read during Create only, so the local outliving the call is enough.
        const FString DebugName = "Texture." + GetName().ToString();

        const uint32 NumMips   = TextureResource->GetNumMips();
        const uint32 NumLayers = TextureResource->GetNumLayers();

        // New RHI: create the sampled texture in the global heap + upload every mip of every layer.
        // Both paths land in the same heap, so the ResourceID is interchangeable -- what decides
        // whether a shader may read it as gTextures2DArray[] is the VIEW type chosen here.
        // PostLoad is NOT once-per-object: the texture editor re-runs it after an in-place edit (flip
        // H/V), and the array factory re-runs it after re-cooking layers. Creating over the top left the
        // previous image and its heap slot orphaned -- one leaked texture per edit -- and moved the
        // ResourceID, which every material that samples this texture has already baked into its uniform
        // block. Both branches therefore have to deal with an existing image.
        if (TextureResource->IsArray())
        {
            // No array overload of Recreate: an array's layer count is baked into the image, so there is
            // no same-shape image to repoint the slot at. Release + create, and accept the new slot --
            // the same trade the array factory already makes explicitly.
            RHI::Textures::Release(TextureResource->NewTexture);
            TextureResource->NewTexture = RHI::Textures::Create(RHI::FTexture2DArrayDesc
            {
                .Width  = Desc.Extent.x,
                .Height = Desc.Extent.y,
                .Layers = NumLayers,
                .Mips   = NumMips,
                .Format = Desc.Format,
                .DebugName = DebugName.c_str(),
            });
        }
        else
        {
            // Repoints the existing heap slot and frame-defers the old image, so a re-run keeps the
            // published ResourceID valid. Falls back to a plain Create when the handle is not yet valid,
            // which is the normal first-load path.
            RHI::Textures::Recreate(TextureResource->NewTexture, RHI::FTexture2DDesc
            {
                .Width  = Desc.Extent.x,
                .Height = Desc.Extent.y,
                .Mips   = NumMips,
                .Format = Desc.Format,
                .DebugName = DebugName.c_str(),
            });
        }

        for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
        {
            for (uint32 i = 0; i < NumMips; ++i)
            {
                const uint32 Index = TextureResource->MipIndex(Layer, i);
                if (Index >= TextureResource->Mips.size())
                {
                    continue;
                }

                const FTextureResource::FMip& Mip = TextureResource->Mips[Index];
                // RowPitchTexels = mip width: pixel rows are tightly packed at the mip's width.
                RHI::Textures::UploadLayer(TextureResource->NewTexture, Layer, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width);
            }
        }

#if !USING(WITH_EDITOR)
        // CPU pixels are dead after upload in cooked builds; editor retains them for reimport/thumbnails.
        for (FTextureResource::FMip& Mip : TextureResource->Mips)
        {
            Mip.Pixels.clear();
            Mip.Pixels.shrink_to_fit();
        }
#endif
    }

    void CTexture::OnDestroy()
    {
        if (TextureResource)
        {
            RHI::Textures::Release(TextureResource->NewTexture);
        }
    }
}
