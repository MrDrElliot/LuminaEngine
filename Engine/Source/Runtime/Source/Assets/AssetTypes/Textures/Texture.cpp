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

        // New RHI: create the sampled texture in the global heap + upload every mip.
        TextureResource->NewTexture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Desc.Extent.x,
            .Height = Desc.Extent.y,
            .Mips   = (uint32)TextureResource->Mips.size(),
            .Format = Desc.Format,
            .DebugName = DebugName.c_str(),
        });

        for (uint8 i = 0; i < TextureResource->Mips.size(); ++i)
        {
            const FTextureResource::FMip& Mip = TextureResource->Mips[i];
            // RowPitchTexels = mip width: pixel rows are tightly packed at the mip's width.
            RHI::Textures::Upload(TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width);
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
