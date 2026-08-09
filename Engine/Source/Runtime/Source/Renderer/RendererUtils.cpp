#include "RuntimePCH.h"
#include "RendererUtils.h"
#include "Tools/Import/ImportHelpers.h"

namespace Lumina::RenderUtils
{
    RHI::FManagedTexture CreateImageFromPixels(TSpan<const uint8> PixelData, bool bFlipVertically, FUIntVector2 Size)
    {
        TOptional<Import::Textures::FTextureImportResult> Result = Import::Textures::ImportTexture(PixelData, bFlipVertically, Size);
        if (!Result.has_value())
        {
            return {};
        }

        RHI::FManagedTexture Texture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Result->Dimensions.x,
            .Height = Result->Dimensions.y,
            .Format = Result->Format,
        });
        RHI::Textures::Upload(Texture, 0, Result->Pixels.data(), Result->Pixels.size(), Result->Dimensions.x);

        return Texture;
    }
}
