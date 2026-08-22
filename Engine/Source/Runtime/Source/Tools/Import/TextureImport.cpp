#include "RuntimePCH.h"

#include "ImportHelpers.h"
#include "Paths/Paths.h"
#include "Memory/MemoryTracking.h"
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
    
        const uint32 BytesPerPixel = (uint32)Source.Pixels.size() / (SrcW * SrcH);

        // A fresh buffer rather than in place, since the resampler reads the source while it writes.
        TVector<uint8> Resized(static_cast<size_t>(DstW) * DstH * BytesPerPixel);

        stbir_resize
        (
            Source.Pixels.data(), (int)SrcW, (int)SrcH, 0,
            Resized.data(), (int)DstW, (int)DstH, 0,
            Layout, DataType,
            STBIR_EDGE_CLAMP,
            STBIR_FILTER_MITCHELL
        );
    
        Source.Pixels     = std::move(Resized);
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

        const size_t RowBytes = Source.Pixels.size() / Height;
        TVector<uint8> Scratch(RowBytes);

        for (uint32 Row = 0; Row < Height / 2; ++Row)
        {
            uint8* Top    = Source.Pixels.data() + (size_t)Row * RowBytes;
            uint8* Bottom = Source.Pixels.data() + (size_t)(Height - 1 - Row) * RowBytes;

            Memory::Memcpy(Scratch.data(), Top, RowBytes);
            Memory::Memcpy(Top, Bottom, RowBytes);
            Memory::Memcpy(Bottom, Scratch.data(), RowBytes);
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

        const size_t RowBytes      = Source.Pixels.size() / Height;
        const size_t BytesPerTexel = RowBytes / Width;
        if (BytesPerTexel == 0)
        {
            return;
        }

        TVector<uint8> Scratch(BytesPerTexel);

        for (uint32 Row = 0; Row < Height; ++Row)
        {
            uint8* RowStart = Source.Pixels.data() + (size_t)Row * RowBytes;
            for (uint32 Column = 0; Column < Width / 2; ++Column)
            {
                uint8* Left  = RowStart + (size_t)Column * BytesPerTexel;
                uint8* Right = RowStart + (size_t)(Width - 1 - Column) * BytesPerTexel;

                Memory::Memcpy(Scratch.data(), Left, BytesPerTexel);
                Memory::Memcpy(Left, Right, BytesPerTexel);
                Memory::Memcpy(Right, Scratch.data(), BytesPerTexel);
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

    TOptional<FTextureImportResult> ImportTexture(FStringView RawFilePath, bool bFlipVertical, FUIntVector2 Size)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        FTextureImportResult Result = {};
        
        stbi_set_flip_vertically_on_load_thread(bFlipVertical);
        
        int x, y, channels;
        
        if (stbi_is_hdr(RawFilePath.data()))
        {
            float* data = stbi_loadf(RawFilePath.data(), &x, &y, &channels, 0);
            if (data == nullptr)
            {
                LOG_WARN("Failed to load HDR image: {0}", RawFilePath);
                return NullOpt;
            }
            
            switch (channels)
            {
                case 1: Result.Format = EFormat::R32_FLOAT; break;
                case 2: Result.Format = EFormat::RG32_FLOAT; break;
                case 3: Result.Format = EFormat::RGB32_FLOAT; break;
                case 4: Result.Format = EFormat::RGBA32_FLOAT; break;
                default:
                    stbi_image_free(data);
                    LOG_WARN("Unsupported channel count for HDR: {0}", channels);
                    return Result;
            }
            
            size_t dataSize = static_cast<size_t>(x) * y * channels * sizeof(float);
            Result.Pixels.assign(reinterpret_cast<uint8*>(data), reinterpret_cast<uint8*>(data) + dataSize);
            stbi_image_free(data);
            
            Result.Dimensions = {x, y};
            return Result;
        }
        
        if (stbi_is_16_bit(RawFilePath.data()))
        {
            uint16* data = stbi_load_16(RawFilePath.data(), &x, &y, &channels, 0);
            if (data == nullptr)
            {
                LOG_WARN("Failed to load 16-bit image: {0}", RawFilePath);
                return NullOpt;
            }
            
            switch (channels)
            {
                case 1: Result.Format = EFormat::R16_UNORM; break;
                case 2: Result.Format = EFormat::RG16_UNORM; break;
                case 4: Result.Format = EFormat::RGBA16_UNORM; break;
                case 3: 
                    // RGB16 not commonly supported, convert to RGBA16
                    {
                        Result.Format = EFormat::RGBA16_UNORM;
                        TVector<uint16> rgba16Data(x * y * 4);
                        for (int i = 0; i < x * y; ++i)
                        {
                            rgba16Data[i * 4 + 0] = data[i * 3 + 0];
                            rgba16Data[i * 4 + 1] = data[i * 3 + 1];
                            rgba16Data[i * 4 + 2] = data[i * 3 + 2];
                            rgba16Data[i * 4 + 3] = 0xFFFF; // Max value for 16-bit
                        }
                        size_t dataSize = rgba16Data.size() * sizeof(uint16);
                        Result.Pixels.assign(reinterpret_cast<uint8*>(rgba16Data.data()), reinterpret_cast<uint8*>(rgba16Data.data()) + dataSize);
                    }
                    break;
                default:
                    stbi_image_free(data);
                    LOG_WARN("Unsupported channel count for 16-bit: {0}", channels);
                    return NullOpt;
            }
            
            if (channels != 3) // If we didn't do the RGB->RGBA conversion above
            {
                size_t dataSize = static_cast<size_t>(x) * y * channels * sizeof(uint16);
                Result.Pixels.assign(reinterpret_cast<uint8*>(data), reinterpret_cast<uint8*>(data) + dataSize);
            }
            
            stbi_image_free(data);
            Result.Dimensions = {x, y};
            return Result;
        }
        
        // Standard 8-bit image
        uint8* data = stbi_load(RawFilePath.data(), &x, &y, &channels, 0);
        if (data == nullptr)
        {
            LOG_WARN("Failed to load 8-bit image: {0}", RawFilePath);
            return NullOpt;
        }
        
        bool bIsSRGB = false;
        
        
        switch (channels)
        {
            case 1: Result.Format = EFormat::R8_UNORM; break;
            case 2: Result.Format = EFormat::RG8_UNORM; break;
            case 4: Result.Format = bIsSRGB ? EFormat::SRGBA8_UNORM : EFormat::RGBA8_UNORM; break;
            case 3:
                // RGB8 not commonly supported, convert to RGBA8
                {
                    Result.Format = bIsSRGB ? EFormat::SRGBA8_UNORM : EFormat::RGBA8_UNORM;
                    TVector<uint8> rgba8Data(x * y * 4);
                    for (int i = 0; i < x * y; ++i)
                    {
                        rgba8Data[i * 4 + 0] = data[i * 3 + 0];
                        rgba8Data[i * 4 + 1] = data[i * 3 + 1];
                        rgba8Data[i * 4 + 2] = data[i * 3 + 2];
                        rgba8Data[i * 4 + 3] = 0xFF;
                    }
                    Result.Pixels = std::move(rgba8Data);
                }
                break;
            default:
                stbi_image_free(data);
                LOG_WARN("Unsupported channel count: {0}", channels);
                return NullOpt;
        }
        
        if (channels != 3) // If we didn't do the RGB -> RGBA conversion above
        {
            Result.Pixels.assign(data, data + static_cast<size_t>(x) * y * channels);
        }
        
        Result.Dimensions = FUIntVector2(x, y);
        
        if (Size.x > 0 && Size.y > 0)
        {
            ResizeImportResult(Result, Size);
        }
        
        stbi_image_free(data);
        return Result;
    }

    TOptional<FTextureImportResult> ImportTexture(TSpan<const uint8> ImageData, bool bFlipVertical, FUIntVector2 Size)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        FTextureImportResult Result = {};
        
        stbi_set_flip_vertically_on_load_thread(bFlipVertical);
        int DataSize = static_cast<int>(ImageData.size());
        
        int x, y, channels;
        
        if (stbi_is_hdr_from_memory(ImageData.data(), DataSize))
        {
            float* Data = stbi_loadf_from_memory(ImageData.data(), DataSize, &x, &y, &channels, 0);
            if (Data == nullptr)
            {
                LOG_WARN("Failed to load HDR image");
                return NullOpt;
            }
            
            switch (channels)
            {
                case 1: Result.Format = EFormat::R32_FLOAT; break;
                case 2: Result.Format = EFormat::RG32_FLOAT; break;
                case 3: Result.Format = EFormat::RGB32_FLOAT; break;
                case 4: Result.Format = EFormat::RGBA32_FLOAT; break;
                default:
                    stbi_image_free(Data);
                    LOG_WARN("Unsupported channel count for HDR: {0}", channels);
                    return Result;
            }
            
            size_t dataSize = static_cast<size_t>(x) * y * channels * sizeof(float);
            Result.Pixels.assign(reinterpret_cast<uint8*>(Data), reinterpret_cast<uint8*>(Data) + dataSize);
            stbi_image_free(Data);
            
            Result.Dimensions = {x, y};
            return Result;
        }
        
        if (stbi_is_16_bit_from_memory(ImageData.data(), DataSize))
        {
            uint16* Data = stbi_load_16_from_memory(ImageData.data(), DataSize, &x, &y, &channels, 0);
            if (Data == nullptr)
            {
                LOG_WARN("Failed to load 16-bit image");
                return NullOpt;
            }
            
            switch (channels)
            {
                case 1: Result.Format = EFormat::R16_UNORM; break;
                case 2: Result.Format = EFormat::RG16_UNORM; break;
                case 4: Result.Format = EFormat::RGBA16_UNORM; break;
                case 3: 
                    // RGB16 not commonly supported, convert to RGBA16
                    {
                        Result.Format = EFormat::RGBA16_UNORM;
                        TVector<uint16> rgba16Data(x * y * 4);
                        for (int i = 0; i < x * y; ++i)
                        {
                            rgba16Data[i * 4 + 0] = Data[i * 3 + 0];
                            rgba16Data[i * 4 + 1] = Data[i * 3 + 1];
                            rgba16Data[i * 4 + 2] = Data[i * 3 + 2];
                            rgba16Data[i * 4 + 3] = 0xFFFF; // Max value for 16-bit
                        }
                        size_t dataSize = rgba16Data.size() * sizeof(uint16);
                        Result.Pixels.assign(reinterpret_cast<uint8*>(rgba16Data.data()), reinterpret_cast<uint8*>(rgba16Data.data()) + dataSize);
                    }
                    break;
                default:
                    stbi_image_free(Data);
                    LOG_WARN("Unsupported channel count for 16-bit: {0}", channels);
                    return NullOpt;
            }
            
            if (channels != 3) // If we didn't do the RGB->RGBA conversion above
            {
                size_t dataSize = static_cast<size_t>(x) * y * channels * sizeof(uint16);
                Result.Pixels.assign(reinterpret_cast<uint8*>(Data), reinterpret_cast<uint8*>(Data) + dataSize);
            }
            
            stbi_image_free(Data);
            Result.Dimensions = {x, y};
            return Result;
        }
        
        // Standard 8-bit image
        uint8* data = stbi_load_from_memory(ImageData.data(), DataSize, &x, &y, &channels, 0);
        if (data == nullptr)
        {
            LOG_WARN("Failed to load 8-bit image");
            return NullOpt;
        }
        
        bool bIsSRGB = false;
        
        
        switch (channels)
        {
            case 1: Result.Format = EFormat::R8_UNORM; break;
            case 2: Result.Format = EFormat::RG8_UNORM; break;
            case 4: Result.Format = bIsSRGB ? EFormat::SRGBA8_UNORM : EFormat::RGBA8_UNORM; break;
            case 3:
                // RGB8 not commonly supported, convert to RGBA8
                {
                    Result.Format = bIsSRGB ? EFormat::SRGBA8_UNORM : EFormat::RGBA8_UNORM;
                    TVector<uint8> rgba8Data(x * y * 4);
                    for (int i = 0; i < x * y; ++i)
                    {
                        rgba8Data[i * 4 + 0] = data[i * 3 + 0];
                        rgba8Data[i * 4 + 1] = data[i * 3 + 1];
                        rgba8Data[i * 4 + 2] = data[i * 3 + 2];
                        rgba8Data[i * 4 + 3] = 0xFF;
                    }
                    Result.Pixels = std::move(rgba8Data);
                }
                break;
            default:
                stbi_image_free(data);
                LOG_WARN("Unsupported channel count: {0}", channels);
                return NullOpt;
        }
        
        if (channels != 3) // If we didn't do the RGB -> RGBA conversion above
        {
            Result.Pixels.assign(data, data + static_cast<size_t>(x) * y * channels);
        }
        
        Result.Dimensions = FUIntVector2(x, y);
        
        if (Size.x > 0 && Size.y > 0)
        {
            ResizeImportResult(Result, Size);
        }
        
        stbi_image_free(data);
        return Result;
    }

    RHI::FManagedTexture CreateTextureFromImport(FStringView RawFilePath, bool bFlipVerticalOnLoad, FUIntVector2 Size)
    {
        LUMINA_MEMORY_SCOPE("Textures");

        LUMINA_PROFILE_SCOPE();

        TOptional<FTextureImportResult> MaybeResult = ImportTexture(RawFilePath, bFlipVerticalOnLoad, Size);
        if (!MaybeResult.has_value())
        {
            return {};
        }

        const FTextureImportResult& Result = MaybeResult.value();

        const FString DebugName = FString("Import.") + FString(RawFilePath.data(), RawFilePath.size());

        RHI::FManagedTexture Texture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Result.Dimensions.x,
            .Height = Result.Dimensions.y,
            .Format = Result.Format,
            .DebugName = DebugName.c_str(),
        });
        RHI::Textures::Upload(Texture, 0, Result.Pixels.data(), Result.Pixels.size(), Result.Dimensions.x);

        return Texture;
    }
}
