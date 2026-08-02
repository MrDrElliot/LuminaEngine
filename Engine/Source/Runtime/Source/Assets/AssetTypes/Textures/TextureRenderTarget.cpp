#include "RuntimePCH.h"
#include "TextureRenderTarget.h"
#include "Core/Object/Class.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/RHITexture.h"

namespace Lumina
{
    void CTextureRenderTarget::Serialize(FArchive& Ar)
    {
        // Skip CTexture's pixel-mip blob; only the reflected properties persist (CObject::Serialize
        // walks them). The GPU image is rebuilt in PostLoad.
        CObject::Serialize(Ar);
    }

    void CTextureRenderTarget::PostLoad()
    {
        BuildResource();
    }

    EFormat CTextureRenderTarget::GetRHIFormat() const
    {
        switch (Format)
        {
        case ERenderTargetFormat::RGBA16F: return EFormat::RGBA16_FLOAT;
        case ERenderTargetFormat::RGBA8:
        default:                           return EFormat::RGBA8_UNORM;
        }
    }

    void CTextureRenderTarget::BuildResource()
    {
        LUMINA_MEMORY_SCOPE("Textures");

        if (!TextureResource)
        {
            TextureResource = MakeUnique<FTextureResource>();
        }

        // No CPU mip data: the target is GPU-only.
        TextureResource->Mips.clear();

        const uint32 W = Width  > 0 ? Width  : 1u;
        const uint32 H = Height > 0 ? Height : 1u;

        FTextureResource::FDescription& Desc = TextureResource->ImageDescription;
        Desc = FTextureResource::FDescription{};
        Desc.Extent  = FUIntVector2(W, H);
        Desc.Format  = GetRHIFormat();
        Desc.NumMips = 1;

        // New RHI: sampled (materials) + storage (paint compute UAV via Textures::StorageSlot).
        RHI::Textures::Release(TextureResource->NewTexture);
        TextureResource->NewTexture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width    = W,
            .Height   = H,
            .Format   = GetRHIFormat(),
            .bStorage = true,
        });

        // Clear so a sampler never reads uninitialized memory before the first paint/render.
        const float Clear[4] = { ClearColor.r, ClearColor.g, ClearColor.b, ClearColor.a };
        RHI::Textures::Clear(TextureResource->NewTexture, Clear);
    }
}
