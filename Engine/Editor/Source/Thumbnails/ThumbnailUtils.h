#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    struct FPackageThumbnail;

    namespace ThumbnailUtils
    {
        // Square resolution every generated thumbnail is stored at.
        inline constexpr uint32 kThumbnailResolution = 256;

        // Stores vertically flipped and resets LoadState, so the upload path flips it back and re-uploads.
        void StoreDownsampledRGBA(FPackageThumbnail& Thumbnail, const uint8* Source,
                                  uint32 SourceWidth, uint32 SourceHeight, size_t RowPitch);
    }
}
