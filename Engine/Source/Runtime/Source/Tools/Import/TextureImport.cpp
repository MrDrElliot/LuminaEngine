#include "RuntimePCH.h"

#include "ImportHelpers.h"
#include "Paths/Paths.h"
#include "Memory/MemoryTracking.h"
#include "Core/Math/SIMD/PixelOps.h"
#include "Renderer/RHITexture.h"

// Declarations only; StbImageImpl.cpp compiles the implementations for the whole engine.
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

namespace Lumina::Import::Textures
{
    
    void ResizeImportResult(FTextureImportResult& Source, FUIntVector2 TargetSize)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        const uint32 SrcW = Source.Dimensions.x;
        const uint32 SrcH = Source.Dimensions.y;
        const uint32 DstW = TargetSize.x;
        const uint32 DstH = TargetSize.y;

        if (SrcW == 0 || SrcH == 0 || DstW == 0 || DstH == 0 || (SrcW == DstW && SrcH == DstH))
        {
            return;
        }

        stbir_pixel_layout Layout;
        stbir_datatype     DataType;
    
        switch (Source.Format)
        {
            case EFormat::R8_UNORM:       Layout = STBIR_1CHANNEL; DataType = STBIR_TYPE_UINT8;  break;
            case EFormat::RG8_UNORM:      Layout = STBIR_2CHANNEL; DataType = STBIR_TYPE_UINT8;  break;
            case EFormat::RGBA8_UNORM:
            case EFormat::SRGBA8_UNORM:   Layout = STBIR_RGBA;     DataType = STBIR_TYPE_UINT8;  break;
            case EFormat::R16_UNORM:      Layout = STBIR_1CHANNEL; DataType = STBIR_TYPE_UINT16; break;
            case EFormat::RG16_UNORM:     Layout = STBIR_2CHANNEL; DataType = STBIR_TYPE_UINT16; break;
            case EFormat::RGBA16_UNORM:   Layout = STBIR_RGBA;     DataType = STBIR_TYPE_UINT16; break;
            case EFormat::R32_FLOAT:      Layout = STBIR_1CHANNEL; DataType = STBIR_TYPE_FLOAT;  break;
            case EFormat::RG32_FLOAT:     Layout = STBIR_2CHANNEL; DataType = STBIR_TYPE_FLOAT;  break;
            case EFormat::RGB32_FLOAT:    Layout = STBIR_RGB;      DataType = STBIR_TYPE_FLOAT;  break;
            case EFormat::RGBA32_FLOAT:   Layout = STBIR_RGBA;     DataType = STBIR_TYPE_FLOAT;  break;
            default:
                LOG_WARN("ResizeImportResult: Unsupported format for resize");
                return;
        }
    
        const uint32 BytesPerPixel = (uint32)Source.Pixels.Num() / (SrcW * SrcH);

        // A fresh buffer rather than in place, since the resampler reads the source while it writes.
        TVector64<uint8> Resized(static_cast<int64>(DstW) * DstH * BytesPerPixel);

        stbir_resize
        (
            Source.Pixels.GetData(), (int)SrcW, (int)SrcH, 0,
            Resized.GetData(), (int)DstW, (int)DstH, 0,
            Layout, DataType,
            STBIR_EDGE_CLAMP,
            STBIR_FILTER_MITCHELL
        );
    
        Source.Pixels     = Move(Resized);
        Source.Dimensions = TargetSize;
    }

    void FlipImportResultVertical(FTextureImportResult& Source)
    {
        const uint32 Width  = Source.Dimensions.x;
        const uint32 Height = Source.Dimensions.y;
        if (Width == 0 || Height < 2)
        {
            return;
        }

        const size_t RowBytes = Source.Pixels.Num() / Height;
        TVector<uint8> Scratch(IntCastChecked<int32>(RowBytes));

        for (uint32 Row = 0; Row < Height / 2; ++Row)
        {
            uint8* Top    = Source.Pixels.GetData() + (size_t)Row * RowBytes;
            uint8* Bottom = Source.Pixels.GetData() + (size_t)(Height - 1 - Row) * RowBytes;

            Memory::Memcpy(Scratch.GetData(), Top, RowBytes);
            Memory::Memcpy(Top, Bottom, RowBytes);
            Memory::Memcpy(Bottom, Scratch.GetData(), RowBytes);
        }
    }

    void FlipImportResultHorizontal(FTextureImportResult& Source)
    {
        const uint32 Width  = Source.Dimensions.x;
        const uint32 Height = Source.Dimensions.y;
        if (Width < 2 || Height == 0)
        {
            return;
        }

        const size_t RowBytes      = Source.Pixels.Num() / Height;
        const size_t BytesPerTexel = RowBytes / Width;
        if (BytesPerTexel == 0)
        {
            return;
        }

        TVector<uint8> Scratch(IntCastChecked<int32>(BytesPerTexel));

        for (uint32 Row = 0; Row < Height; ++Row)
        {
            uint8* RowStart = Source.Pixels.GetData() + (size_t)Row * RowBytes;
            for (uint32 Column = 0; Column < Width / 2; ++Column)
            {
                uint8* Left  = RowStart + (size_t)Column * BytesPerTexel;
                uint8* Right = RowStart + (size_t)(Width - 1 - Column) * BytesPerTexel;

                Memory::Memcpy(Scratch.GetData(), Left, BytesPerTexel);
                Memory::Memcpy(Left, Right, BytesPerTexel);
                Memory::Memcpy(Right, Scratch.GetData(), BytesPerTexel);
            }
        }
    }

    FUIntVector2 ClampToMaxDimension(FUIntVector2 Size, uint32 MaxDimension)
    {
        if (MaxDimension == 0 || Size.x == 0 || Size.y == 0)
        {
            return Size;
        }

        const uint32 Longest = Math::Max(Size.x, Size.y);
        if (Longest <= MaxDimension)
        {
            return Size;
        }

        const double Scale = (double)MaxDimension / (double)Longest;
        return FUIntVector2(
            Math::Max(1u, (uint32)std::lround(Size.x * Scale)),
            Math::Max(1u, (uint32)std::lround(Size.y * Scale)));
    }

    TOptional<EFormat> GetHDRChannelFormat(int Channels)
    {
        switch (Channels)
        {
            case 1: return EFormat::R32_FLOAT;
            case 2: return EFormat::RG32_FLOAT;
            case 3: return EFormat::RGB32_FLOAT;
            case 4: return EFormat::RGBA32_FLOAT;
            default: return NullOpt;
        }
    }

    // A source "path" can be a data URI, and the whole payload in a log line or a GPU label is dead weight.
    static FFixedString ClipPathForLabel(FStringView Path)
    {
        constexpr size_t MaxLabelChars = 96;
        if (static_cast<size_t>(Path.Len()) <= MaxLabelChars)
        {
            return FFixedString(Path.GetData(), Path.Len());
        }

        return FormatAs<FFixedString>("{}...[{} chars]",
                                      FStringView(Path.GetData(), MaxLabelChars), (uint64)Path.Len());
    }

    template<typename TPixel>
    static TOptional<EFormat> GetChannelFormat(int Channels)
    {
        if constexpr (std::is_same_v<TPixel, float>)
        {
            return GetHDRChannelFormat(Channels);
        }
        else
        {
            constexpr bool b16Bit = sizeof(TPixel) == sizeof(uint16);
            switch (Channels)
            {
                case 1: return b16Bit ? EFormat::R16_UNORM : EFormat::R8_UNORM;
                case 2: return b16Bit ? EFormat::RG16_UNORM : EFormat::RG8_UNORM;
                // RGB is rarely a supported GPU format, so three channels expand to RGBA.
                case 3:
                case 4: return b16Bit ? EFormat::RGBA16_UNORM : EFormat::RGBA8_UNORM;
                default: return NullOpt;
            }
        }
    }

    template<typename TPixel, typename TLoader>
    static TOptional<FTextureImportResult> DecodeImage(FStringView SourceLabel, FUIntVector2 Size, TLoader&& Load)
    {
        constexpr const char* DepthName = std::is_same_v<TPixel, float> ? "HDR" : sizeof(TPixel) == sizeof(uint16) ? "16-bit" : "8-bit";

        int X, Y, Channels;
        TPixel* Data = Load(&X, &Y, &Channels);
        if (Data == nullptr)
        {
            LOG_WARN("Failed to load {0} image: {1}", DepthName, ClipPathForLabel(SourceLabel));
            return NullOpt;
        }

        TOptional<EFormat> Format = GetChannelFormat<TPixel>(Channels);
        if (!Format.IsSet())
        {
            stbi_image_free(Data);
            LOG_WARN("Unsupported channel count for {0}: {1}", DepthName, Channels);
            return NullOpt;
        }

        FTextureImportResult Result = {};
        Result.Format     = Format.GetValue();
        Result.Dimensions = FUIntVector2(X, Y);

        const size_t PixelCount = static_cast<size_t>(X) * Y;
        if constexpr (!std::is_same_v<TPixel, float>)
        {
            if (Channels == 3)
            {
                Result.Pixels = TVector64<uint8>(static_cast<int64>(PixelCount * 4 * sizeof(TPixel)));
                if constexpr (sizeof(TPixel) == sizeof(uint16))
                {
                    SIMD::ExpandRGBToRGBA16(Data, reinterpret_cast<uint16*>(Result.Pixels.GetData()), PixelCount);
                }
                else
                {
                    SIMD::ExpandRGBToRGBA8(Data, Result.Pixels.GetData(), PixelCount);
                }
            }
        }

        if (Result.Pixels.IsEmpty())
        {
            const uint8* Bytes = reinterpret_cast<const uint8*>(Data);
            Result.Pixels.Append(Bytes, Bytes + PixelCount * Channels * sizeof(TPixel));
        }

        stbi_image_free(Data);
        ResizeImportResult(Result, Size);
        return Result;
    }

    TOptional<FTextureImportResult> ImportTexture(FStringView RawFilePath, bool bFlipVertical, FUIntVector2 Size)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        stbi_set_flip_vertically_on_load_thread(bFlipVertical);

        const char* Path = RawFilePath.GetData();
        if (stbi_is_hdr(Path))
        {
            return DecodeImage<float>(RawFilePath, Size, [&](int* X, int* Y, int* C) { return stbi_loadf(Path, X, Y, C, 0); });
        }
        if (stbi_is_16_bit(Path))
        {
            return DecodeImage<uint16>(RawFilePath, Size, [&](int* X, int* Y, int* C) { return stbi_load_16(Path, X, Y, C, 0); });
        }
        return DecodeImage<uint8>(RawFilePath, Size, [&](int* X, int* Y, int* C) { return stbi_load(Path, X, Y, C, 0); });
    }

    TOptional<FTextureImportResult> ImportTexture(TSpan<const uint8> ImageData, bool bFlipVertical, FUIntVector2 Size)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        stbi_set_flip_vertically_on_load_thread(bFlipVertical);

        const uint8* Bytes = ImageData.GetData();
        const int ByteCount = ImageData.Num();
        constexpr FStringView MemoryLabel = "memory buffer";
        if (stbi_is_hdr_from_memory(Bytes, ByteCount))
        {
            return DecodeImage<float>(MemoryLabel, Size, [&](int* X, int* Y, int* C) { return stbi_loadf_from_memory(Bytes, ByteCount, X, Y, C, 0); });
        }
        if (stbi_is_16_bit_from_memory(Bytes, ByteCount))
        {
            return DecodeImage<uint16>(MemoryLabel, Size, [&](int* X, int* Y, int* C) { return stbi_load_16_from_memory(Bytes, ByteCount, X, Y, C, 0); });
        }
        return DecodeImage<uint8>(MemoryLabel, Size, [&](int* X, int* Y, int* C) { return stbi_load_from_memory(Bytes, ByteCount, X, Y, C, 0); });
    }

    RHI::FManagedTexture CreateTextureFromImport(FStringView RawFilePath, bool bFlipVerticalOnLoad, FUIntVector2 Size)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        LUMINA_PROFILE_SCOPE();

        TOptional<FTextureImportResult> MaybeResult = ImportTexture(RawFilePath, bFlipVerticalOnLoad, Size);
        if (!MaybeResult.IsSet())
        {
            return {};
        }

        const FTextureImportResult& Result = MaybeResult.GetValue();

        const FFixedString DebugName = FFixedString("Import.") + ClipPathForLabel(RawFilePath);

        RHI::FManagedTexture Texture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Result.Dimensions.x,
            .Height = Result.Dimensions.y,
            .Format = Result.Format,
            .DebugName = *DebugName,
        });
        RHI::Textures::Upload(Texture, 0, Result.Pixels.GetData(), Result.Pixels.Num(), Result.Dimensions.x);

        return Texture;
    }
}
