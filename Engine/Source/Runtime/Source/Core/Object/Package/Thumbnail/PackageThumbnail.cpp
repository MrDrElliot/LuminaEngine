#include "RuntimePCH.h"
#include "PackageThumbnail.h"

#include "Core/Serialization/Archiver.h"

#include "Tools/Image/ImageWrite.h"
#include "stb_image.h"

namespace Lumina
{
    namespace
    {
        // Identifies a valid on-disk thumbnail block; anything else (e.g. an absent thumbnail) is ignored.
        constexpr uint32 kThumbnailMagic   = 0x424D4854; // 'THMB'
        constexpr uint8  kThumbnailVersion = 1;

        enum class EThumbnailStorage : uint8
        {
            Raw = 0,
            PNG = 1,
        };

    }

    void FPackageThumbnail::Serialize(FArchive& Ar)
    {
        if (Ar.IsReading())
        {
            uint32 Magic = 0;
            Ar << Magic;
            if (Magic != kThumbnailMagic)
            {
                // An unrecognized thumbnail block is left empty and regenerates on the next save.
                ImageData.clear();
                ImageWidth  = 0;
                ImageHeight = 0;
                return;
            }

            uint8 Version = 0;
            Ar << Version;
            Ar << ImageWidth;
            Ar << ImageHeight;

            uint8 Storage = 0;
            Ar << Storage;

            TVector<uint8> Stored;
            Ar << Stored;

            if ((EThumbnailStorage)Storage == EThumbnailStorage::PNG && !Stored.empty())
            {
                int W = 0, H = 0, Channels = 0;
                stbi_uc* Decoded = stbi_load_from_memory(Stored.data(), (int)Stored.size(), &W, &H, &Channels, 4);
                if (Decoded != nullptr)
                {
                    ImageWidth  = (uint32)W;
                    ImageHeight = (uint32)H;
                    ImageData.assign(Decoded, Decoded + (size_t)W * (size_t)H * 4);
                    stbi_image_free(Decoded);
                }
                else
                {
                    ImageData.clear();
                }
            }
            else
            {
                ImageData = Move(Stored);
            }
        }
        else
        {
            uint32 Magic   = kThumbnailMagic;
            uint8  Version = kThumbnailVersion;
            Ar << Magic;
            Ar << Version;
            Ar << ImageWidth;
            Ar << ImageHeight;

            // Encode RGBA8 -> PNG (lossless); fall back to raw if there's nothing valid to encode.
            TVector<uint8> Encoded;
            uint8 Storage = (uint8)EThumbnailStorage::Raw;
            const bool bValidRGBA = !ImageData.empty()
                && ImageWidth > 0 && ImageHeight > 0
                && ImageData.size() == (size_t)ImageWidth * (size_t)ImageHeight * 4;

            if (bValidRGBA)
            {
                if (ImageWrite::EncodePng(Encoded, ImageWidth, ImageHeight, 4, ImageData.data(), ImageWidth * 4))
                {
                    Storage = (uint8)EThumbnailStorage::PNG;
                }
            }

            Ar << Storage;
            if ((EThumbnailStorage)Storage == EThumbnailStorage::PNG)
            {
                Ar << Encoded;
            }
            else
            {
                Ar << ImageData;
            }
        }
    }
}
