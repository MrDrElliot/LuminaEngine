#include "ThumbnailUtils.h"

#include "Core/Math/Math.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"

namespace Lumina::ThumbnailUtils
{
    void StoreDownsampledRGBA(FPackageThumbnail& Thumbnail, const uint8* Source,
                              uint32 SourceWidth, uint32 SourceHeight, size_t RowPitch)
    {
        if (Source == nullptr || SourceWidth == 0 || SourceHeight == 0)
        {
            return;
        }

        constexpr uint32 Size          = kThumbnailResolution;
        constexpr size_t BytesPerPixel = 4;

        Thumbnail.LoadState.store(FPackageThumbnail::EState::None, std::memory_order_relaxed);
        Thumbnail.ImageWidth  = Size;
        Thumbnail.ImageHeight = Size;
        Thumbnail.ImageData.SetNum(Size * Size * BytesPerPixel);

        uint8* DestData = Thumbnail.ImageData.GetData();

        const float ScaleX = static_cast<float>(SourceWidth) / Size;
        const float ScaleY = static_cast<float>(SourceHeight) / Size;

        for (uint32 DestY = 0; DestY < Size; ++DestY)
        {
            const uint32 FlippedDestY = Size - 1 - DestY;

            for (uint32 DestX = 0; DestX < Size; ++DestX)
            {
                const float SrcX = DestX * ScaleX;
                const float SrcY = DestY * ScaleY;

                const uint32 X0 = static_cast<uint32>(SrcX);
                const uint32 Y0 = static_cast<uint32>(SrcY);
                const uint32 X1 = Math::Min(X0 + 1, SourceWidth - 1);
                const uint32 Y1 = Math::Min(Y0 + 1, SourceHeight - 1);

                const float FracX = SrcX - X0;
                const float FracY = SrcY - Y0;

                const uint8* P00 = Source + (Y0 * RowPitch) + (X0 * BytesPerPixel);
                const uint8* P10 = Source + (Y0 * RowPitch) + (X1 * BytesPerPixel);
                const uint8* P01 = Source + (Y1 * RowPitch) + (X0 * BytesPerPixel);
                const uint8* P11 = Source + (Y1 * RowPitch) + (X1 * BytesPerPixel);

                uint8* DestPixel = DestData + (FlippedDestY * Size * BytesPerPixel) + (DestX * BytesPerPixel);

                for (size_t Channel = 0; Channel < BytesPerPixel; ++Channel)
                {
                    const float Top    = Math::Lerp(static_cast<float>(P00[Channel]), static_cast<float>(P10[Channel]), FracX);
                    const float Bottom = Math::Lerp(static_cast<float>(P01[Channel]), static_cast<float>(P11[Channel]), FracX);
                    const float Result = Math::Lerp(Top, Bottom, FracY);

                    DestPixel[Channel] = static_cast<uint8>(Result + 0.5f);
                }
            }
        }
    }
}
