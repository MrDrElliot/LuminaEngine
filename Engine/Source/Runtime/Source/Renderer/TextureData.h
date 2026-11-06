#pragma once
#include "RenderResource.h"   // RHI::Format::BytesPerBlock
#include "RHITexture.h"
#include "Containers/Algorithm.h"
#include "Containers/Vector.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    // Mips at or below this long edge stay inline and always resident, so a streamed-out texture is never blank.
    constexpr uint32 kInlineMipMaxDimension = 256;

    // Imported file bytes for a pristine re-cook, editor-only and not resident after load.
    struct FTextureSourceFile
    {
        TVector<uint8> Bytes;
        FBulkDataRef   BulkRef;

        bool IsValid() const     { return !Bytes.IsEmpty() || BulkRef.IsValid(); }
        bool IsResident() const  { return !Bytes.IsEmpty(); }
        uint64 SizeBytes() const { return BulkRef.IsValid() ? (uint64)BulkRef.Size : (uint64)Bytes.Num(); }

        void Reset()
        {
            Bytes.Reset();
            Bytes.Shrink();
            BulkRef = FBulkDataRef{};
        }

        friend FArchive& operator << (FArchive& Ar, FTextureSourceFile& Data)
        {
            if (Ar.SupportsBulkData())
            {
                if (Ar.IsWriting() && !Ar.IsBulkPassthrough())
                {
                    if (!Ar.WriteBulkData(Data.BulkRef, Data.Bytes.GetData(), Data.Bytes.Num()))
                    {
                        LOG_ERROR("FTextureSourceFile: failed to write {} source bytes", Data.Bytes.Num());
                        Data.BulkRef = FBulkDataRef{};
                    }
                }
                Ar << Data.BulkRef;
                return Ar;
            }

            // No bulk region (duplication, transient, network), so it goes inline.
            Ar << Data.Bytes;
            return Ar;
        }
    };

    struct FTextureResource
    {
        struct FMip
        {
            uint32 Width;
            uint32 Height;
            uint32 Depth;
            uint32 RowPitch;
            uint32 SlicePitch;

            // Empty for a streamed-out mip, so never infer from it that the mip does not exist.
            TVector<uint8> Pixels;

            // Invalid for an inline mip.
            FBulkDataRef BulkRef;

            uint64 SizeBytes() const { return BulkRef.IsValid() ? (uint64)BulkRef.Size : (uint64)Pixels.Num(); }
        };

        // Serialized texture description: exactly what Textures::Create needs at load.
        struct FDescription
        {
            FUIntVector2 Extent  = FUIntVector2(1, 1);
            uint8        NumMips = 1;
            EFormat      Format  = EFormat::UNKNOWN;

            uint16       LayerCount = 1;

            // Mips below this stream from the bulk region, and 0 means the whole chain is inline.
            uint8        FirstInlineMip = 0;

            // Set from CTexture::bNeverStream before a write; the split it produces is what persists.
            bool         bNeverStream = false;

            ETextureSwizzle Swizzle = ETextureSwizzle::Identity;

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

                if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::TEXTURE_SWIZZLE)
                {
                    Ar << Data.Swizzle;
                }
                return Ar;
            }
        };

        FDescription            ImageDescription;
        RHI::FManagedTexture    NewTexture;

        TInlineVector<FMip, 1>  Mips;

        // Runtime only, starting at FirstInlineMip and walking toward 0 as the streamer promotes.
        uint8                   ResidentFirstMip = 0;

        uint32 GetNumMips() const   { return ImageDescription.NumMips    > 0 ? (uint32)ImageDescription.NumMips    : 1u; }
        uint32 GetNumLayers() const { return ImageDescription.LayerCount > 0 ? (uint32)ImageDescription.LayerCount : 1u; }
        bool   IsArray() const      { return GetNumLayers() > 1; }

        uint32 MipIndex(uint32 Layer, uint32 Mip) const { return Layer * GetNumMips() + Mip; }

        bool IsStreamable() const { return ImageDescription.FirstInlineMip > 0; }

        // Sizes an image holding only mips [K, NumMips), which is created at mip K's dimensions.
        FUIntVector2 MipExtent(uint32 Mip) const
        {
            const uint32 W = ImageDescription.Extent.x >> Mip;
            const uint32 H = ImageDescription.Extent.y >> Mip;
            return FUIntVector2(W > 0 ? W : 1u, H > 0 ? H : 1u);
        }

        // Prices a residency the budgeter has not applied yet.
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
                    if (Index < static_cast<uint32>(Mips.Num()))
                    {
                        Total += Mips[Index].SizeBytes();
                    }
                }
            }
            return Total;
        }

        // Summed per mip because BC block rows make RowPitch disagree with Height.
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
                    if (Index < static_cast<uint32>(Mips.Num()))
                    {
                        Total += Mips[Index].SizeBytes();
                    }
                }
            }
            return Total;
        }

        // Bytes the GPU image would occupy fully resident.
        uint64 CalcFullSizeBytes() const
        {
            return Algo::Sum(Mips, &FMip::SizeBytes);
        }

        uint64 CalcTotalSizeBytes() const
        {
            return Algo::Accumulate(Mips, uint64(0),
                [](const FMip& Mip) { return Mip.SizeBytes(); });
        }

        // Cook-time index of the first mip small enough to keep inline.
        static uint8 ComputeFirstInlineMip(const FDescription& Desc)
        {
            if (Desc.bNeverStream)
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

        bool IsPlausibleDescription(FArchive& Ar) const
        {
            constexpr uint32 MaxExtent = 32768;
            constexpr uint32 MaxMips   = 16;
            constexpr uint32 MaxLayers = 2048;
            constexpr uint64 MinMipHeaderBytes = 5 * sizeof(uint32);
            const FDescription& Desc = ImageDescription;
            return Desc.Format < EFormat::COUNT
                && Desc.Extent.x >= 1 && Desc.Extent.y >= 1 && Desc.Extent.x <= MaxExtent && Desc.Extent.y <= MaxExtent
                && Desc.NumMips <= MaxMips && Desc.LayerCount <= MaxLayers && Desc.FirstInlineMip <= Desc.NumMips
                && Ar.CanHoldCount((uint64)GetNumMips() * GetNumLayers(), MinMipHeaderBytes);
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

            // A passthrough save copies the OLD region verbatim, so the split that produced it is the only
            // one the copied bytes agree with. Recomputing could move a mip from bulk to inline -- and a
            // streamed-out mip has no Pixels to write inline, so that lands as an empty payload. Today's
            // policy usually agrees with the file's, but "usually" is not a thing to serialize on.
            if (Ar.IsWriting() && !Ar.IsBulkPassthrough())
            {
                // Recomputed on every save rather than carried on the object: it is purely a function of the
                // description.
                Data.ImageDescription.FirstInlineMip = Ar.SupportsBulkData()
                    ? ComputeFirstInlineMip(Data.ImageDescription)
                    : 0;
            }

            Ar << Data.ImageDescription;

            // A corrupt description would size the mip table and the GPU image, so the texture loads empty instead.
            if (Ar.IsReading() && !Data.IsPlausibleDescription(Ar))
            {
                LOG_ERROR("FTextureResource: implausible description ({}x{}, {} mips, {} layers, format {}); the texture loads empty",
                    Data.ImageDescription.Extent.x, Data.ImageDescription.Extent.y, Data.ImageDescription.NumMips,
                    Data.ImageDescription.LayerCount, (uint32)Data.ImageDescription.Format);
                Ar.SetHasError(true);
                Data.ImageDescription = FDescription{};
                Data.Mips.Reset();
                return Ar;
            }

            if (Ar.IsReading())
            {
                Data.Mips.Reset();
                Data.Mips.SetNum(Data.GetNumMips() * Data.GetNumLayers());
            }

            const uint32 NumMips        = Data.GetNumMips();
            const uint32 FirstInlineMip = Data.ImageDescription.FirstInlineMip;

            for (int32 Index = 0; Index < Data.Mips.Num(); ++Index)
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
                    if (Ar.IsWriting() && Mip.Pixels.IsEmpty() && Mip.BulkRef.IsValid())
                    {
                        // Flagged, not just reported: a successful splice commits this container verbatim.
                        LOG_WARN("FTextureResource: mip {} is inline in these bytes but has no resident pixels; "
                                 "declining to write it back empty (FirstInlineMip={}, NumMips={}, Layers={}, "
                                 "Extent={}x{}, MipEntries={}, Passthrough={})",
                            MipLevel, FirstInlineMip, NumMips, Data.GetNumLayers(),
                            Data.ImageDescription.Extent.x, Data.ImageDescription.Extent.y,
                            Data.Mips.Num(), Ar.IsBulkPassthrough());

                        Ar.FlagUnresolvedBulkData();
                    }

                    Ar << Mip.Pixels;
                    continue;
                }
                
                if (Ar.IsWriting())
                {
                    if (!Ar.WriteBulkData(Mip.BulkRef, Mip.Pixels.GetData(), Mip.Pixels.Num()))
                    {
                        LOG_ERROR("FTextureResource: failed to write bulk mip {} ({} bytes); texture will be incomplete",
                            MipLevel, Mip.Pixels.Num());
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
