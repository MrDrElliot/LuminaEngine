#include "pch.h"

#include "ImportHelpers.h"
#include "Paths/Paths.h"
#include "Renderer/RHITexture.h"

#define STBI_MALLOC(Sz) Lumina::Memory::Malloc(Sz)
#define STBI_REALLOC(p, newsz) Lumina::Memory::Realloc(p, newsz)
#define STBI_FREE(p) Lumina::Memory::Free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "FileSystem/FileSystem.h"

namespace Lumina::Import::Textures
{
    
    static void ResizeResult(FTextureImportResult& Source, FUIntVector2 TargetSize)
    {
        const uint32 SrcW = Source.Dimensions.x;
        const uint32 SrcH = Source.Dimensions.y;
        const uint32 DstW = TargetSize.x;
        const uint32 DstH = TargetSize.y;
        
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
                LOG_WARN("ResizeResult: Unsupported format for resize");
                return;
        }
    
        uint32 BytesPerPixel = (uint32)Source.Pixels.size() / (SrcW * SrcH);
        Source.Pixels.resize(static_cast<size_t>(DstW * DstH * BytesPerPixel));
    
        stbir_resize
        (
            Source.Pixels.data(), (int)SrcW, (int)SrcH, 0,
            Source.Pixels.data(), (int)DstW, (int)DstH, 0,
            Layout, DataType,
            STBIR_EDGE_CLAMP,
            STBIR_FILTER_MITCHELL
        );
    
        Source.Dimensions = TargetSize;
    }
    
    TOptional<FTextureImportResult> ImportTexture(FStringView RawFilePath, bool bFlipVertical, FUIntVector2 Size)
    {
        FTextureImportResult Result = {};
        
        stbi_set_flip_vertically_on_load_thread(bFlipVertical);
        
        int x, y, channels;
        
        if (stbi_is_hdr(RawFilePath.data()))
        {
            float* data = stbi_loadf(RawFilePath.data(), &x, &y, &channels, 0);
            if (data == nullptr)
            {
                LOG_WARN("Failed to load HDR image: {0}", RawFilePath);
                return eastl::nullopt;
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
                return eastl::nullopt;
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
                    return eastl::nullopt;
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
            return eastl::nullopt;
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
                    Result.Pixels = eastl::move(rgba8Data);
                }
                break;
            default:
                stbi_image_free(data);
                LOG_WARN("Unsupported channel count: {0}", channels);
                return eastl::nullopt;
        }
        
        if (channels != 3) // If we didn't do the RGB -> RGBA conversion above
        {
            Result.Pixels.assign(data, data + static_cast<size_t>(x) * y * channels);
        }
        
        Result.Dimensions = FUIntVector2(x, y);
        
        if (Size.x > 0 && Size.y > 0)
        {
            ResizeResult(Result, Size);
        }
        
        stbi_image_free(data);
        return Result;
    }

    TOptional<FTextureImportResult> ImportTexture(TSpan<const uint8> ImageData, bool bFlipVertical, FUIntVector2 Size)
    {
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
                return eastl::nullopt;
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
                return eastl::nullopt;
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
                    return eastl::nullopt;
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
            return eastl::nullopt;
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
                    Result.Pixels = eastl::move(rgba8Data);
                }
                break;
            default:
                stbi_image_free(data);
                LOG_WARN("Unsupported channel count: {0}", channels);
                return eastl::nullopt;
        }
        
        if (channels != 3) // If we didn't do the RGB -> RGBA conversion above
        {
            Result.Pixels.assign(data, data + static_cast<size_t>(x) * y * channels);
        }
        
        Result.Dimensions = FUIntVector2(x, y);
        
        if (Size.x > 0 && Size.y > 0)
        {
            ResizeResult(Result, Size);
        }
        
        stbi_image_free(data);
        return Result;
    }

    RHI::FManagedTexture CreateTextureFromImport(FStringView RawFilePath, bool bFlipVerticalOnLoad, FUIntVector2 Size)
    {
        LUMINA_PROFILE_SCOPE();

        TOptional<FTextureImportResult> MaybeResult = ImportTexture(RawFilePath, bFlipVerticalOnLoad, Size);
        if (!MaybeResult.has_value())
        {
            return {};
        }

        const FTextureImportResult& Result = MaybeResult.value();

        RHI::FManagedTexture Texture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Result.Dimensions.x,
            .Height = Result.Dimensions.y,
            .Format = Result.Format,
        });
        RHI::Textures::Upload(Texture, 0, Result.Pixels.data(), Result.Pixels.size(), Result.Dimensions.x);

        return Texture;
    }
}
