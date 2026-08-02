#pragma once
#include "RenderResource.h"   // RHI::Format::BytesPerBlock
#include "RHITexture.h"
#include "Containers/Array.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    struct FTextureResource
    {
        struct FMip
        {
            uint32 Width;
            uint32 Height;
            uint32 Depth;
            uint32 RowPitch;
            uint32 SlicePitch;
            TVector<uint8> Pixels;
        };

        // Serialized texture description: exactly what Textures::Create needs at load.
        struct FDescription
        {
            FUIntVector2 Extent  = FUIntVector2(1, 1);
            uint8        NumMips = 1;
            EFormat      Format  = EFormat::UNKNOWN;

            friend FArchive& operator << (FArchive& Ar, FDescription& Data)
            {
                Ar << Data.Extent;
                Ar << Data.NumMips;
                Ar << Data.Format;
                return Ar;
            }
        };

        FDescription            ImageDescription;
        RHI::FManagedTexture    NewTexture;
        TFixedVector<FMip, 1>   Mips;

        uint64 CalcTotalSizeBytes() const
        {
            uint64 BytesPerBlock = RHI::Format::BytesPerBlock(ImageDescription.Format);
            uint64 TotalSize = 0;

            for (const FMip& Mip : Mips)
            {
                TotalSize += (uint64)Mip.RowPitch * Mip.Height * Mip.Depth;
            }

            return TotalSize;
        }

        friend FArchive& operator << (FArchive& Ar, FTextureResource& Data)
        {
            Ar << Data.ImageDescription;

            if (Ar.IsReading())
            {
                Data.Mips.clear();
                Data.Mips.resize(Data.ImageDescription.NumMips);
            }

            for (FMip& Mip : Data.Mips)
            {
                Ar << Mip.Width;
                Ar << Mip.Height;
                Ar << Mip.Depth;
                Ar << Mip.RowPitch;
                Ar << Mip.SlicePitch;
                Ar << Mip.Pixels;
            }

            return Ar;
        }
    };
}
