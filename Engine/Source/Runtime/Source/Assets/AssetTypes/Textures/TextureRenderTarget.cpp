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
        
        if (TextureResource->NewTexture.IsValid()
         && TextureResource->ImageDescription.Extent == FUIntVector2(W, H)
         && TextureResource->ImageDescription.Format == GetRHIFormat())
        {
            return;
        }

        FTextureResource::FDescription& Desc = TextureResource->ImageDescription;
        Desc = FTextureResource::FDescription{};
        Desc.Extent  = FUIntVector2(W, H);
        Desc.Format  = GetRHIFormat();
        Desc.NumMips = 1;

        RHI::Textures::Release(TextureResource->NewTexture);
        TextureResource->NewTexture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width    = W,
            .Height   = H,
            .Format   = GetRHIFormat(),
            .bStorage = true,
            .DebugName = "TextureRenderTarget",
        });

        const float Clear[4] = { ClearColor.r, ClearColor.g, ClearColor.b, ClearColor.a };
        RHI::Textures::Clear(TextureResource->NewTexture, Clear);
    }

    void CTextureRenderTarget::Update(const void* Pixels, uint64 SizeBytes, uint32 InWidth, uint32 InHeight)
    {
        if (Pixels == nullptr || SizeBytes == 0 || InWidth == 0 || InHeight == 0)
        {
            return;
        }

        // Resize before uploading, also handles the image not existing yet.
        if (Width != InWidth || Height != InHeight || TextureResource == nullptr || !TextureResource->NewTexture.IsValid())
        {
            Width  = InWidth;
            Height = InHeight;
            BuildResource();
        }

        // RowPitchTexels is the mip's own width.
        RHI::Textures::Upload(TextureResource->NewTexture, 0, Pixels, SizeBytes, InWidth, InWidth, InHeight);
    }
}
