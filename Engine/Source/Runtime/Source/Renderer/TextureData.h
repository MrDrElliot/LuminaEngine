#pragma once
#include "RenderResource.h"   // RHI::Format::BytesPerBlock
#include "RHITexture.h"
#include "Containers/Array.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    /**
     * Mips at or below this on their long edge stay inline in the export and are always resident, so a
     * texture is never blank and never needs IO to be drawn -- the worst a streamed-out 4K texture looks
     * is like a 256px one. Everything above streams.
     *
     * 256 is chosen so the inline tail is negligible (~87 KiB for a 4K BC7, against ~21 MiB streamed)
     * while still being a usable image at distance. Raising it costs resident memory on EVERY texture;
     * lowering it makes the pop-in more visible.
     */
    constexpr uint32 kInlineMipMaxDimension = 256;

    struct FTextureResource
    {
        struct FMip
        {
            uint32 Width;
            uint32 Height;
            uint32 Depth;
            uint32 RowPitch;
            uint32 SlicePitch;

            /** Resident bytes. Empty for a streamed mip that isn't currently loaded -- the size then lives
             *  in BulkRef, not here, so never infer "this mip doesn't exist" from an empty Pixels. */
            TVector<uint8> Pixels;

            /** Where this mip lives in the package's bulk region; invalid for an inline mip. */
            FBulkDataRef BulkRef;

            /** Byte count regardless of residency. */
            uint64 SizeBytes() const { return BulkRef.IsValid() ? (uint64)BulkRef.Size : (uint64)Pixels.size(); }
        };

        // Serialized texture description: exactly what Textures::Create needs at load.
        struct FDescription
        {
            FUIntVector2 Extent  = FUIntVector2(1, 1);
            uint8        NumMips = 1;
            EFormat      Format  = EFormat::UNKNOWN;

            uint16       LayerCount = 1;

            /** Mips [0, FirstInlineMip) live in the package's bulk region and stream; [FirstInlineMip,
             *  NumMips) are inline and always resident. 0 means the whole chain is inline, which is what
             *  every pre-PACKAGE_BULK_DATA asset, every texture array, and every texture small enough not to
             *  be worth splitting look like. */
            uint8        FirstInlineMip = 0;

            friend FArchive& operator << (FArchive& Ar, FDescription& Data)
            {
                Ar << Data.Extent;
                Ar << Data.NumMips;
                Ar << Data.Format;

                if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::TEXTURE_ARRAY_LAYERS)
                {
                    Ar << Data.LayerCount;
                }

                if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::PACKAGE_BULK_DATA)
                {
                    Ar << Data.FirstInlineMip;
                }
                return Ar;
            }
        };

        FDescription            ImageDescription;
        RHI::FManagedTexture    NewTexture;

        TFixedVector<FMip, 1>   Mips;

        /** First mip currently uploaded to the GPU. Starts at FirstInlineMip after load and walks toward 0
         *  as the streamer promotes. Runtime state -- never serialized. */
        uint8                   ResidentFirstMip = 0;

        uint32 GetNumMips() const   { return ImageDescription.NumMips    > 0 ? (uint32)ImageDescription.NumMips    : 1u; }
        uint32 GetNumLayers() const { return ImageDescription.LayerCount > 0 ? (uint32)ImageDescription.LayerCount : 1u; }
        bool   IsArray() const      { return GetNumLayers() > 1; }

        uint32 MipIndex(uint32 Layer, uint32 Mip) const { return Layer * GetNumMips() + Mip; }

        /** True when some of this texture's mips live on disk rather than in memory. */
        bool IsStreamable() const { return ImageDescription.FirstInlineMip > 0; }

        /** Dimensions of a given mip of the full chain. Used to size the image when only part of the chain
         *  is resident: an image holding mips [K, NumMips) is created at mip K's dimensions. */
        FUIntVector2 MipExtent(uint32 Mip) const
        {
            const uint32 W = ImageDescription.Extent.x >> Mip;
            const uint32 H = ImageDescription.Extent.y >> Mip;
            return FUIntVector2(W > 0 ? W : 1u, H > 0 ? H : 1u);
        }

        /** How many mips the GPU image holds given the current residency. */
        uint32 GetResidentMipCount() const
        {
            const uint32 NumMips = GetNumMips();
            return ResidentFirstMip < NumMips ? NumMips - ResidentFirstMip : 1u;
        }

        /** Bytes the GPU image occupies at the current residency, summed from the mips themselves rather
         *  than re-derived from the format -- BC block math and RowPitch disagree here (RowPitch counts
         *  block rows, Height counts texels), and the per-mip sizes are exact. */
        /** Bytes the GPU image WOULD occupy if it held mips [FirstMip, NumMips). The budgeter needs to
         *  price a residency it has not applied yet, so this is separate from CalcResidentSizeBytes. */
        uint64 CalcSizeBytesFromMip(uint32 FirstMip) const
        {
            const uint32 NumMips   = GetNumMips();
            const uint32 NumLayers = GetNumLayers();

            uint64 Total = 0;
            for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
            {
                for (uint32 Mip = FirstMip; Mip < NumMips; ++Mip)
                {
                    const uint32 Index = MipIndex(Layer, Mip);
                    if (Index < Mips.size())
                    {
                        Total += Mips[Index].SizeBytes();
                    }
                }
            }
            return Total;
        }

        uint64 CalcResidentSizeBytes() const
        {
            const uint32 NumMips   = GetNumMips();
            const uint32 NumLayers = GetNumLayers();

            uint64 Total = 0;
            for (uint32 Layer = 0; Layer < NumLayers; ++Layer)
            {
                for (uint32 Mip = ResidentFirstMip; Mip < NumMips; ++Mip)
                {
                    const uint32 Index = MipIndex(Layer, Mip);
                    if (Index < Mips.size())
                    {
                        Total += Mips[Index].SizeBytes();
                    }
                }
            }
            return Total;
        }

        /** Bytes the GPU image would occupy fully resident. */
        uint64 CalcFullSizeBytes() const
        {
            uint64 Total = 0;
            for (const FMip& Mip : Mips)
            {
                Total += Mip.SizeBytes();
            }
            return Total;
        }

        uint64 CalcTotalSizeBytes() const
        {
            uint64 TotalSize = 0;

            for (const FMip& Mip : Mips)
            {
                TotalSize += (uint64)Mip.RowPitch * Mip.Height * Mip.Depth;
            }

            return TotalSize;
        }

        /** Cook-time policy: index of the first mip small enough to keep inline. Arrays are excluded --
         *  their layer count is baked into the image, so there is no Recreate overload that could resize
         *  one in place, and streaming them would mean dropping and reallocating a published heap slot. */
        static uint8 ComputeFirstInlineMip(const FDescription& Desc)
        {
            if (Desc.LayerCount > 1)
            {
                return 0;
            }

            const uint32 NumMips = Desc.NumMips > 0 ? (uint32)Desc.NumMips : 1u;
            for (uint32 Mip = 0; Mip < NumMips; ++Mip)
            {
                const uint32 W = Desc.Extent.x >> Mip;
                const uint32 H = Desc.Extent.y >> Mip;
                const uint32 Long = (W > H ? W : H) > 0 ? (W > H ? W : H) : 1u;
                if (Long <= kInlineMipMaxDimension)
                {
                    return (uint8)Mip;
                }
            }

            // Every mip is above the threshold (a chain that stops short of 256px). Keep the last one inline
            // so there is always something to draw.
            return (uint8)(NumMips - 1);
        }

        friend FArchive& operator << (FArchive& Ar, FTextureResource& Data)
        {
            // An archive with nowhere to put bulk data (duplication, transient, network) has to fall back to
            // a fully inline chain. That fallback describes the BYTES BEING WRITTEN, not this object: the
            // BulkRefs the mips already carry still address the package they were loaded from, and the
            // streamer's cached TailFirstMip still matches it. Stamping 0 onto the live description would
            // flip IsStreamable() false and desynchronize both, so it is restored below.
            const uint8 PrevFirstInlineMip = Data.ImageDescription.FirstInlineMip;
            const bool  bRestoreInlineMip  = Ar.IsWriting() && !Ar.SupportsBulkData();

            if (Ar.IsWriting())
            {
                // Recomputed on every save rather than carried on the object: it is purely a function of the
                // description.
                Data.ImageDescription.FirstInlineMip = Ar.SupportsBulkData()
                    ? ComputeFirstInlineMip(Data.ImageDescription)
                    : 0;
            }

            Ar << Data.ImageDescription;

            if (Ar.IsReading())
            {
                Data.Mips.clear();
                Data.Mips.resize(Data.GetNumMips() * Data.GetNumLayers());
            }

            const uint32 NumMips        = Data.GetNumMips();
            const uint32 FirstInlineMip = Data.ImageDescription.FirstInlineMip;

            for (uint32 Index = 0; Index < (uint32)Data.Mips.size(); ++Index)
            {
                FMip& Mip = Data.Mips[Index];

                Ar << Mip.Width;
                Ar << Mip.Height;
                Ar << Mip.Depth;
                Ar << Mip.RowPitch;
                Ar << Mip.SlicePitch;

                // Layer-major: the mip level within the chain, not the flat array index.
                const uint32 MipLevel = NumMips > 0 ? (Index % NumMips) : 0u;

                if (MipLevel >= FirstInlineMip)
                {
                    if (Ar.IsWriting() && Mip.Pixels.empty() && Mip.BulkRef.IsValid())
                    {
                        LOG_ERROR("FTextureResource: mip {} is streamed out and could not be made resident; "
                                  "it is being written as empty and its data will be lost", MipLevel);
                    }

                    Ar << Mip.Pixels;
                    continue;
                }
                
                if (Ar.IsWriting())
                {
                    if (!Ar.WriteBulkData(Mip.BulkRef, Mip.Pixels.data(), (int64)Mip.Pixels.size()))
                    {
                        LOG_ERROR("FTextureResource: failed to write bulk mip {} ({} bytes); texture will be incomplete",
                            MipLevel, Mip.Pixels.size());
                        Mip.BulkRef = FBulkDataRef{};
                    }
                }

                Ar << Mip.BulkRef;
            }

            if (Ar.IsReading())
            {
                Data.ResidentFirstMip = Data.ImageDescription.FirstInlineMip;
            }

            if (bRestoreInlineMip)
            {
                Data.ImageDescription.FirstInlineMip = PrevFirstInlineMip;
            }

            return Ar;
        }
    };
}
