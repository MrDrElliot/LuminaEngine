#include "EditorPCH.h"
#include "TextureFactory.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "encoder/basisu_comp.h"
#include "encoder/basisu_enc.h"
#include "encoder/basisu_gpu_texture.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RendererUtils.h"
#include "Renderer/RHITexture.h"
#include "Tools/Import/ImportHelpers.h"
#include "Thumbnails/ThumbnailUtils.h"
#include "Core/Math/Math.h"
#include "Log/Log.h"

namespace Lumina
{
    CObject* CTextureFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CTexture>(Package, Name);
    }

    static bool HasPerTexelAdjustments(const CTexture* Texture);

    // Enforces the texture group's mip policy on an already-cooked resource.
    static void ApplyTextureGroupMipPolicy(CTexture* Texture)
    {
        if (Texture == nullptr || Texture->TextureResource == nullptr)
        {
            return;
        }
        if (Texture->GetResolvedPolicy().bGenerateMips)
        {
            return;
        }

        FTextureResource& Resource = *Texture->TextureResource;
        if (Resource.Mips.size() > 1)
        {
            Resource.Mips.resize(1);
        }
        Resource.ImageDescription.NumMips = 1;
    }

    // Box-downsamples an RGBA-float32 image to half dimensions (min 1px). Plain 2x2 average in linear
    // radiance, which is correct for HDR -- no gamma to undo.
    static void DownsampleEnvironmentMip(const TVector<float>& Src, uint32 SrcW, uint32 SrcH,
                                         TVector<float>& Dst, uint32& DstW, uint32& DstH)
    {
        DstW = Math::Max(SrcW >> 1, 1u);
        DstH = Math::Max(SrcH >> 1, 1u);
        Dst.resize((size_t)DstW * DstH * 4);

        for (uint32 y = 0; y < DstH; ++y)
        {
            const uint32 sy0 = Math::Min(y * 2u, SrcH - 1u);
            const uint32 sy1 = Math::Min(sy0 + 1u, SrcH - 1u);
            for (uint32 x = 0; x < DstW; ++x)
            {
                const uint32 sx0 = Math::Min(x * 2u, SrcW - 1u);
                const uint32 sx1 = Math::Min(sx0 + 1u, SrcW - 1u);

                const float* P00 = &Src[((size_t)sy0 * SrcW + sx0) * 4];
                const float* P01 = &Src[((size_t)sy0 * SrcW + sx1) * 4];
                const float* P10 = &Src[((size_t)sy1 * SrcW + sx0) * 4];
                const float* P11 = &Src[((size_t)sy1 * SrcW + sx1) * 4];

                float* D = &Dst[((size_t)y * DstW + x) * 4];
                for (int c = 0; c < 4; ++c)
                {
                    D[c] = (P00[c] + P01[c] + P10[c] + P11[c]) * 0.25f;
                }
            }
        }
    }
    
    static bool CookEnvironmentTexture(CTexture* Texture, const Import::Textures::FTextureImportResult& Source,
                                       bool bCreateGPUResource = true)
    {
        const uint32 Width  = Source.Dimensions.x;
        const uint32 Height = Source.Dimensions.y;
        const uint64 NumTexels = uint64(Width) * uint64(Height);
        if (NumTexels == 0)
        {
            return false;
        }

        // Source is always float32 here; only float-format files reach this path.
        uint32 SrcChannels = 0;
        switch (Source.Format)
        {
            case EFormat::R32_FLOAT:    SrcChannels = 1; break;
            case EFormat::RG32_FLOAT:   SrcChannels = 2; break;
            case EFormat::RGB32_FLOAT:  SrcChannels = 3; break;
            case EFormat::RGBA32_FLOAT: SrcChannels = 4; break;
            default:
                LOG_WARN("CookEnvironmentTexture: '{0}' isn't a float source; Environment color space requires .hdr",
                         Texture->GetName().c_str());
                return false;
        }

        // The adjustment stack quantizes to 8 bits, which is the one thing this path exists to avoid.
        if (HasPerTexelAdjustments(Texture))
        {
            LOG_WARN("TextureFactory: '{0}' is an HDR source; its per-texel adjustments are ignored because "
                     "applying them would quantize the radiances to 8 bits.", Texture->GetName().c_str());
        }

        const float* SrcFloats = reinterpret_cast<const float*>(Source.Pixels.data());
        
        auto Sanitize = [](float X) -> float
        {
            if (!std::isfinite(X))
            {
                return 0.0f;
            }
            return Math::Clamp(X, 0.0f, 64000.0f);
        };

        // Mip 0 as a packed RGBA-float32 working buffer; each subsequent mip box-downsamples the prior.
        TVector<float> MipFloat((size_t)NumTexels * 4);
        for (uint64 i = 0; i < NumTexels; ++i)
        {
            const float* Src = SrcFloats + i * SrcChannels;
            float* D = &MipFloat[i * 4];
            D[0] = Sanitize(SrcChannels >= 1 ? Src[0] : 0.0f);
            D[1] = Sanitize(SrcChannels >= 2 ? Src[1] : 0.0f);
            D[2] = Sanitize(SrcChannels >= 3 ? Src[2] : 0.0f);
            D[3] = SrcChannels >= 4 ? Src[3] : 1.0f;
        }

        const uint32 MaxDim  = Math::Max(Width, Height);
        const uint32 NumMips = (uint32)std::floor(std::log2((float)MaxDim)) + 1u;

        FTextureResource::FDescription ImageDescription;
        ImageDescription.Format  = EFormat::RGBA16_FLOAT;
        ImageDescription.Extent  = FUIntVector2(Width, Height);
        ImageDescription.NumMips = (uint8)NumMips;

        if (!Texture->TextureResource)
        {
            Texture->TextureResource = MakeUnique<FTextureResource>();
        }

        Texture->TextureResource->ImageDescription = ImageDescription;
        Texture->TextureResource->Mips.clear();
        Texture->TextureResource->Mips.resize(NumMips);

        uint32 MipW = Width, MipH = Height;
        TVector<float> NextFloat;
        for (uint32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
        {
            const uint64 MipTexels = (uint64)MipW * MipH;

            // RGBA16F: two uint32 per pixel; (R,G) low half, (B,A) high half. 8 bytes per pixel.
            TVector<uint32> Halves(MipTexels * 2);
            for (uint64 i = 0; i < MipTexels; ++i)
            {
                const float* P = &MipFloat[i * 4];
                Halves[i * 2 + 0] = Math::PackHalf2x16(FVector2(P[0], P[1]));
                Halves[i * 2 + 1] = Math::PackHalf2x16(FVector2(P[2], P[3]));
            }

            const uint32 RowPitch   = MipW * 8u;
            const uint32 SlicePitch = RowPitch * MipH;

            FTextureResource::FMip& Mip = Texture->TextureResource->Mips[MipIndex];
            Mip.Width      = MipW;
            Mip.Height     = MipH;
            Mip.RowPitch   = RowPitch;
            Mip.Depth      = 1;
            Mip.SlicePitch = SlicePitch;
            Mip.Pixels.assign(reinterpret_cast<uint8*>(Halves.data()),
                              reinterpret_cast<uint8*>(Halves.data()) + SlicePitch);

            if (MipIndex + 1u < NumMips)
            {
                uint32 NextW, NextH;
                DownsampleEnvironmentMip(MipFloat, MipW, MipH, NextFloat, NextW, NextH);
                MipFloat = Move(NextFloat);
                MipW = NextW;
                MipH = NextH;
            }
        }
        
        ApplyTextureGroupMipPolicy(Texture);
        const uint32 UploadMips = (uint32)Texture->TextureResource->Mips.size();

        if (!bCreateGPUResource)
        {
            return true;
        }

        // Recreate: see CookTexturePixels. A re-cook must keep the published ResourceID.
        const FString DebugName = "Texture." + Texture->GetName().ToString();
        RHI::Textures::Recreate(Texture->TextureResource->NewTexture, RHI::FTexture2DDesc
        {
            .Width  = Width,
            .Height = Height,
            .Mips   = UploadMips,
            .Format = EFormat::RGBA16_FLOAT,
            .DebugName = DebugName.c_str(),
        });
        for (uint32 i = 0; i < UploadMips; ++i)
        {
            const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[i];
            RHI::Textures::Upload(Texture->TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width, Mip.Width, Mip.Height);
        }

        // Publishes the image staged above once those uploads have executed. Skipping it does not merely
        // delay the new pixels: the swap stays unarmed forever, so the slot keeps sampling the pre-cook
        // image AND the texture can never change residency again. See RHITexture.h.
        RHI::Textures::CommitRecreate(Texture->TextureResource->NewTexture);
        Texture->OnFullyUploadedExternally();

        return true;
    }

    static bool NormalizeToRGBA8(Import::Textures::FTextureImportResult& Result)
    {
        const uint64 PixelCount = (uint64)Result.Dimensions.x * Result.Dimensions.y;
        if (PixelCount == 0)
        {
            return false;
        }

        // Channel count + bytes-per-channel of the source layout; gray sources replicate into RGB.
        uint32 Channels = 0;
        uint32 BytesPerChannel = 1;
        switch (Result.Format)
        {
            case EFormat::RGBA8_UNORM:
            case EFormat::SRGBA8_UNORM:
                return Result.Pixels.size() >= PixelCount * 4;
            case EFormat::R8_UNORM:     Channels = 1; break;
            case EFormat::RG8_UNORM:    Channels = 2; break;
            case EFormat::R16_UNORM:    Channels = 1; BytesPerChannel = 2; break;
            case EFormat::RG16_UNORM:   Channels = 2; BytesPerChannel = 2; break;
            case EFormat::RGBA16_UNORM: Channels = 4; BytesPerChannel = 2; break;
            default:
                return false;
        }

        if (Result.Pixels.size() < PixelCount * Channels * BytesPerChannel)
        {
            return false;
        }

        TVector<uint8> Converted(PixelCount * 4);
        for (uint64 i = 0; i < PixelCount; ++i)
        {
            uint8 Value[4];
            for (uint32 c = 0; c < Channels; ++c)
            {
                // 16-bit sources keep the high byte (little-endian uint16).
                const uint64 Offset = (i * Channels + c) * BytesPerChannel;
                Value[c] = Result.Pixels[Offset + BytesPerChannel - 1];
            }

            uint8* Out = &Converted[i * 4];
            switch (Channels)
            {
                case 1: Out[0] = Out[1] = Out[2] = Value[0]; Out[3] = 0xFF;      break;  // gray
                case 2: Out[0] = Out[1] = Out[2] = Value[0]; Out[3] = Value[1];  break;  // gray + alpha
                case 4: Out[0] = Value[0]; Out[1] = Value[1]; Out[2] = Value[2]; Out[3] = Value[3]; break;
            }
        }

        Result.Pixels = Move(Converted);
        Result.Format = EFormat::RGBA8_UNORM;
        return true;
    }
    
    // True when the texture asks for an edit that rewrites individual texels of an RGBA8 source.
    static bool HasPerTexelAdjustments(const CTexture* Texture)
    {
        return Texture->bFlipGreenChannel
            || Texture->bChromaKey
            || Texture->bCompressWithoutAlpha
            || Texture->AdjustBrightness      != 1.0f
            || Texture->AdjustBrightnessCurve != 1.0f
            || Texture->AdjustRGBCurve        != 1.0f
            || Texture->AdjustSaturation      != 1.0f
            || Texture->AdjustVibrance        != 0.0f
            || Texture->AdjustHue             != 0.0f
            || Texture->AdjustMinAlpha        != 0.0f
            || Texture->AdjustMaxAlpha        != 1.0f;
    }

    // Every source edit, including the ones that apply to any pixel layout.
    static bool HasSourceAdjustments(const CTexture* Texture)
    {
        return Texture->bFlipVertical || Texture->bFlipHorizontal || HasPerTexelAdjustments(Texture);
    }

    static void RGBToHSV(float R, float G, float B, float& H, float& S, float& V)
    {
        const float MaxC  = Math::Max(R, Math::Max(G, B));
        const float MinC  = Math::Min(R, Math::Min(G, B));
        const float Delta = MaxC - MinC;

        V = MaxC;
        S = (MaxC > 0.0f) ? (Delta / MaxC) : 0.0f;

        if (Delta <= 0.0f)
        {
            H = 0.0f;
            return;
        }

        if (MaxC == R)      { H = 60.0f * std::fmod((G - B) / Delta, 6.0f); }
        else if (MaxC == G) { H = 60.0f * (((B - R) / Delta) + 2.0f); }
        else                { H = 60.0f * (((R - G) / Delta) + 4.0f); }

        if (H < 0.0f)
        {
            H += 360.0f;
        }
    }

    static void HSVToRGB(float H, float S, float V, float& R, float& G, float& B)
    {
        H = std::fmod(H, 360.0f);
        if (H < 0.0f)
        {
            H += 360.0f;
        }

        const float C = V * S;
        const float X = C * (1.0f - std::fabs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
        const float M = V - C;

        float Rp = 0.0f, Gp = 0.0f, Bp = 0.0f;
        if      (H <  60.0f) { Rp = C; Gp = X; }
        else if (H < 120.0f) { Rp = X; Gp = C; }
        else if (H < 180.0f) { Gp = C; Bp = X; }
        else if (H < 240.0f) { Gp = X; Bp = C; }
        else if (H < 300.0f) { Rp = X; Bp = C; }
        else                 { Rp = C; Bp = X; }

        R = Rp + M;
        G = Gp + M;
        B = Bp + M;
    }

    // Per-texel source edits on an RGBA8 buffer, in place, before block compression.
    static void ApplySourceAdjustments(const CTexture* Texture, TVector<uint8>& Pixels, uint64 PixelCount)
    {
        if (!HasPerTexelAdjustments(Texture))
        {
            return;
        }

        const bool bHue         = Texture->AdjustHue             != 0.0f;
        const bool bSaturation  = Texture->AdjustSaturation      != 1.0f;
        const bool bBrightness  = Texture->AdjustBrightness      != 1.0f;
        const bool bBrightCurve = Texture->AdjustBrightnessCurve != 1.0f;
        const bool bRGBCurve    = Texture->AdjustRGBCurve        != 1.0f;
        const bool bVibrance    = Texture->AdjustVibrance        >  0.0f;
        const bool bAlphaRange  = Texture->AdjustMinAlpha != 0.0f || Texture->AdjustMaxAlpha != 1.0f;
        const bool bHSV         = bHue || bSaturation || bBrightness || bBrightCurve;

        const FVector3 Key       = Texture->ChromaKeyColor;
        const float    KeyThresh = Texture->ChromaKeyThreshold;

        auto Quantize = [](float X)
        {
            return (uint8)Math::Clamp((int32)std::lround(Math::Clamp(X, 0.0f, 1.0f) * 255.0f), 0, 255);
        };

        for (uint64 i = 0; i < PixelCount; ++i)
        {
            uint8* Texel = &Pixels[i * 4];

            float R = Texel[0] / 255.0f;
            float G = Texel[1] / 255.0f;
            float B = Texel[2] / 255.0f;
            float A = Texel[3] / 255.0f;

            if (Texture->bFlipGreenChannel)
            {
                G = 1.0f - G;
            }

            if (Texture->bChromaKey)
            {
                const float DR = R - Key.x;
                const float DG = G - Key.y;
                const float DB = B - Key.z;
                if ((DR * DR + DG * DG + DB * DB) <= (KeyThresh * KeyThresh))
                {
                    Texel[0] = Texel[1] = Texel[2] = Texel[3] = 0;
                    continue;
                }
            }

            if (bHSV)
            {
                float H, S, V;
                RGBToHSV(R, G, B, H, S, V);

                H += Texture->AdjustHue;
                S  = Math::Clamp(S * Texture->AdjustSaturation, 0.0f, 1.0f);
                V  = Math::Clamp(V * Texture->AdjustBrightness, 0.0f, 1.0f);

                if (bBrightCurve)
                {
                    V = std::pow(V, Texture->AdjustBrightnessCurve);
                }

                HSVToRGB(H, S, V, R, G, B);
            }

            if (bVibrance)
            {
                const float Luma = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                const float MaxC = Math::Max(R, Math::Max(G, B));
                const float MinC = Math::Min(R, Math::Min(G, B));

                // Weighted by how UNsaturated the texel already is: vivid colors are left alone.
                const float Boost = Texture->AdjustVibrance * (1.0f - (MaxC - MinC));

                R = Luma + (R - Luma) * (1.0f + Boost);
                G = Luma + (G - Luma) * (1.0f + Boost);
                B = Luma + (B - Luma) * (1.0f + Boost);
            }

            if (bRGBCurve)
            {
                R = std::pow(Math::Max(R, 0.0f), Texture->AdjustRGBCurve);
                G = std::pow(Math::Max(G, 0.0f), Texture->AdjustRGBCurve);
                B = std::pow(Math::Max(B, 0.0f), Texture->AdjustRGBCurve);
            }

            if (bAlphaRange)
            {
                A = Texture->AdjustMinAlpha + A * (Texture->AdjustMaxAlpha - Texture->AdjustMinAlpha);
            }

            if (Texture->bCompressWithoutAlpha)
            {
                A = 1.0f;
            }

            Texel[0] = Quantize(R);
            Texel[1] = Quantize(G);
            Texel[2] = Quantize(B);
            Texel[3] = Quantize(A);
        }
    }

    // Encoder effort. Higher settings cost cook time only; the stored format and size do not change.
    static void ApplyCompressionQuality(ETextureCompressionQuality Quality, basisu::basis_compressor_params& Params)
    {
        switch (Quality)
        {
        case ETextureCompressionQuality::Fastest:
            Params.m_quality_level            = 64;
            Params.m_pack_uastc_ldr_4x4_flags = basisu::cPackUASTCLevelFastest;
            break;
        case ETextureCompressionQuality::High:
            Params.m_quality_level            = 192;
            Params.m_pack_uastc_ldr_4x4_flags = basisu::cPackUASTCLevelDefault;
            break;
        case ETextureCompressionQuality::Highest:
            Params.m_quality_level            = 255;
            Params.m_pack_uastc_ldr_4x4_flags = basisu::cPackUASTCLevelSlower;
            break;
        case ETextureCompressionQuality::Default:
        default:
            Params.m_quality_level            = 128;
            Params.m_pack_uastc_ldr_4x4_flags = basisu::cPackUASTCLevelFastest;
            break;
        }
    }

    // Source edits that apply to every pixel layout, so they run ahead of the format branch.
    static void PrepareSource(const CTexture* Texture, Import::Textures::FTextureImportResult& Result)
    {
        if (Texture->bFlipVertical)
        {
            Import::Textures::FlipImportResultVertical(Result);
        }

        if (Texture->bFlipHorizontal)
        {
            Import::Textures::FlipImportResultHorizontal(Result);
        }

        const uint32 MaxDimension = Texture->GetResolvedPolicy().MaxDimension;
        const FUIntVector2 Target = Import::Textures::ClampToMaxDimension(Result.Dimensions, MaxDimension);
        if (Target.x != Result.Dimensions.x || Target.y != Result.Dimensions.y)
        {
            LOG_INFO("TextureFactory: '{0}' capped from {1}x{2} to {3}x{4} by MaxTextureSize.",
                     Texture->GetName().c_str(), Result.Dimensions.x, Result.Dimensions.y, Target.x, Target.y);
            Import::Textures::ResizeImportResult(Result, Target);
        }
    }

    static bool CookTexturePixels(CTexture* Texture, TVector<uint8>& Pixels, FUIntVector2 Dimensions, ETextureColorSpace ColorSpace, uint32 EncodeThreads = 0, bool bCreateGPUResource = true)
    {
        const uint64 RequiredBytes = (uint64)Dimensions.x * Dimensions.y * 4;
        if (RequiredBytes == 0 || Pixels.size() < RequiredBytes)
        {
            LOG_ERROR("CookTexturePixels: '{0}' pixel buffer ({1} bytes) doesn't cover {2}x{3} RGBA8 ({4} bytes); refusing to cook.",
                      Texture->GetName().c_str(), Pixels.size(), Dimensions.x, Dimensions.y, RequiredBytes);
            return false;
        }
        
        ApplySourceAdjustments(Texture, Pixels, (uint64)Dimensions.x * Dimensions.y);

        basisu::basisu_encoder_init();

        const bool bIsSRGB     = (ColorSpace == ETextureColorSpace::SRGB);

        const uint32 TotalEncodeThreads = (EncodeThreads == 0)
            ? Math::Max(1u, Threading::GetNumThreads() - 1u)
            : Math::Max(1u, EncodeThreads);
        basisu::job_pool JobPool(TotalEncodeThreads);   // total incl. caller; 1 => single-threaded (0 new threads)

        basisu::basis_compressor_params Params;
        Params.m_pJob_pool = &JobPool;

        Params.m_source_images.resize(1);
        Params.m_source_images[0].init(Pixels.data(), Dimensions.x, Dimensions.y, 4);

        Params.m_uastc                      = true;
        Params.m_print_stats                = false;
        Params.m_status_output              = false;   // silence per-slice "Slice: N, alpha: ..." spam during cook
        Params.m_mip_gen                    = Texture->GetResolvedPolicy().bGenerateMips;
        Params.m_mip_fast                   = true;
        Params.m_multithreading             = (TotalEncodeThreads > 1);
        Params.m_create_ktx2_file           = false;
        Params.m_perceptual                 = bIsSRGB;
        Params.m_mip_srgb                   = bIsSRGB;

        ApplyCompressionQuality(Texture->CompressionQuality, Params);

        basisu::basis_compressor Compressor;
        if (!Compressor.init(Params))
        {
            return false;
        }
        if (Compressor.process() != basisu::basis_compressor::cECSuccess)
        {
            return false;
        }

        const basisu::uint8_vec& BasisData = Compressor.get_output_basis_file();
        basist::basisu_transcoder Transcoder;
        if (!Transcoder.start_transcoding(BasisData.data(), BasisData.size()))
        {
            return false;
        }

        basist::basisu_file_info FileInfo;
        Transcoder.get_file_info(BasisData.data(), BasisData.size(), FileInfo);
        const uint32 NumMips = FileInfo.m_image_mipmap_levels[0];

        basist::basisu_image_info ImageInfo;
        Transcoder.get_image_info(BasisData.data(), BasisData.size(), ImageInfo, 0);
        
        const uint32 Width  = ImageInfo.m_orig_width;
        const uint32 Height = ImageInfo.m_orig_height;

        // SRGB->BC7_UNORM_SRGB; NormalMap->BC5_UNORM (shader reconstructs Z); Linear/Packed->BC7_UNORM.
        EFormat StoredFormat;
        basist::transcoder_texture_format TranscodeTarget;
        if (bIsSRGB)
        {
            StoredFormat    = EFormat::BC7_UNORM_SRGB;
            TranscodeTarget = basist::transcoder_texture_format::cTFBC7_RGBA;
        }
        else
        {
            // Normal maps included: BC5-packed normals are currently broken, so they cook as full BC7 RGB
            // like everything else. The material output node reconstructs Z from XY, so a linear RGB normal
            // renders correctly, and a NormalMap-tagged texture stays safe even if one is set manually.
            StoredFormat    = EFormat::BC7_UNORM;
            TranscodeTarget = basist::transcoder_texture_format::cTFBC7_RGBA;
        }

        FTextureResource::FDescription ImageDescription;
        ImageDescription.Format  = StoredFormat;
        ImageDescription.Extent  = FUIntVector2(Width, Height);
        ImageDescription.NumMips = static_cast<uint8>(NumMips);

        if (!Texture->TextureResource)
        {
            Texture->TextureResource = MakeUnique<FTextureResource>();
        }

        Texture->TextureResource->ImageDescription = ImageDescription;
        Texture->TextureResource->Mips.clear();
        Texture->TextureResource->Mips.resize(NumMips);

        const uint32 BytesPerBlock = RHI::Format::BytesPerBlock(StoredFormat);

        for (uint32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
        {
            basist::basisu_image_level_info LevelInfo;
            if (!Transcoder.get_image_level_info(BasisData.data(), BasisData.size(), LevelInfo, 0, MipIndex))
            {
                continue;
            }

            const uint32 BlocksX     = LevelInfo.m_num_blocks_x;
            const uint32 BlocksY     = LevelInfo.m_num_blocks_y;
            const uint32 TotalBlocks = LevelInfo.m_total_blocks;
            const uint32 RowPitch    = BlocksX * BytesPerBlock;
            const uint32 DepthPitch  = RowPitch * BlocksY;

            TVector<uint8> TranscodedData(TotalBlocks * BytesPerBlock);
            if (!Transcoder.transcode_image_level(
                    BasisData.data(), BasisData.size(),
                    0,
                    MipIndex,
                    TranscodedData.data(), TotalBlocks,
                    TranscodeTarget))
            {
                continue;
            }

            FTextureResource::FMip& Mip = Texture->TextureResource->Mips[MipIndex];
            Mip.Width      = LevelInfo.m_orig_width;
            Mip.Height     = LevelInfo.m_orig_height;
            Mip.RowPitch   = RowPitch;
            Mip.Depth      = 1;
            Mip.SlicePitch = DepthPitch;
            Mip.Pixels     = Move(TranscodedData);
        }

        const FUIntVector2 Extent = Texture->TextureResource->ImageDescription.Extent;

        ApplyTextureGroupMipPolicy(Texture);
        const uint32 UploadMips = (uint32)Texture->TextureResource->Mips.size();
        
        if (!bCreateGPUResource)
        {
            return true;
        }
        
        const FString DebugName = "Texture." + Texture->GetName().ToString();
        RHI::Textures::Recreate(Texture->TextureResource->NewTexture, RHI::FTexture2DDesc
        {
            .Width  = Extent.x,
            .Height = Extent.y,
            .Mips   = UploadMips,
            .Format = StoredFormat,
            .DebugName = DebugName.c_str(),
        });
        for (uint32 i = 0; i < UploadMips; ++i)
        {
            const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[i];
            if (!Mip.Pixels.empty())
            {
                RHI::Textures::Upload(Texture->TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width, Mip.Width, Mip.Height);
            }
        }

        // Publishes the image staged above once those uploads have executed. Skipping it does not merely
        // delay the new pixels: the swap stays unarmed forever, so the slot keeps sampling the pre-cook
        // image AND the texture can never change residency again. See RHITexture.h.
        RHI::Textures::CommitRecreate(Texture->TextureResource->NewTexture);
        Texture->OnFullyUploadedExternally();

        return true;
    }

    static uint32 ReadU32LE(const uint8* P)
    {
        return (uint32)P[0] | ((uint32)P[1] << 8) | ((uint32)P[2] << 16) | ((uint32)P[3] << 24);
    }

    // DDS magic "DDS " + a full DDS_HEADER (124B); the smallest valid file is magic(4)+header(124).
    static bool LooksLikeDDS(TSpan<const uint8> Data)
    {
        return Data.size() >= 128 && Data[0] == 'D' && Data[1] == 'D' && Data[2] == 'S' && Data[3] == ' ';
    }

    // DDS holds already-block-compressed (BCn) data that stb_image cannot read. Instead of decode+recompress,
    // map the DDS format to our EFormat and upload the blocks VERBATIM (lossless; the GPU samples BCn natively).
    // sRGB-ness is taken from the texture's role (base color = SRGB, normals/data = Linear), matching the cook.
    // Returns false for non-DDS or unsupported (uncompressed/legacy) DDS so the caller can fall back to stb.
    static bool CookDDS(CTexture* Texture, TSpan<const uint8> Data, ETextureColorSpace ColorSpace,
                        bool bCreateGPUResource = true)
    {
        if (!LooksLikeDDS(Data))
        {
            return false;
        }

        const uint8* Bytes = Data.data();
        const uint32 Height   = ReadU32LE(Bytes + 12);   // DDS_HEADER.dwHeight
        const uint32 Width    = ReadU32LE(Bytes + 16);   // DDS_HEADER.dwWidth
        uint32       MipCount = ReadU32LE(Bytes + 28);   // DDS_HEADER.dwMipMapCount
        const uint32 FourCC   = ReadU32LE(Bytes + 84);   // DDS_HEADER.ddspf.dwFourCC

        if (Width == 0 || Height == 0)
        {
            return false;
        }
        MipCount = Math::Max(1u, MipCount);

        auto MakeFourCC = [](char A, char B, char C, char D) -> uint32
        {
            return (uint32)(uint8)A | ((uint32)(uint8)B << 8) | ((uint32)(uint8)C << 16) | ((uint32)(uint8)D << 24);
        };

        const bool bSRGB = (ColorSpace == ETextureColorSpace::SRGB);
        EFormat Format   = EFormat::UNKNOWN;
        size_t  DataOffset = 128;   // magic(4) + DDS_HEADER(124)

        if (FourCC == MakeFourCC('D', 'X', '1', '0'))
        {
            // DDS_HEADER_DXT10 (20B) follows; dxgiFormat is its first field. Pixel data then starts at 148.
            if (Data.size() < 148)
            {
                return false;
            }
            const uint32 DXGI = ReadU32LE(Bytes + 128);
            DataOffset = 148;
            switch (DXGI)
            {
                case 70: case 71: case 72: Format = bSRGB ? EFormat::BC1_UNORM_SRGB : EFormat::BC1_UNORM; break;
                case 73: case 74: case 75: Format = bSRGB ? EFormat::BC2_UNORM_SRGB : EFormat::BC2_UNORM; break;
                case 76: case 77: case 78: Format = bSRGB ? EFormat::BC3_UNORM_SRGB : EFormat::BC3_UNORM; break;
                case 79: case 80:          Format = EFormat::BC4_UNORM; break;
                case 81:                   Format = EFormat::BC4_SNORM; break;
                case 82: case 83:          Format = EFormat::BC5_UNORM; break;
                case 84:                   Format = EFormat::BC5_SNORM; break;
                case 94: case 95:          Format = EFormat::BC6H_UFLOAT; break;
                case 96:                   Format = EFormat::BC6H_SFLOAT; break;
                case 97: case 98: case 99: Format = bSRGB ? EFormat::BC7_UNORM_SRGB : EFormat::BC7_UNORM; break;
                default: return false;     // uncompressed / unsupported DX10 format
            }
        }
        else if (FourCC == MakeFourCC('D', 'X', 'T', '1'))                                          { Format = bSRGB ? EFormat::BC1_UNORM_SRGB : EFormat::BC1_UNORM; }
        else if (FourCC == MakeFourCC('D', 'X', 'T', '3'))                                          { Format = bSRGB ? EFormat::BC2_UNORM_SRGB : EFormat::BC2_UNORM; }
        else if (FourCC == MakeFourCC('D', 'X', 'T', '5'))                                          { Format = bSRGB ? EFormat::BC3_UNORM_SRGB : EFormat::BC3_UNORM; }
        else if (FourCC == MakeFourCC('A', 'T', 'I', '1') || FourCC == MakeFourCC('B', 'C', '4', 'U')) { Format = EFormat::BC4_UNORM; }
        else if (FourCC == MakeFourCC('B', 'C', '4', 'S'))                                          { Format = EFormat::BC4_SNORM; }
        else if (FourCC == MakeFourCC('A', 'T', 'I', '2') || FourCC == MakeFourCC('B', 'C', '5', 'U')) { Format = EFormat::BC5_UNORM; }
        else if (FourCC == MakeFourCC('B', 'C', '5', 'S'))                                          { Format = EFormat::BC5_SNORM; }
        else
        {
            return false;   // uncompressed / legacy-RGB DDS not handled here
        }

        const uint32 BytesPerBlock = RHI::Format::BytesPerBlock(Format);
        if (BytesPerBlock == 0)
        {
            return false;
        }

        if (!Texture->TextureResource)
        {
            Texture->TextureResource = MakeUnique<FTextureResource>();
        }
        Texture->TextureResource->Mips.clear();
        Texture->TextureResource->Mips.reserve(MipCount);

        size_t Offset     = DataOffset;
        uint32 StoredMips = 0;
        for (uint32 m = 0; m < MipCount; ++m)
        {
            const uint32 MipW    = Math::Max(1u, Width  >> m);
            const uint32 MipH    = Math::Max(1u, Height >> m);
            const uint32 BlocksX = Math::Max(1u, (MipW + 3u) / 4u);
            const uint32 BlocksY = Math::Max(1u, (MipH + 3u) / 4u);
            const size_t MipSize = (size_t)BlocksX * BlocksY * BytesPerBlock;

            if (Offset + MipSize > Data.size())
            {
                break;   // truncated file -- keep whatever mips parsed cleanly
            }

            FTextureResource::FMip Mip;
            Mip.Width      = MipW;
            Mip.Height     = MipH;
            Mip.Depth      = 1;
            Mip.RowPitch   = BlocksX * BytesPerBlock;
            Mip.SlicePitch = (uint32)MipSize;
            Mip.Pixels.assign(Bytes + Offset, Bytes + Offset + MipSize);
            Texture->TextureResource->Mips.push_back(Move(Mip));

            Offset += MipSize;
            ++StoredMips;
        }

        if (StoredMips == 0)
        {
            return false;
        }

        FTextureResource::FDescription Desc;
        Desc.Format  = Format;
        Desc.Extent  = FUIntVector2(Width, Height);
        Desc.NumMips = (uint8)StoredMips;
        Texture->TextureResource->ImageDescription = Desc;

        // Trim to the group's mip policy BEFORE the GPU texture is created, so the allocation
        // itself is single-mip rather than a full chain we then ignore.
        ApplyTextureGroupMipPolicy(Texture);
        const uint32 UploadMips = (uint32)Texture->TextureResource->Mips.size();

        if (!bCreateGPUResource)
        {
            return true;
        }

        // Recreate: see CookTexturePixels. A re-cook must keep the published ResourceID.
        const FString DebugName = "Texture." + Texture->GetName().ToString();
        RHI::Textures::Recreate(Texture->TextureResource->NewTexture, RHI::FTexture2DDesc
        {
            .Width  = Width,
            .Height = Height,
            .Mips   = UploadMips,
            .Format = Format,
            .DebugName = DebugName.c_str(),
        });
        for (uint32 i = 0; i < UploadMips; ++i)
        {
            const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[i];
            if (!Mip.Pixels.empty())
            {
                RHI::Textures::Upload(Texture->TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width, Mip.Width, Mip.Height);
            }
        }

        // Publishes the image staged above once those uploads have executed. Skipping it does not merely
        // delay the new pixels: the swap stays unarmed forever, so the slot keeps sampling the pre-cook
        // image AND the texture can never change residency again. See RHITexture.h.
        RHI::Textures::CommitRecreate(Texture->TextureResource->NewTexture);
        Texture->OnFullyUploadedExternally();

        return true;
    }

    // Filename suffix heuristic for Auto; falls back to SRGB. Editable in the inspector for misclassifications.
    ETextureColorSpace CTextureFactory::ClassifyColorSpaceByFilename(FStringView Path)
    {
        eastl::string Stem(Path.data(), Path.size());

        // Lowercase first so .HDR/.hdr both match.
        for (char& C : Stem)
        {
            if (C >= 'A' && C <= 'Z') C = (char)(C + ('a' - 'A'));
        }

        // Route .hdr to Environment before suffix heuristics run.
        if (Stem.size() >= 4 && Stem.compare(Stem.size() - 4, 4, ".hdr") == 0)
        {
            return ETextureColorSpace::Environment;
        }

        // Strip extension before suffix match.
        const size_t DotPos = Stem.find_last_of('.');
        if (DotPos != eastl::string::npos)
        {
            Stem.resize(DotPos);
        }

        auto EndsWith = [&Stem](const char* Suffix)
        {
            const size_t SufLen = strlen(Suffix);
            return Stem.size() >= SufLen && Stem.compare(Stem.size() - SufLen, SufLen, Suffix) == 0;
        };

        // Normal maps resolve to Linear (BC7 RGB), NOT NormalMap: the BC5-packed normal path is currently
        // broken. The material output node reconstructs Z from XY, so Linear-stored normals work correctly.
        if (EndsWith("_n") || EndsWith("_normal") || EndsWith("_norm") || EndsWith("_nrm"))
            return ETextureColorSpace::Linear;

        if (EndsWith("_orm") || EndsWith("_arm") || EndsWith("_mra") || EndsWith("_rmo") ||
            EndsWith("_mro") || EndsWith("_rma") || EndsWith("_amr") ||
            EndsWith("_metalroughness") || EndsWith("_metallicroughness") ||
            EndsWith("_metalrough") || EndsWith("_mr") || EndsWith("_rm"))
            return ETextureColorSpace::PackedData;

        if (EndsWith("_r") || EndsWith("_rough") || EndsWith("_roughness") ||
            EndsWith("_m") || EndsWith("_metal") || EndsWith("_metallic") ||
            EndsWith("_ao") || EndsWith("_occ") || EndsWith("_occlusion") ||
            EndsWith("_h") || EndsWith("_height") || EndsWith("_disp") || EndsWith("_displacement"))
            return ETextureColorSpace::Linear;

        return ETextureColorSpace::SRGB;
    }
    
#if USING(WITH_EDITOR)
    // Narkowicz 2015 ACES filmic fit, then sRGB OETF + quantize. Tonemaps one linear-HDR channel to 8-bit.
    static uint8 HdrChannelToSRGB8(float Linear)
    {
        constexpr float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        const float Mapped = Math::Clamp((Linear * (a * Linear + b)) / (Linear * (c * Linear + d) + e), 0.0f, 1.0f);
        const float Srgb = (Mapped <= 0.0031308f) ? (12.92f * Mapped)
                                                  : (1.055f * std::pow(Mapped, 1.0f / 2.4f) - 0.055f);
        return (uint8)Math::Clamp((int32)std::lround(Srgb * 255.0f), 0, 255);
    }

    // Builds an RGBA8 thumbnail from the imported source. Float (HDR) sources are tonemapped so they no
    // longer clip to white; 8-bit sources pass through. The shared helper does the downsample + flip.
    static void CreatePackageThumbnail(CTexture* Texture, const Import::Textures::FTextureImportResult& Source)
    {
        const uint32 Width  = Source.Dimensions.x;
        const uint32 Height = Source.Dimensions.y;
        if (Width == 0 || Height == 0 || Source.Pixels.empty())
        {
            return;
        }

        uint32 FloatChannels = 0;
        switch (Source.Format)
        {
            case EFormat::R32_FLOAT:    FloatChannels = 1; break;
            case EFormat::RG32_FLOAT:   FloatChannels = 2; break;
            case EFormat::RGB32_FLOAT:  FloatChannels = 3; break;
            case EFormat::RGBA32_FLOAT: FloatChannels = 4; break;
            default:                    FloatChannels = 0; break;
        }

        const uint8* RGBA8Source = nullptr;
        TVector<uint8> Converted;

        if (FloatChannels > 0)
        {
            if (Source.Pixels.size() < (size_t)Width * Height * FloatChannels * sizeof(float))
            {
                return;
            }

            const float* Src = reinterpret_cast<const float*>(Source.Pixels.data());
            Converted.resize((size_t)Width * Height * 4);
            for (size_t i = 0; i < (size_t)Width * Height; ++i)
            {
                const float* P = Src + i * FloatChannels;
                const float R = P[0];
                const float G = (FloatChannels >= 2) ? P[1] : P[0];
                const float B = (FloatChannels >= 3) ? P[2] : P[0];

                uint8* Dst = Converted.data() + i * 4;
                Dst[0] = HdrChannelToSRGB8(R);
                Dst[1] = HdrChannelToSRGB8(G);
                Dst[2] = HdrChannelToSRGB8(B);
                Dst[3] = 255;
            }
            RGBA8Source = Converted.data();
        }
        else
        {
            // LDR import path already produced RGBA8.
            if (Source.Pixels.size() < (size_t)Width * Height * 4)
            {
                return;
            }
            RGBA8Source = Source.Pixels.data();
        }

        ThumbnailUtils::StoreDownsampledRGBA(*Texture->GetPackage()->GetPackageThumbnail(),
            RGBA8Source, Width, Height, (size_t)Width * 4);
    }
#endif

    bool CTextureFactory::RecoverSourceImage(CTexture* Texture, Import::Textures::FTextureImportResult& OutResult)
    {
        if (Texture->TextureResource == nullptr || Texture->TextureResource->Mips.empty())
        {
            return false;
        }

        // The top mip is the first thing the streamer evicts, and it is exactly the one needed here.
        Texture->MakeStreamedMipsResident();

        const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[0];
        const EFormat Format = Texture->TextureResource->ImageDescription.Format;

        const uint32 Width  = Mip.Width;
        const uint32 Height = Mip.Height;
        if (Width == 0 || Height == 0 || Mip.Pixels.empty())
        {
            return false;
        }

        OutResult.Dimensions = FUIntVector2(Width, Height);

        if (Format == EFormat::RGBA16_FLOAT)
        {
            const uint64 Texels = (uint64)Width * Height;
            if (Mip.Pixels.size() < Texels * 8)
            {
                return false;
            }

            OutResult.Format = EFormat::RGBA32_FLOAT;
            OutResult.Pixels.resize(Texels * 4 * sizeof(float));

            const uint32* Halves = reinterpret_cast<const uint32*>(Mip.Pixels.data());
            float* Floats = reinterpret_cast<float*>(OutResult.Pixels.data());
            for (uint64 i = 0; i < Texels; ++i)
            {
                const FVector2 RG = Math::UnpackHalf2x16(Halves[i * 2 + 0]);
                const FVector2 BA = Math::UnpackHalf2x16(Halves[i * 2 + 1]);
                Floats[i * 4 + 0] = RG.x;
                Floats[i * 4 + 1] = RG.y;
                Floats[i * 4 + 2] = BA.x;
                Floats[i * 4 + 3] = BA.y;
            }
            return true;
        }

        if (Format == EFormat::RGBA8_UNORM || Format == EFormat::SRGBA8_UNORM)
        {
            const uint64 Required = (uint64)Width * Height * 4;
            if (Mip.Pixels.size() < Required)
            {
                return false;
            }

            OutResult.Format = EFormat::RGBA8_UNORM;
            OutResult.Pixels.assign(Mip.Pixels.begin(), Mip.Pixels.begin() + (size_t)Required);
            return true;
        }

        if (Format != EFormat::BC7_UNORM && Format != EFormat::BC7_UNORM_SRGB)
        {
            return false;
        }

        const uint32 BlocksX = (Width  + 3u) / 4u;
        const uint32 BlocksY = (Height + 3u) / 4u;
        constexpr uint32 BytesPerBlock = 16;

        if (Mip.Pixels.size() < (uint64)BlocksX * BlocksY * BytesPerBlock)
        {
            return false;
        }

        OutResult.Format = EFormat::RGBA8_UNORM;
        OutResult.Pixels.resize((size_t)Width * Height * 4);

        for (uint32 By = 0; By < BlocksY; ++By)
        {
            for (uint32 Bx = 0; Bx < BlocksX; ++Bx)
            {
                basisu::color_rgba Block[16];
                const uint8* Source = Mip.Pixels.data() + ((uint64)By * BlocksX + Bx) * BytesPerBlock;
                if (!basisu::unpack_block(basisu::texture_format::cBC7, Source, Block, false))
                {
                    return false;
                }

                for (uint32 Ty = 0; Ty < 4; ++Ty)
                {
                    const uint32 Y = By * 4 + Ty;
                    if (Y >= Height)
                    {
                        break;
                    }

                    for (uint32 Tx = 0; Tx < 4; ++Tx)
                    {
                        const uint32 X = Bx * 4 + Tx;
                        if (X >= Width)
                        {
                            break;
                        }

                        const basisu::color_rgba& Texel = Block[Ty * 4 + Tx];
                        uint8* Out = &OutResult.Pixels[((size_t)Y * Width + X) * 4];
                        Out[0] = Texel.r;
                        Out[1] = Texel.g;
                        Out[2] = Texel.b;
                        Out[3] = Texel.a;
                    }
                }
            }
        }

        return true;
    }

    bool CTextureFactory::CookIntoTexture(CTexture* Texture, const Import::Textures::FTextureCookRequest& Request)
    {
        if (Texture == nullptr)
        {
            return false;
        }

        if (!Texture->TextureResource)
        {
            Texture->TextureResource = MakeUnique<FTextureResource>();
        }

        // Embedded bytes come from a mesh source and have no file of their own; SourcePath is then only a
        // name hint for the color-space heuristic and is deliberately not persisted.
        const bool bEmbedded = !Request.EmbeddedBytes.empty();
        const FFixedString& SourcePath = Request.SourcePath;

        // DDS containers hold pre-compressed BCn blocks stb_image can't read; pass them straight through to
        // the GPU. Non-DDS falls through to stb.
        {
            auto HasDDSExtension = [](const FFixedString& P) -> bool
            {
                const size_t N = P.size();
                if (N < 4) { return false; }
                const char* S = P.c_str();
                return S[N - 4] == '.'
                    && (S[N - 3] == 'd' || S[N - 3] == 'D')
                    && (S[N - 2] == 'd' || S[N - 2] == 'D')
                    && (S[N - 1] == 's' || S[N - 1] == 'S');
            };

            TVector<uint8>     DDSStorage;
            TSpan<const uint8> DDSBytes;
            if (bEmbedded)
            {
                DDSBytes = Request.EmbeddedBytes;
            }
            else if (HasDDSExtension(SourcePath) && FileHelper::LoadFileToArray(DDSStorage, SourcePath.c_str()))
            {
                DDSBytes = TSpan<const uint8>(DDSStorage.data(), DDSStorage.size());
            }

            if (LooksLikeDDS(DDSBytes))
            {
                const ETextureColorSpace Role = (Request.ColorSpace != ETextureColorSpace::Auto)
                    ? Request.ColorSpace
                    : ClassifyColorSpaceByFilename(SourcePath.c_str());
                Texture->ColorSpace = Role;

                // Editing finished BCn blocks means a decode/re-encode, so the edits are declined out loud.
                if (HasSourceAdjustments(Texture) || Texture->GetResolvedPolicy().MaxDimension > 0)
                {
                    LOG_WARN("TextureFactory: '{0}' is a DDS passthrough; its source adjustments and size cap "
                             "are ignored because the file is already block-compressed.", SourcePath.c_str());
                }

                if (!CookDDS(Texture, DDSBytes, Role, Request.bCreateGPUResource))
                {
                    LOG_WARN("TextureFactory: unsupported DDS format for '{}'.", SourcePath.c_str());
                    return false;
                }

                if (!bEmbedded)
                {
                    Texture->SourcePath = FString(SourcePath.c_str());
                }
                return true;
            }
        }

        // Read once and KEPT, not handed to stb as a path: these bytes become the asset's stored source.
        TVector<uint8> SourceBytes;
        if (bEmbedded)
        {
            SourceBytes.assign(Request.EmbeddedBytes.begin(), Request.EmbeddedBytes.end());
        }
        else if (!FileHelper::LoadFileToArray(SourceBytes, SourcePath.c_str()))
        {
            LOG_ERROR("TextureFactory: could not read '{0}'.", SourcePath.c_str());
            return false;
        }

        TOptional<Import::Textures::FTextureImportResult> MaybeResult =
            Import::Textures::ImportTexture(TSpan<const uint8>(SourceBytes.data(), SourceBytes.size()), false);

        if (!MaybeResult.has_value())
        {
            return false;
        }

        Texture->SourceFile.Reset();
        Texture->SourceFile.Bytes = Move(SourceBytes);

        PrepareSource(Texture, MaybeResult.value());

        const Import::Textures::FTextureImportResult& Result = MaybeResult.value();

        // A caller-supplied role wins; otherwise classify by filename.
        if (Request.ColorSpace != ETextureColorSpace::Auto)
        {
            Texture->ColorSpace = Request.ColorSpace;
        }
        else if (Texture->ColorSpace == ETextureColorSpace::Auto)
        {
            Texture->ColorSpace = ClassifyColorSpaceByFilename(SourcePath.c_str());
        }

        // Float-source data must take the Environment path; Basis would silently corrupt it.
        const bool bIsFloatSource =
            Result.Format == EFormat::R32_FLOAT    ||
            Result.Format == EFormat::RG32_FLOAT   ||
            Result.Format == EFormat::RGB32_FLOAT  ||
            Result.Format == EFormat::RGBA32_FLOAT;
        if (bIsFloatSource)
        {
            Texture->ColorSpace = ETextureColorSpace::Environment;
        }

        #if USING(WITH_EDITOR)
        CreatePackageThumbnail(Texture, Result);
        #endif

        if (!bEmbedded)
        {
            Texture->SourcePath = FString(SourcePath.c_str());
        }

        if (Texture->ColorSpace == ETextureColorSpace::Environment)
        {
            return CookEnvironmentTexture(Texture, Result, Request.bCreateGPUResource);
        }

        if (Import::Textures::FTextureImportResult& Mutable = MaybeResult.value(); NormalizeToRGBA8(Mutable))
        {
            TVector<uint8> Pixels = Move(Mutable.Pixels);
            return CookTexturePixels(Texture, Pixels, Mutable.Dimensions, Texture->ColorSpace,
                                     Request.EncodeThreadBudget, Request.bCreateGPUResource);
        }

        LOG_ERROR("TextureFactory: '{0}' has an unsupported pixel layout for the Basis cook (format {1}, {2}x{3}); import skipped.",
                  Texture->GetName().c_str(), (uint32)Result.Format, Result.Dimensions.x, Result.Dimensions.y);
        return false;
    }

    bool CTextureFactory::CookLayerFromFile(CTexture* Scratch, FStringView SourcePath, ETextureColorSpace ColorSpace,
                                            FUIntVector2 TargetSize)
    {
        if (Scratch == nullptr || Scratch->TextureResource == nullptr)
        {
            return false;
        }
        
        const FFixedString Path(SourcePath.data(), SourcePath.size());
        TOptional<Import::Textures::FTextureImportResult> MaybeResult = Import::Textures::ImportTexture(Path, false, TargetSize);
        if (!MaybeResult.has_value())
        {
            LOG_ERROR("TextureFactory: could not load '{0}' as an array layer.", Path.c_str());
            return false;
        }

        Import::Textures::FTextureImportResult& Result = MaybeResult.value();

        PrepareSource(Scratch, Result);

        const bool bIsFloatSource =
            Result.Format == EFormat::R32_FLOAT    ||
            Result.Format == EFormat::RG32_FLOAT   ||
            Result.Format == EFormat::RGB32_FLOAT  ||
            Result.Format == EFormat::RGBA32_FLOAT;
        if (bIsFloatSource)
        {
            LOG_ERROR("TextureFactory: '{0}' is a float/HDR source; array layers must be LDR.", Path.c_str());
            return false;
        }

        if (!NormalizeToRGBA8(Result))
        {
            LOG_ERROR("TextureFactory: '{0}' has an unsupported pixel layout for the Basis cook (format {1}, {2}x{3}).",
                      Path.c_str(), (uint32)Result.Format, Result.Dimensions.x, Result.Dimensions.y);
            return false;
        }

        // Single-threaded encode: the caller cooks layers in a loop, so a full basisu pool per layer
        // would oversubscribe. EncodeThreads = 1 means "no new threads", not "one extra".
        TVector<uint8> Pixels = Move(Result.Pixels);
        return CookTexturePixels(Scratch, Pixels, Result.Dimensions, ColorSpace, 1u, /*bCreateGPUResource*/ false);
    }

    CTexture* CTextureFactory::CreateSolidColorTexture(FStringView Path, uint8 R, uint8 G, uint8 B, uint8 A, ETextureColorSpace ColorSpace)
    {
        CTexture* Texture = CFactory::CreateNewOf<CTexture>(Path);
        if (Texture == nullptr)
        {
            return nullptr;
        }

        Texture->SetFlag(OF_NeedsPostLoad);
        Texture->ColorSpace = ColorSpace;
        if (!Texture->TextureResource)
        {
            Texture->TextureResource = MakeUnique<FTextureResource>();
        }

        // 4x4 (not 1x1) so the block-compression encoder always has a full BC block to work with.
        constexpr uint32 Dim = 4;
        TVector<uint8> Pixels(Dim * Dim * 4);
        for (uint32 i = 0; i < Dim * Dim; ++i)
        {
            Pixels[i * 4 + 0] = R;
            Pixels[i * 4 + 1] = G;
            Pixels[i * 4 + 2] = B;
            Pixels[i * 4 + 3] = A;
        }

        const FUIntVector2 Extent(Dim, Dim);
        if (!CookTexturePixels(Texture, Pixels, Extent, ColorSpace))
        {
            Texture->ConditionalBeginDestroy();
            return nullptr;
        }

        return Texture;
    }

    bool CTextureFactory::Recook(CTexture* Texture)
    {
        if (Texture == nullptr)
        {
            return false;
        }

        TOptional<Import::Textures::FTextureImportResult> MaybeResult;

        // The file on disk wins when it is there: the user may have edited it since the import.
        if (!Texture->SourcePath.empty())
        {
            MaybeResult = Import::Textures::ImportTexture(Texture->SourcePath, false);
        }

        // Then the copy the import kept. Same pristine bytes, so settings stay absolute with the file gone.
        if (!MaybeResult.has_value() && Texture->LoadSourceFileBytes())
        {
            MaybeResult = Import::Textures::ImportTexture(
                TSpan<const uint8>(Texture->SourceFile.Bytes.data(), Texture->SourceFile.Bytes.size()), false);
        }

        // Last resort, for an asset imported before the source was kept. What it costs is logged below.
        if (!MaybeResult.has_value())
        {
            Import::Textures::FTextureImportResult Recovered;
            if (!RecoverSourceImage(Texture, Recovered))
            {
                const uint32 CookedFormat = Texture->TextureResource
                    ? (uint32)Texture->TextureResource->ImageDescription.Format : 0u;
                LOG_ERROR("TextureFactory::Recook: '{0}' has no readable source and its cooked format ({1}) "
                          "cannot be decoded back to an image; nothing to re-cook from.",
                          Texture->GetName().c_str(), CookedFormat);
                return false;
            }

            // Block-compressed pixels have already lost information, so re-encoding them loses more.
            if (Texture->TextureResource->ImageDescription.Format == EFormat::BC7_UNORM
             || Texture->TextureResource->ImageDescription.Format == EFormat::BC7_UNORM_SRGB)
            {
                LOG_WARN("TextureFactory::Recook: '{0}' is re-cooking from its own BC7 blocks; this is a second "
                         "compression generation. Reimport it to give the asset a stored source.",
                         Texture->GetName().c_str());
            }

            // The recovered pixels already carry the last cook's adjustments, so a new pass compounds.
            if (HasSourceAdjustments(Texture))
            {
                LOG_WARN("TextureFactory::Recook: '{0}' has source adjustments and no stored source; they apply on "
                         "top of the already-cooked pixels rather than replacing the previous pass. Reimport it "
                         "to make them absolute.", Texture->GetName().c_str());
            }

            MaybeResult = Move(Recovered);
        }

        return CookFromSource(Texture, MaybeResult.value());
    }

    bool CTextureFactory::CookFromSource(CTexture* Texture, Import::Textures::FTextureImportResult& Source)
    {
        if (Texture == nullptr)
        {
            return false;
        }

        PrepareSource(Texture, Source);

        // Auto resolves like a fresh import would.
        if (Texture->ColorSpace == ETextureColorSpace::Auto)
        {
            Texture->ColorSpace = ClassifyColorSpaceByFilename(Texture->SourcePath);
        }

        // Float-source data must stay on the Environment path even if the user changed ColorSpace.
        const bool bIsFloatSource =
            Source.Format == EFormat::R32_FLOAT    ||
            Source.Format == EFormat::RG32_FLOAT   ||
            Source.Format == EFormat::RGB32_FLOAT  ||
            Source.Format == EFormat::RGBA32_FLOAT;
        if (bIsFloatSource)
        {
            Texture->ColorSpace = ETextureColorSpace::Environment;
        }

        bool bCooked = false;
        if (Texture->ColorSpace == ETextureColorSpace::Environment)
        {
            bCooked = CookEnvironmentTexture(Texture, Source);
        }
        else if (NormalizeToRGBA8(Source))
        {
            TVector<uint8> Pixels = Move(Source.Pixels);
            bCooked = CookTexturePixels(Texture, Pixels, Source.Dimensions, Texture->ColorSpace);
        }
        else
        {
            LOG_ERROR("TextureFactory::CookFromSource: '{0}' has an unsupported pixel layout for the Basis cook (format {1}, {2}x{3}).",
                      Texture->GetName().c_str(), (uint32)Source.Format, Source.Dimensions.x, Source.Dimensions.y);
        }

        if (!bCooked)
        {
            return false;
        }

        if (CPackage* Package = Texture->GetPackage())
        {
            Package->MarkDirty();
        }

        return true;
    }

}
