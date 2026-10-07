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
#include "TaskSystem/TaskSystem.h"
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

    // A plain 2x2 average in linear radiance, which is correct for HDR since there is no gamma to undo.
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

            // RGBA16F packs two uint32 per pixel, (R,G) low half and (B,A) high half, eight bytes per pixel.
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

        // A re-cook must keep the published ResourceID, same as CookTexturePixels.
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

        // Skipping this leaves the swap unarmed forever, so the slot never changes residency again.
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

                // Weighted by how UNsaturated the texel already is, so vivid colors are left alone.
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

    static float CoverageAtScale(const TVector<float>& Alpha, float Scale, float Cutoff)
    {
        size_t Passing = 0;
        for (float A : Alpha)
        {
            Passing += (A * Scale >= Cutoff) ? 1 : 0;
        }
        return Alpha.empty() ? 0.0f : float(Passing) / float(Alpha.size());
    }

    // Coverage only rises with the scale, so bisection finds the scale that restores the target.
    static float FindCoverageScale(const TVector<float>& Alpha, float Cutoff, float TargetCoverage)
    {
        constexpr float MaxScale = 16.0f;
        float Low = 0.0f;
        float High = MaxScale;
        for (int32 Step = 0; Step < 20; ++Step)
        {
            const float Mid = (Low + High) * 0.5f;
            if (CoverageAtScale(Alpha, Mid, Cutoff) < TargetCoverage)
            {
                Low = Mid;
            }
            else
            {
                High = Mid;
            }
        }
        return High;
    }

    // Cutout art usually leaves white or black behind its mask, which filtering drags into the visible edge, so hidden texels take the color of the nearest visible ones.
    static void BleedColorIntoCutout(TVector<uint8>& Pixels, FUIntVector2 Dimensions, float Cutoff)
    {
        struct FLevel
        {
            uint32 Width = 0;
            uint32 Height = 0;
            TVector<FVector4> Texels;
        };

        const uint8 Threshold = (uint8)Math::Clamp((int32)std::lround(Cutoff * 255.0f), 1, 255);
        TVector<FLevel> Levels;
        FLevel& Base = Levels.emplace_back();
        Base.Width = Dimensions.x;
        Base.Height = Dimensions.y;
        Base.Texels.resize((size_t)Base.Width * Base.Height);
        bool bAnyHidden = false;
        for (size_t i = 0; i < Base.Texels.size(); ++i)
        {
            const uint8* Texel = &Pixels[i * 4];
            const float Weight = Texel[3] >= Threshold ? 1.0f : 0.0f;
            bAnyHidden |= Weight == 0.0f;
            Base.Texels[i] = FVector4(Texel[0] * Weight, Texel[1] * Weight, Texel[2] * Weight, Weight);
        }
        if (!bAnyHidden)
        {
            return;
        }

        // Pull, so each coarser texel averages whatever visible color lies under it.
        while (Levels.back().Width > 1 || Levels.back().Height > 1)
        {
            const FLevel& Fine = Levels.back();
            FLevel Coarse;
            Coarse.Width = Math::Max(1u, Fine.Width / 2);
            Coarse.Height = Math::Max(1u, Fine.Height / 2);
            Coarse.Texels.resize((size_t)Coarse.Width * Coarse.Height);
            for (uint32 Y = 0; Y < Coarse.Height; ++Y)
            {
                for (uint32 X = 0; X < Coarse.Width; ++X)
                {
                    FVector4 Sum(0.0f);
                    for (uint32 Tap = 0; Tap < 4; ++Tap)
                    {
                        const uint32 SX = Math::Min(X * 2 + (Tap & 1), Fine.Width - 1);
                        const uint32 SY = Math::Min(Y * 2 + (Tap >> 1), Fine.Height - 1);
                        Sum += Fine.Texels[(size_t)SY * Fine.Width + SX];
                    }
                    Coarse.Texels[(size_t)Y * Coarse.Width + X] = Sum;
                }
            }
            Levels.push_back(Move(Coarse));
        }

        // Push, so an empty texel inherits the nearest coarser level that saw any color.
        for (size_t L = Levels.size() - 1; L-- > 0;)
        {
            FLevel& Fine = Levels[L];
            const FLevel& Coarse = Levels[L + 1];
            for (uint32 Y = 0; Y < Fine.Height; ++Y)
            {
                for (uint32 X = 0; X < Fine.Width; ++X)
                {
                    FVector4& Texel = Fine.Texels[(size_t)Y * Fine.Width + X];
                    if (Texel.w <= 0.0f)
                    {
                        const FVector4& Parent = Coarse.Texels[(size_t)Math::Min(Y / 2, Coarse.Height - 1) * Coarse.Width + Math::Min(X / 2, Coarse.Width - 1)];
                        Texel = Parent.w > 0.0f ? Parent / Parent.w : Parent;
                    }
                }
            }
        }

        for (size_t i = 0; i < Base.Texels.size(); ++i)
        {
            uint8* Texel = &Pixels[i * 4];
            if (Texel[3] < Threshold && Levels[0].Texels[i].w > 0.0f)
            {
                const FVector4& Filled = Levels[0].Texels[i];
                Texel[0] = (uint8)Math::Clamp((int32)std::lround(Filled.x / Filled.w), 0, 255);
                Texel[1] = (uint8)Math::Clamp((int32)std::lround(Filled.y / Filled.w), 0, 255);
                Texel[2] = (uint8)Math::Clamp((int32)std::lround(Filled.z / Filled.w), 0, 255);
            }
        }
    }

    // A box-filtered chain down to 1x1, each level's alpha scaled so an alpha test keeps mip 0's coverage.
    static void BuildCoveragePreservingMips(const TVector<uint8>& Pixels, FUIntVector2 Dimensions, bool bSRGB, float Cutoff,
                                            basisu::vector<basisu::image>& OutMips)
    {
        float ToLinear[256];
        for (int32 i = 0; i < 256; ++i)
        {
            const float C = float(i) / 255.0f;
            ToLinear[i] = bSRGB ? (C <= 0.04045f ? C / 12.92f : std::pow((C + 0.055f) / 1.055f, 2.4f)) : C;
        }
        auto ToStored = [bSRGB](float C) -> uint8
        {
            C = Math::Clamp(C, 0.0f, 1.0f);
            if (bSRGB)
            {
                C = C <= 0.0031308f ? C * 12.92f : 1.055f * std::pow(C, 1.0f / 2.4f) - 0.055f;
            }
            return (uint8)std::lround(Math::Clamp(C, 0.0f, 1.0f) * 255.0f);
        };

        uint32 Width = Dimensions.x;
        uint32 Height = Dimensions.y;
        TVector<FVector3> Color((size_t)Width * Height);
        TVector<float> Alpha((size_t)Width * Height);
        for (size_t i = 0; i < Color.size(); ++i)
        {
            const uint8* Texel = &Pixels[i * 4];
            Color[i] = FVector3(ToLinear[Texel[0]], ToLinear[Texel[1]], ToLinear[Texel[2]]);
            Alpha[i] = Texel[3] / 255.0f;
        }

        const float TargetCoverage = CoverageAtScale(Alpha, 1.0f, Cutoff);
        TVector<uint8> Stored;

        while (Width > 1 || Height > 1)
        {
            const uint32 NextWidth = Math::Max(1u, Width / 2);
            const uint32 NextHeight = Math::Max(1u, Height / 2);
            TVector<FVector3> NextColor((size_t)NextWidth * NextHeight);
            TVector<float> NextAlpha((size_t)NextWidth * NextHeight);

            for (uint32 Y = 0; Y < NextHeight; ++Y)
            {
                const uint32 Y0 = Math::Min(Y * 2, Height - 1);
                const uint32 Y1 = Math::Min(Y * 2 + 1, Height - 1);
                for (uint32 X = 0; X < NextWidth; ++X)
                {
                    const uint32 X0 = Math::Min(X * 2, Width - 1);
                    const uint32 X1 = Math::Min(X * 2 + 1, Width - 1);
                    const size_t Taps[4] = { (size_t)Y0 * Width + X0, (size_t)Y0 * Width + X1, (size_t)Y1 * Width + X0, (size_t)Y1 * Width + X1 };

                    // Weighting color by alpha keeps the invisible texels around a cutout from bleeding into its edge.
                    FVector3 Weighted(0.0f);
                    FVector3 Plain(0.0f);
                    float AlphaSum = 0.0f;
                    for (size_t Tap : Taps)
                    {
                        Weighted += Color[Tap] * Alpha[Tap];
                        Plain += Color[Tap];
                        AlphaSum += Alpha[Tap];
                    }

                    const size_t Out = (size_t)Y * NextWidth + X;
                    NextColor[Out] = AlphaSum > 1e-4f ? Weighted / AlphaSum : Plain * 0.25f;
                    NextAlpha[Out] = AlphaSum * 0.25f;
                }
            }

            Width = NextWidth;
            Height = NextHeight;
            Color = Move(NextColor);
            Alpha = Move(NextAlpha);

            const float Scale = FindCoverageScale(Alpha, Cutoff, TargetCoverage);
            Stored.resize(Color.size() * 4);
            for (size_t i = 0; i < Color.size(); ++i)
            {
                Stored[i * 4 + 0] = ToStored(Color[i].x);
                Stored[i * 4 + 1] = ToStored(Color[i].y);
                Stored[i * 4 + 2] = ToStored(Color[i].z);
                Stored[i * 4 + 3] = (uint8)std::lround(Math::Clamp(Alpha[i] * Scale, 0.0f, 1.0f) * 255.0f);
            }

            OutMips.push_back(basisu::image());
            OutMips.back().init(Stored.data(), Width, Height, 4);
        }
    }

    static float SRGBToLinear(float C)
    {
        return C <= 0.04045f ? C / 12.92f : std::pow((C + 0.055f) / 1.055f, 2.4f);
    }

    static float LinearToSRGB(float C)
    {
        C = Math::Clamp(C, 0.0f, 1.0f);
        return C <= 0.0031308f ? C * 12.92f : 1.055f * std::pow(C, 1.0f / 2.4f) - 0.055f;
    }

    // Formats with no sRGB variant hold linear values, and these are the settings the cook writes without basisu.
    static bool IsDirectEncoded(ETextureCompressionSettings Settings)
    {
        switch (Settings)
        {
        case ETextureCompressionSettings::Grayscale:
        case ETextureCompressionSettings::Alpha:
        case ETextureCompressionSettings::TwoChannel:
        case ETextureCompressionSettings::UserInterface2D:
        case ETextureCompressionSettings::GrayscaleUncompressed:
            return true;
        default:
            return false;
        }
    }

    struct FRGBA8Level
    {
        uint32         Width  = 0;
        uint32         Height = 0;
        TVector<uint8> Texels;
    };

    // Averaged in linear light for an sRGB source, since a gamma-space average darkens every mip.
    static FRGBA8Level DownsampleRGBA8(const FRGBA8Level& Source, bool bSRGB)
    {
        static const TArray<float, 256> GToLinear = []
        {
            TArray<float, 256> Table;
            for (int32 i = 0; i < 256; ++i)
            {
                Table[i] = SRGBToLinear(float(i) / 255.0f);
            }
            return Table;
        }();

        FRGBA8Level Next;
        Next.Width  = Math::Max(1u, Source.Width / 2);
        Next.Height = Math::Max(1u, Source.Height / 2);
        Next.Texels.resize((size_t)Next.Width * Next.Height * 4);

        for (uint32 Y = 0; Y < Next.Height; ++Y)
        {
            const uint32 Y0 = Math::Min(Y * 2, Source.Height - 1);
            const uint32 Y1 = Math::Min(Y * 2 + 1, Source.Height - 1);
            for (uint32 X = 0; X < Next.Width; ++X)
            {
                const uint32 X0 = Math::Min(X * 2, Source.Width - 1);
                const uint32 X1 = Math::Min(X * 2 + 1, Source.Width - 1);
                const size_t Taps[4] = { (size_t)Y0 * Source.Width + X0, (size_t)Y0 * Source.Width + X1,
                                         (size_t)Y1 * Source.Width + X0, (size_t)Y1 * Source.Width + X1 };

                uint8* Out = &Next.Texels[((size_t)Y * Next.Width + X) * 4];
                for (uint32 Channel = 0; Channel < 4; ++Channel)
                {
                    const bool bGamma = bSRGB && Channel < 3;
                    float Sum = 0.0f;
                    for (size_t Tap : Taps)
                    {
                        const uint8 Value = Source.Texels[Tap * 4 + Channel];
                        Sum += bGamma ? GToLinear[Value] : float(Value) / 255.0f;
                    }
                    const float Average = Sum * 0.25f;
                    Out[Channel] = (uint8)std::lround((bGamma ? LinearToSRGB(Average) : Math::Clamp(Average, 0.0f, 1.0f)) * 255.0f);
                }
            }
        }
        return Next;
    }

    // BC4 decodes to these eight values, kept unrounded because hardware interpolates at higher precision.
    static void BC4Palette(int32 E0, int32 E1, float Out[8])
    {
        Out[0] = float(E0);
        Out[1] = float(E1);
        if (E0 > E1)
        {
            for (int32 i = 1; i <= 6; ++i)
            {
                Out[i + 1] = float((7 - i) * E0 + i * E1) / 7.0f;
            }
            return;
        }

        for (int32 i = 1; i <= 4; ++i)
        {
            Out[i + 1] = float((5 - i) * E0 + i * E1) / 5.0f;
        }
        Out[6] = 0.0f;
        Out[7] = 255.0f;
    }

    static float FitBC4Indices(const float Values[16], int32 E0, int32 E1, uint8 Indices[16])
    {
        float Palette[8];
        BC4Palette(E0, E1, Palette);

        float Error = 0.0f;
        for (int32 Texel = 0; Texel < 16; ++Texel)
        {
            float BestError = FLT_MAX;
            for (uint8 Index = 0; Index < 8; ++Index)
            {
                const float Delta = Values[Texel] - Palette[Index];
                if (Delta * Delta < BestError)
                {
                    BestError = Delta * Delta;
                    Indices[Texel] = Index;
                }
            }
            Error += BestError;
        }
        return Error;
    }

    // Values are 0 to 255, texels row by row; SearchRadius widens the endpoint search for higher quality settings.
    static void EncodeBC4Block(const float Values[16], int32 SearchRadius, uint8 Out[8])
    {
        float Lo = 255.0f, Hi = 0.0f;
        float InnerLo = 255.0f, InnerHi = 0.0f;
        for (int32 Texel = 0; Texel < 16; ++Texel)
        {
            const float Value = Values[Texel];
            Lo = Math::Min(Lo, Value);
            Hi = Math::Max(Hi, Value);
            if (Value > 0.5f && Value < 254.5f)
            {
                InnerLo = Math::Min(InnerLo, Value);
                InnerHi = Math::Max(InnerHi, Value);
            }
        }

        int32 BestE0 = 0, BestE1 = 0;
        uint8 BestIndices[16] = {};
        float BestError = FLT_MAX;
        auto Try = [&](int32 E0, int32 E1)
        {
            E0 = Math::Clamp(E0, 0, 255);
            E1 = Math::Clamp(E1, 0, 255);
            uint8 Indices[16];
            const float Error = FitBC4Indices(Values, E0, E1, Indices);
            if (Error < BestError)
            {
                BestError = Error;
                BestE0 = E0;
                BestE1 = E1;
                Memory::Memcpy(BestIndices, Indices, sizeof(Indices));
            }
        };

        const int32 HiQ = (int32)std::lround(Hi);
        const int32 LoQ = (int32)std::lround(Lo);
        for (int32 D0 = -SearchRadius; D0 <= SearchRadius; ++D0)
        {
            for (int32 D1 = -SearchRadius; D1 <= SearchRadius; ++D1)
            {
                if (HiQ + D0 > LoQ + D1)
                {
                    Try(HiQ + D0, LoQ + D1);
                }
            }
        }

        // The six-value mode, which decodes a flat block exactly and gets pure black and white for free.
        Try(LoQ, HiQ);
        if (InnerLo <= InnerHi)
        {
            Try((int32)std::lround(InnerLo), (int32)std::lround(InnerHi));
        }

        Out[0] = (uint8)BestE0;
        Out[1] = (uint8)BestE1;
        uint64 Bits = 0;
        for (int32 Texel = 0; Texel < 16; ++Texel)
        {
            Bits |= uint64(BestIndices[Texel]) << (Texel * 3);
        }
        for (int32 Byte = 0; Byte < 6; ++Byte)
        {
            Out[2 + Byte] = uint8(Bits >> (Byte * 8));
        }
    }

    static int32 BC4SearchRadius(ETextureCompressionQuality Quality)
    {
        switch (Quality)
        {
        case ETextureCompressionQuality::High:    return 1;
        case ETextureCompressionQuality::Highest: return 2;
        default:                                  return 0;
        }
    }

    // One BC4 block per listed channel, so BC4 takes one channel and BC5 takes red then green.
    static TVector<uint8> EncodeBC4Channels(const FRGBA8Level& Level, TSpan<const uint32> Channels, bool bLinearizeColor, int32 SearchRadius)
    {
        static const TArray<float, 256> GLinear255 = []
        {
            TArray<float, 256> Table;
            for (int32 i = 0; i < 256; ++i)
            {
                Table[i] = SRGBToLinear(float(i) / 255.0f) * 255.0f;
            }
            return Table;
        }();

        const uint32 BlocksX = (Level.Width + 3) / 4;
        const uint32 BlocksY = (Level.Height + 3) / 4;
        const uint32 BlockBytes = 8u * (uint32)Channels.size();
        TVector<uint8> Encoded((size_t)BlocksX * BlocksY * BlockBytes);

        Task::ParallelFor(BlocksY, [&](uint32 By)
        {
            for (uint32 Bx = 0; Bx < BlocksX; ++Bx)
            {
                uint8* Block = &Encoded[((size_t)By * BlocksX + Bx) * BlockBytes];
                for (size_t Slot = 0; Slot < Channels.size(); ++Slot)
                {
                    const uint32 Channel = Channels[Slot];
                    const bool bLinearize = bLinearizeColor && Channel < 3;

                    float Values[16];
                    for (uint32 Ty = 0; Ty < 4; ++Ty)
                    {
                        const uint32 Y = Math::Min(By * 4 + Ty, Level.Height - 1);
                        for (uint32 Tx = 0; Tx < 4; ++Tx)
                        {
                            const uint32 X = Math::Min(Bx * 4 + Tx, Level.Width - 1);
                            const uint8 Value = Level.Texels[((size_t)Y * Level.Width + X) * 4 + Channel];
                            Values[Ty * 4 + Tx] = bLinearize ? GLinear255[Value] : float(Value);
                        }
                    }
                    EncodeBC4Block(Values, SearchRadius, Block + Slot * 8);
                }
            }
        });
        return Encoded;
    }

    static TVector<uint8> ExtractR8(const FRGBA8Level& Level, bool bLinearize)
    {
        TVector<uint8> Out((size_t)Level.Width * Level.Height);
        for (size_t i = 0; i < Out.size(); ++i)
        {
            const uint8 Value = Level.Texels[i * 4];
            Out[i] = bLinearize ? (uint8)std::lround(SRGBToLinear(float(Value) / 255.0f) * 255.0f) : Value;
        }
        return Out;
    }

    // Shared tail of every cook, so a recook keeps the ResourceID materials have already baked.
    static bool PublishCookedMips(CTexture* Texture, bool bCreateGPUResource)
    {
        ApplyTextureGroupMipPolicy(Texture);
        if (!bCreateGPUResource)
        {
            return true;
        }

        const FTextureResource::FDescription& Desc = Texture->TextureResource->ImageDescription;
        const uint32 UploadMips = (uint32)Texture->TextureResource->Mips.size();
        const FString DebugName = "Texture." + Texture->GetName().ToString();
        RHI::Textures::Recreate(Texture->TextureResource->NewTexture, RHI::FTexture2DDesc
        {
            .Width  = Desc.Extent.x,
            .Height = Desc.Extent.y,
            .Mips   = UploadMips,
            .Format = Desc.Format,
            .DebugName = DebugName.c_str(),
            .Swizzle = Desc.Swizzle,
        });
        for (uint32 i = 0; i < UploadMips; ++i)
        {
            const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[i];
            if (!Mip.Pixels.empty())
            {
                RHI::Textures::Upload(Texture->TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width, Mip.Width, Mip.Height);
            }
        }

        // Skipping this leaves the swap unarmed forever, so the slot never changes residency again.
        RHI::Textures::CommitRecreate(Texture->TextureResource->NewTexture);
        Texture->OnFullyUploadedExternally();
        return true;
    }

    static void ResetCookedResource(CTexture* Texture, EFormat Format, ETextureSwizzle Swizzle, FUIntVector2 Extent, uint32 NumMips)
    {
        if (!Texture->TextureResource)
        {
            Texture->TextureResource = MakeUnique<FTextureResource>();
        }

        FTextureResource::FDescription Desc;
        Desc.Format  = Format;
        Desc.Swizzle = Swizzle;
        Desc.Extent  = Extent;
        Desc.NumMips = (uint8)NumMips;

        Texture->TextureResource->ImageDescription = Desc;
        Texture->TextureResource->Mips.clear();
        Texture->TextureResource->Mips.resize(NumMips);
    }

    static void StoreMip(CTexture* Texture, uint32 MipIndex, uint32 Width, uint32 Height, uint32 RowPitch, TVector<uint8>&& Bytes)
    {
        FTextureResource::FMip& Mip = Texture->TextureResource->Mips[MipIndex];
        Mip.Width      = Width;
        Mip.Height     = Height;
        Mip.Depth      = 1;
        Mip.RowPitch   = RowPitch;
        Mip.SlicePitch = (uint32)Bytes.size();
        Mip.Pixels     = Move(Bytes);
    }

    // The formats basisu cannot produce, encoded straight from the RGBA8 source with our own mips.
    static bool CookDirectEncoded(CTexture* Texture, TVector<uint8>& Pixels, FUIntVector2 Dimensions, bool bSRGB, bool bCreateGPUResource)
    {
        const ETextureCompressionSettings Settings = Texture->CompressionSettings;
        const bool bPreserveCoverage = Texture->bPreserveAlphaCoverage && !Texture->bCompressWithoutAlpha;
        if (bPreserveCoverage)
        {
            BleedColorIntoCutout(Pixels, Dimensions, Texture->AlphaCoverageCutoff);
        }

        TVector<FRGBA8Level> Levels;
        Levels.push_back(FRGBA8Level{ Dimensions.x, Dimensions.y, Pixels });

        if (Texture->GetResolvedPolicy().bGenerateMips)
        {
            if (bPreserveCoverage)
            {
                basisu::vector<basisu::image> CoverageMips;
                BuildCoveragePreservingMips(Pixels, Dimensions, bSRGB, Texture->AlphaCoverageCutoff, CoverageMips);
                for (const basisu::image& Image : CoverageMips)
                {
                    FRGBA8Level& Level = Levels.emplace_back();
                    Level.Width  = Image.get_width();
                    Level.Height = Image.get_height();
                    Level.Texels.resize((size_t)Level.Width * Level.Height * 4);
                    for (uint32 Y = 0; Y < Level.Height; ++Y)
                    {
                        for (uint32 X = 0; X < Level.Width; ++X)
                        {
                            const basisu::color_rgba& Texel = Image(X, Y);
                            uint8* Out = &Level.Texels[((size_t)Y * Level.Width + X) * 4];
                            Out[0] = Texel.r;
                            Out[1] = Texel.g;
                            Out[2] = Texel.b;
                            Out[3] = Texel.a;
                        }
                    }
                }
            }
            else
            {
                while (Levels.back().Width > 1 || Levels.back().Height > 1)
                {
                    FRGBA8Level Next = DownsampleRGBA8(Levels.back(), bSRGB);
                    Levels.push_back(Move(Next));
                }
            }
        }

        EFormat Format = EFormat::RGBA8_UNORM;
        ETextureSwizzle Swizzle = ETextureSwizzle::Identity;
        switch (Settings)
        {
        case ETextureCompressionSettings::Grayscale:             Format = EFormat::BC4_UNORM; Swizzle = ETextureSwizzle::Grayscale; break;
        case ETextureCompressionSettings::Alpha:                 Format = EFormat::BC4_UNORM; Swizzle = ETextureSwizzle::Alpha; break;
        case ETextureCompressionSettings::TwoChannel:            Format = EFormat::BC5_UNORM; break;
        case ETextureCompressionSettings::GrayscaleUncompressed: Format = EFormat::R8_UNORM; Swizzle = ETextureSwizzle::Grayscale; break;
        default:                                                 Format = bSRGB ? EFormat::SRGBA8_UNORM : EFormat::RGBA8_UNORM; break;
        }

        ResetCookedResource(Texture, Format, Swizzle, Dimensions, (uint32)Levels.size());

        const int32 SearchRadius = BC4SearchRadius(Texture->CompressionQuality);
        constexpr uint32 RedOnly[]   = { 0 };
        constexpr uint32 AlphaOnly[] = { 3 };
        constexpr uint32 RedGreen[]  = { 0, 1 };

        for (uint32 MipIndex = 0; MipIndex < (uint32)Levels.size(); ++MipIndex)
        {
            FRGBA8Level& Level = Levels[MipIndex];
            const uint32 BlocksX = (Level.Width + 3) / 4;
            switch (Format)
            {
            case EFormat::BC4_UNORM:
            {
                const bool bAlpha = Swizzle == ETextureSwizzle::Alpha;
                TVector<uint8> Encoded = bAlpha
                    ? EncodeBC4Channels(Level, TSpan<const uint32>(AlphaOnly), false, SearchRadius)
                    : EncodeBC4Channels(Level, TSpan<const uint32>(RedOnly), bSRGB, SearchRadius);
                StoreMip(Texture, MipIndex, Level.Width, Level.Height, BlocksX * 8, Move(Encoded));
                break;
            }
            case EFormat::BC5_UNORM:
                StoreMip(Texture, MipIndex, Level.Width, Level.Height, BlocksX * 16,
                         EncodeBC4Channels(Level, TSpan<const uint32>(RedGreen), bSRGB, SearchRadius));
                break;
            case EFormat::R8_UNORM:
                StoreMip(Texture, MipIndex, Level.Width, Level.Height, Level.Width, ExtractR8(Level, bSRGB));
                break;
            default:
                StoreMip(Texture, MipIndex, Level.Width, Level.Height, Level.Width * 4, Move(Level.Texels));
                break;
            }
        }

        return PublishCookedMips(Texture, bCreateGPUResource);
    }

    static bool IsFloatSourceFormat(EFormat Format)
    {
        return Format == EFormat::R32_FLOAT || Format == EFormat::RG32_FLOAT || Format == EFormat::RGB32_FLOAT || Format == EFormat::RGBA32_FLOAT;
    }

    // The first channel at the source's own precision, so a 16-bit or float heightmap keeps its steps.
    static bool ReadFirstChannel(const Import::Textures::FTextureImportResult& Source, TVector<float>& Out)
    {
        const uint64 Texels = (uint64)Source.Dimensions.x * Source.Dimensions.y;
        uint32 Stride = 0;
        uint32 Bytes  = 1;
        switch (Source.Format)
        {
        case EFormat::R8_UNORM:     Stride = 1; break;
        case EFormat::RG8_UNORM:    Stride = 2; break;
        case EFormat::RGBA8_UNORM:
        case EFormat::SRGBA8_UNORM: Stride = 4; break;
        case EFormat::R16_UNORM:    Stride = 1; Bytes = 2; break;
        case EFormat::RG16_UNORM:   Stride = 2; Bytes = 2; break;
        case EFormat::RGBA16_UNORM: Stride = 4; Bytes = 2; break;
        case EFormat::R32_FLOAT:    Stride = 1; Bytes = 4; break;
        case EFormat::RG32_FLOAT:   Stride = 2; Bytes = 4; break;
        case EFormat::RGB32_FLOAT:  Stride = 3; Bytes = 4; break;
        case EFormat::RGBA32_FLOAT: Stride = 4; Bytes = 4; break;
        default:
            return false;
        }

        if (Texels == 0 || Source.Pixels.size() < Texels * Stride * Bytes)
        {
            return false;
        }

        Out.resize(Texels);
        for (uint64 i = 0; i < Texels; ++i)
        {
            const uint8* Texel = &Source.Pixels[i * Stride * Bytes];
            if (Bytes == 1)
            {
                Out[i] = float(Texel[0]) / 255.0f;
            }
            else if (Bytes == 2)
            {
                uint16 Value;
                Memory::Memcpy(&Value, Texel, sizeof(Value));
                Out[i] = float(Value) / 65535.0f;
            }
            else
            {
                float Value;
                Memory::Memcpy(&Value, Texel, sizeof(Value));
                Out[i] = std::isfinite(Value) ? Math::Clamp(Value, -65504.0f, 65504.0f) : 0.0f;
            }
        }
        return true;
    }

    static bool CookHalfFloatTexture(CTexture* Texture, const Import::Textures::FTextureImportResult& Source, bool bCreateGPUResource)
    {
        TVector<float> Level;
        if (!ReadFirstChannel(Source, Level))
        {
            LOG_ERROR("TextureFactory: '{0}' has an unsupported pixel layout for HalfFloat (format {1}).",
                      Texture->GetName().c_str(), (uint32)Source.Format);
            return false;
        }

        if (HasPerTexelAdjustments(Texture))
        {
            LOG_WARN("TextureFactory: '{0}' cooks as HalfFloat, which ignores per-texel adjustments to keep the source's precision.",
                     Texture->GetName().c_str());
        }

        const bool bLinearize = Texture->IsSRGB() && !IsFloatSourceFormat(Source.Format);
        if (bLinearize)
        {
            for (float& Value : Level)
            {
                Value = SRGBToLinear(Value);
            }
        }

        uint32 Width = Source.Dimensions.x;
        uint32 Height = Source.Dimensions.y;
        const bool bGenerateMips = Texture->GetResolvedPolicy().bGenerateMips;
        uint32 NumMips = 1;
        if (bGenerateMips)
        {
            for (uint32 Size = Math::Max(Width, Height); Size > 1; Size /= 2)
            {
                ++NumMips;
            }
        }

        ResetCookedResource(Texture, EFormat::R16_FLOAT, ETextureSwizzle::Grayscale, Source.Dimensions, NumMips);

        for (uint32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
        {
            TVector<uint8> Bytes((size_t)Width * Height * 2);
            for (size_t i = 0; i < (size_t)Width * Height; ++i)
            {
                const uint16 Half = (uint16)(Math::PackHalf2x16(FVector2(Level[i], 0.0f)) & 0xFFFFu);
                Memory::Memcpy(&Bytes[i * 2], &Half, sizeof(Half));
            }
            StoreMip(Texture, MipIndex, Width, Height, Width * 2, Move(Bytes));

            if (MipIndex + 1 < NumMips)
            {
                const uint32 NextWidth = Math::Max(1u, Width / 2);
                const uint32 NextHeight = Math::Max(1u, Height / 2);
                TVector<float> Next((size_t)NextWidth * NextHeight);
                for (uint32 Y = 0; Y < NextHeight; ++Y)
                {
                    const uint32 Y0 = Math::Min(Y * 2, Height - 1);
                    const uint32 Y1 = Math::Min(Y * 2 + 1, Height - 1);
                    for (uint32 X = 0; X < NextWidth; ++X)
                    {
                        const uint32 X0 = Math::Min(X * 2, Width - 1);
                        const uint32 X1 = Math::Min(X * 2 + 1, Width - 1);
                        Next[(size_t)Y * NextWidth + X] = 0.25f * (Level[(size_t)Y0 * Width + X0] + Level[(size_t)Y0 * Width + X1]
                                                                 + Level[(size_t)Y1 * Width + X0] + Level[(size_t)Y1 * Width + X1]);
                    }
                }
                Level = Move(Next);
                Width = NextWidth;
                Height = NextHeight;
            }
        }

        return PublishCookedMips(Texture, bCreateGPUResource);
    }

    // An 8-bit source widened to float, so an HDR setting on an ordinary image still cooks.
    static void PromoteToFloat(const CTexture* Texture, Import::Textures::FTextureImportResult& Source)
    {
        const uint64 Texels = (uint64)Source.Dimensions.x * Source.Dimensions.y;
        const bool bSRGB = Texture->IsSRGB();
        TVector<uint8> Floats(Texels * 4 * sizeof(float));
        float* Out = reinterpret_cast<float*>(Floats.data());
        for (uint64 i = 0; i < Texels; ++i)
        {
            for (uint32 Channel = 0; Channel < 4; ++Channel)
            {
                const float Value = float(Source.Pixels[i * 4 + Channel]) / 255.0f;
                Out[i * 4 + Channel] = (bSRGB && Channel < 3) ? SRGBToLinear(Value) : Value;
            }
        }
        Source.Pixels = Move(Floats);
        Source.Format = EFormat::RGBA32_FLOAT;
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

        const bool bIsSRGB     = (ColorSpace == ETextureColorSpace::SRGB);

        if (IsDirectEncoded(Texture->CompressionSettings))
        {
            return CookDirectEncoded(Texture, Pixels, Dimensions, bIsSRGB, bCreateGPUResource);
        }

        basisu::basisu_encoder_init();

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
        Params.m_status_output              = false;   // silence the per-slice progress spam during cook
        const bool bGenerateMips = Texture->GetResolvedPolicy().bGenerateMips;
        const bool bCoverageMips = bGenerateMips && Texture->bPreserveAlphaCoverage && !Texture->bCompressWithoutAlpha;
        if (Texture->bPreserveAlphaCoverage && !Texture->bCompressWithoutAlpha)
        {
            BleedColorIntoCutout(Pixels, Dimensions, Texture->AlphaCoverageCutoff);
            Params.m_source_images[0].init(Pixels.data(), Dimensions.x, Dimensions.y, 4);
        }
        if (bCoverageMips)
        {
            Params.m_source_mipmap_images.resize(1);
            BuildCoveragePreservingMips(Pixels, Dimensions, bIsSRGB, Texture->AlphaCoverageCutoff, Params.m_source_mipmap_images[0]);
        }

        Params.m_mip_gen                    = bGenerateMips && !bCoverageMips;
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
        if (!Transcoder.start_transcoding(BasisData.data(), (uint32_t)BasisData.size()))
        {
            return false;
        }

        basist::basisu_file_info FileInfo;
        Transcoder.get_file_info(BasisData.data(), (uint32_t)BasisData.size(), FileInfo);
        const uint32 NumMips = FileInfo.m_image_mipmap_levels[0];

        basist::basisu_image_info ImageInfo;
        Transcoder.get_image_info(BasisData.data(), (uint32_t)BasisData.size(), ImageInfo, 0);
        
        const uint32 Width  = ImageInfo.m_orig_width;
        const uint32 Height = ImageInfo.m_orig_height;

        // Normals cook as full BC7 RGB rather than BC5, since basisu's BC5 takes red and alpha, not red and green.
        const bool bNoAlpha = Texture->CompressionSettings == ETextureCompressionSettings::NoAlpha;
        const EFormat StoredFormat = bNoAlpha
            ? (bIsSRGB ? EFormat::BC1_UNORM_SRGB : EFormat::BC1_UNORM)
            : (bIsSRGB ? EFormat::BC7_UNORM_SRGB : EFormat::BC7_UNORM);
        const basist::transcoder_texture_format TranscodeTarget = bNoAlpha
            ? basist::transcoder_texture_format::cTFBC1_RGB
            : basist::transcoder_texture_format::cTFBC7_RGBA;

        ResetCookedResource(Texture, StoredFormat, ETextureSwizzle::Identity, FUIntVector2(Width, Height), NumMips);

        const uint32 BytesPerBlock = RHI::Format::BytesPerBlock(StoredFormat);

        for (uint32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
        {
            basist::basisu_image_level_info LevelInfo;
            if (!Transcoder.get_image_level_info(BasisData.data(), (uint32_t)BasisData.size(), LevelInfo, 0, MipIndex))
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
                    BasisData.data(), (uint32_t)BasisData.size(),
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

        return PublishCookedMips(Texture, bCreateGPUResource);
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

    // Maps the DDS format to EFormat and uploads the blocks VERBATIM, losslessly, with no recompress.
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
                break;   // truncated file, keep whatever mips parsed cleanly
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
        if (Format == EFormat::BC4_UNORM)
        {
            Desc.Swizzle = ETextureSwizzle::Grayscale;
            Texture->CompressionSettings = ETextureCompressionSettings::Grayscale;
        }
        Texture->TextureResource->ImageDescription = Desc;

        return PublishCookedMips(Texture, bCreateGPUResource);
    }

    // Filename suffix heuristic for Auto; falls back to SRGB. Editable in the inspector for misclassifications.
    static bool CookImportedSource(CTexture* Texture, Import::Textures::FTextureImportResult& Source, uint32 EncodeThreads, bool bCreateGPUResource)
    {
        if (Texture->CompressionSettings == ETextureCompressionSettings::HalfFloat)
        {
            return CookHalfFloatTexture(Texture, Source, bCreateGPUResource);
        }

        const bool bFloatSource = IsFloatSourceFormat(Source.Format);
        if (Texture->ColorSpace == ETextureColorSpace::Environment || Texture->CompressionSettings == ETextureCompressionSettings::HDR)
        {
            if (!bFloatSource)
            {
                if (!NormalizeToRGBA8(Source))
                {
                    return false;
                }
                PromoteToFloat(Texture, Source);
            }
            return CookEnvironmentTexture(Texture, Source, bCreateGPUResource);
        }

        if (!NormalizeToRGBA8(Source))
        {
            LOG_ERROR("TextureFactory: '{0}' has an unsupported pixel layout for the cook (format {1}, {2}x{3}).",
                      Texture->GetName().c_str(), (uint32)Source.Format, Source.Dimensions.x, Source.Dimensions.y);
            return false;
        }

        TVector<uint8> Pixels = Move(Source.Pixels);
        return CookTexturePixels(Texture, Pixels, Source.Dimensions, Texture->ColorSpace, EncodeThreads, bCreateGPUResource);
    }

    ETextureColorSpace CTextureFactory::ClassifyColorSpaceByFilename(FStringView Path)
    {
        FString Stem(Path.data(), Path.size());

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
        if (DotPos != FString::npos)
        {
            Stem.resize(DotPos);
        }

        auto EndsWith = [&Stem](const char* Suffix)
        {
            const size_t SufLen = strlen(Suffix);
            return Stem.size() >= SufLen && Stem.compare(Stem.size() - SufLen, SufLen, Suffix) == 0;
        };

        // Normals resolve to Linear BC7 rather than NormalMap, since the BC5-packed path is broken.
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

    // Float sources are tonemapped so they no longer clip to white, while 8-bit sources pass through.
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

        const ETextureSwizzle Swizzle = Texture->TextureResource->ImageDescription.Swizzle;
        const uint64 Texels = (uint64)Width * Height;

        // The cook linearized color for formats with no sRGB variant, so an sRGB texture gets its gamma back here.
        static const TArray<uint8, 256> GLinearToSRGB8 = []
        {
            TArray<uint8, 256> Table;
            for (int32 i = 0; i < 256; ++i)
            {
                Table[i] = (uint8)std::lround(LinearToSRGB(float(i) / 255.0f) * 255.0f);
            }
            return Table;
        }();
        const bool bRestoreGamma = Texture->IsSRGB() && Swizzle != ETextureSwizzle::Alpha;
        auto Color = [&](uint8 Value) { return bRestoreGamma ? GLinearToSRGB8[Value] : Value; };

        auto WriteTexel = [&](uint64 Index, uint8 R, uint8 G, uint8 B, uint8 A)
        {
            uint8* Out = &OutResult.Pixels[Index * 4];
            switch (Swizzle)
            {
            case ETextureSwizzle::Grayscale: Out[0] = Out[1] = Out[2] = Color(R); Out[3] = 0xFF; break;
            case ETextureSwizzle::Alpha:     Out[0] = Out[1] = Out[2] = 0xFF; Out[3] = R; break;
            case ETextureSwizzle::Identity:  Out[0] = R; Out[1] = G; Out[2] = B; Out[3] = A; break;
            }
        };

        OutResult.Format = EFormat::RGBA8_UNORM;
        OutResult.Pixels.resize((size_t)Texels * 4);

        if (Format == EFormat::R8_UNORM)
        {
            if (Mip.Pixels.size() < Texels)
            {
                return false;
            }
            for (uint64 i = 0; i < Texels; ++i)
            {
                WriteTexel(i, Mip.Pixels[i], 0, 0, 0xFF);
            }
            return true;
        }

        if (Format == EFormat::R16_FLOAT)
        {
            if (Mip.Pixels.size() < Texels * 2)
            {
                return false;
            }
            for (uint64 i = 0; i < Texels; ++i)
            {
                uint16 Half;
                Memory::Memcpy(&Half, &Mip.Pixels[i * 2], sizeof(Half));
                const float Value = Math::UnpackHalf2x16((uint32)Half).x;
                WriteTexel(i, (uint8)std::lround(Math::Clamp(Value, 0.0f, 1.0f) * 255.0f), 0, 0, 0xFF);
            }
            return true;
        }

        basisu::texture_format BlockFormat;
        bool bLinearColor = false;
        switch (Format)
        {
        case EFormat::BC7_UNORM:
        case EFormat::BC7_UNORM_SRGB: BlockFormat = basisu::texture_format::cBC7; break;
        case EFormat::BC1_UNORM:
        case EFormat::BC1_UNORM_SRGB: BlockFormat = basisu::texture_format::cBC1; break;
        case EFormat::BC4_UNORM:      BlockFormat = basisu::texture_format::cBC4; break;
        case EFormat::BC5_UNORM:      BlockFormat = basisu::texture_format::cBC5; bLinearColor = true; break;
        default:
            return false;
        }

        const uint32 BlocksX = (Width  + 3u) / 4u;
        const uint32 BlocksY = (Height + 3u) / 4u;
        const uint32 BytesPerBlock = RHI::Format::BytesPerBlock(Format);

        if (Mip.Pixels.size() < (uint64)BlocksX * BlocksY * BytesPerBlock)
        {
            return false;
        }

        for (uint32 By = 0; By < BlocksY; ++By)
        {
            for (uint32 Bx = 0; Bx < BlocksX; ++Bx)
            {
                basisu::color_rgba Block[16];
                for (basisu::color_rgba& Texel : Block)
                {
                    Texel.set(0, 0, 0, 255);
                }

                const uint8* Source = Mip.Pixels.data() + ((uint64)By * BlocksX + Bx) * BytesPerBlock;
                if (!basisu::unpack_block(BlockFormat, Source, Block, false))
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
                        const uint64 Index = (uint64)Y * Width + X;
                        if (bLinearColor)
                        {
                            WriteTexel(Index, Color(Texel.r), Color(Texel.g), 0, 0xFF);
                        }
                        else
                        {
                            WriteTexel(Index, Texel.r, Texel.g, Texel.b, Texel.a);
                        }
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

        // Embedded bytes have no file, so SourcePath is only a color-space name hint and is not persisted.
        const bool bEmbedded = !Request.EmbeddedBytes.empty();
        const FFixedString& SourcePath = Request.SourcePath;

        // DDS holds pre-compressed blocks stb_image cannot read, so pass them straight through to the GPU.
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

        // Read once and KEPT rather than handed to stb as a path, since these become the stored source.
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

        if (Request.AlphaCoverageCutoff > 0.0f)
        {
            Texture->bPreserveAlphaCoverage = true;
            Texture->AlphaCoverageCutoff    = Request.AlphaCoverageCutoff;
        }

        PrepareSource(Texture, MaybeResult.value());

        const Import::Textures::FTextureImportResult& Result = MaybeResult.value();

        // A new texture starts as SRGB rather than Auto, so an Auto request classifies by filename unconditionally.
        Texture->ColorSpace = Request.ColorSpace != ETextureColorSpace::Auto
            ? Request.ColorSpace
            : ClassifyColorSpaceByFilename(SourcePath.c_str());

        // Float-source data must take the Environment path; Basis would silently corrupt it.
        if (IsFloatSourceFormat(Result.Format) && Texture->CompressionSettings != ETextureCompressionSettings::HalfFloat)
        {
            Texture->ColorSpace = ETextureColorSpace::Environment;
        }

        // A one-channel file would spend three quarters of a BC7 block on copies of the same value.
        const bool bSingleChannelSource = Result.Format == EFormat::R8_UNORM || Result.Format == EFormat::R16_UNORM;
        if (bSingleChannelSource && Texture->CompressionSettings == ETextureCompressionSettings::Default)
        {
            Texture->CompressionSettings = ETextureCompressionSettings::Grayscale;
        }

        #if USING(WITH_EDITOR)
        CreatePackageThumbnail(Texture, Result);
        #endif

        if (!bEmbedded)
        {
            Texture->SourcePath = FString(SourcePath.c_str());
        }

        return CookImportedSource(Texture, MaybeResult.value(), Request.EncodeThreadBudget, Request.bCreateGPUResource);
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

        // The caller cooks layers in a loop, so a full basisu pool per layer would oversubscribe.
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

        // The file on disk wins when present, since the user may have edited it since the import.
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
            if (RHI::Format::Info(Texture->TextureResource->ImageDescription.Format).BlockSize > 1)
            {
                LOG_WARN("TextureFactory::Recook: '{0}' is re-cooking from its own compressed blocks; this is a second "
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
        if (IsFloatSourceFormat(Source.Format) && Texture->CompressionSettings != ETextureCompressionSettings::HalfFloat)
        {
            Texture->ColorSpace = ETextureColorSpace::Environment;
        }

        if (!CookImportedSource(Texture, Source, 0, true))
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
