#include "pch.h"
#include "TextureFactory.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "encoder/basisu_comp.h"
#include "encoder/basisu_enc.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RendererUtils.h"
#include "Renderer/RHITexture.h"
#include "Tools/Import/ImportHelpers.h"
#include "Thumbnails/ThumbnailUtils.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    CObject* CTextureFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CTexture>(Package, Name);
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

    // HDR/Environment cook: bypasses Basis (LDR-only) and stores RGBA16F with a full box-filtered mip
    // chain. The mips let the visible-sky pass sample a screen-derivative LOD so the equirect stops
    // shimmering; the IBL cube prefilter still owns its own roughness chain.
    static bool CookEnvironmentTexture(CTexture* Texture, const Import::Textures::FTextureImportResult& Source)
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

        const float* SrcFloats = reinterpret_cast<const float*>(Source.Pixels.data());

        // Radiance values above half-float max (65504) pack to +Inf, and Inf/NaN texels poison the IBL
        // convolution downstream (black/white reflection blocks + bloom flashes). Drop non-finite to 0 and
        // clamp into a finite, format-safe range so the stored panorama is well-conditioned.
        auto Sanitize = [](float X) -> float
        {
            if (!std::isfinite(X)) return 0.0f;
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

        // New RHI: create + upload from the CPU mips (same path as CTexture::PostLoad).
        Texture->TextureResource->NewTexture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Width,
            .Height = Height,
            .Mips   = NumMips,
            .Format = EFormat::RGBA16_FLOAT,
        });
        for (uint32 i = 0; i < NumMips; ++i)
        {
            const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[i];
            RHI::Textures::Upload(Texture->TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width);
        }

        return true;
    }

    // Encodes RGBA8 via Basis Universal; shared by initial import and Recook.
    // EncodeThreads = total basisu encode threads (basisu counts the calling thread, so 1 = single-threaded,
    // 0 new threads). 0 = auto (use the worker count). A batch importer cooking many textures in parallel
    // passes 1 so each texture doesn't spawn its own full pool and oversubscribe the cores.
    static bool CookTexturePixels(CTexture* Texture, const TVector<uint8>& Pixels, FUIntVector2 Dimensions, ETextureColorSpace ColorSpace, uint32 EncodeThreads = 0)
    {
        // basisu's encoder tables are per-DLL static; FEngine::Init's init runs in Runtime,
        // so init the Editor's copy here. Idempotent (mutex + g_library_initialized).
        basisu::basisu_encoder_init();

        const bool bIsSRGB     = (ColorSpace == ETextureColorSpace::SRGB);
        const bool bIsNormalMap = (ColorSpace == ETextureColorSpace::NormalMap);

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
        Params.m_mip_gen                    = true;
        Params.m_mip_fast                   = true;
        Params.m_multithreading             = (TotalEncodeThreads > 1);
        Params.m_create_ktx2_file           = false;
        Params.m_quality_level              = 128;
        Params.m_pack_uastc_ldr_4x4_flags   = basisu::cPackUASTCLevelFastest;

        // Perceptual mode must match storage format or sRGB albedos look wrong.
        Params.m_perceptual = bIsSRGB;
        Params.m_mip_srgb   = bIsSRGB;

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
        const uint32 Width  = ImageInfo.m_width;
        const uint32 Height = ImageInfo.m_height;

        // SRGB->BC7_UNORM_SRGB; NormalMap->BC5_UNORM (shader reconstructs Z); Linear/Packed->BC7_UNORM.
        EFormat StoredFormat;
        basist::transcoder_texture_format TranscodeTarget;
        if (bIsSRGB)
        {
            StoredFormat    = EFormat::BC7_UNORM_SRGB;
            TranscodeTarget = basist::transcoder_texture_format::cTFBC7_RGBA;
        }
        else if (bIsNormalMap)
        {
            // BC5-packed normals are currently broken, so cook normal maps as full BC7 RGB instead. The
            // material output node reconstructs Z from XY, so a linear RGB normal renders correctly. This
            // makes a NormalMap-tagged texture safe even if one is set manually (importers now tag Linear).
            StoredFormat    = EFormat::BC7_UNORM;
            TranscodeTarget = basist::transcoder_texture_format::cTFBC7_RGBA;
        }
        else
        {
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
            Mip.Width      = LevelInfo.m_width;
            Mip.Height     = LevelInfo.m_height;
            Mip.RowPitch   = RowPitch;
            Mip.Depth      = 1;
            Mip.SlicePitch = DepthPitch;
            Mip.Pixels     = Move(TranscodedData);
        }

        // New RHI: create + upload the (block-compressed) mips into the global heap.
        const FUIntVector2 Extent = Texture->TextureResource->ImageDescription.Extent;
        Texture->TextureResource->NewTexture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Extent.x,
            .Height = Extent.y,
            .Mips   = NumMips,
            .Format = StoredFormat,
        });
        for (uint32 i = 0; i < NumMips; ++i)
        {
            const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[i];
            if (!Mip.Pixels.empty())
            {
                RHI::Textures::Upload(Texture->TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width);
            }
        }

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
    static bool CookDDS(CTexture* Texture, TSpan<const uint8> Data, ETextureColorSpace ColorSpace)
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

        Texture->TextureResource->NewTexture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Width,
            .Height = Height,
            .Mips   = StoredMips,
            .Format = Format,
        });
        for (uint32 i = 0; i < StoredMips; ++i)
        {
            const FTextureResource::FMip& Mip = Texture->TextureResource->Mips[i];
            if (!Mip.Pixels.empty())
            {
                RHI::Textures::Upload(Texture->TextureResource->NewTexture, i, Mip.Pixels.data(), Mip.Pixels.size(), Mip.Width);
            }
        }

        return true;
    }

    // Filename suffix heuristic for Auto; falls back to SRGB. Editable in the inspector for misclassifications.
    static ETextureColorSpace ClassifyByFilename(FStringView Path)
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

    void CTextureFactory::TryImport(const FFixedString& RawPath, const FFixedString& DestinationPath, const Import::FImportSettings* Settings)
    {
        CTexture* NewTexture = TryCreateNew<CTexture>(DestinationPath);
        NewTexture->SetFlag(OF_NeedsPostLoad);

        NewTexture->TextureResource = MakeUnique<FTextureResource>();
        
        TOptional<Import::Textures::FTextureImportResult> MaybeResult;
        const Import::Mesh::FMeshImportImage* ImageSettings = nullptr;

        if (Settings)
        {
            ImageSettings = &Settings->As<Import::Mesh::FMeshImportImage>();
        }

        // DDS containers hold pre-compressed BCn blocks stb_image can't read; pass them straight through to the
        // GPU (handles both .dds bytes embedded in a mesh and external .dds files). Non-DDS falls through to stb.
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
            if (ImageSettings && ImageSettings->IsBytes())
            {
                DDSBytes = TSpan<const uint8>(ImageSettings->Bytes.data(), ImageSettings->Bytes.size());
            }
            else if (HasDDSExtension(RawPath) && FileHelper::LoadFileToArray(DDSStorage, RawPath.c_str()))
            {
                DDSBytes = TSpan<const uint8>(DDSStorage.data(), DDSStorage.size());
            }

            if (LooksLikeDDS(DDSBytes))
            {
                const ETextureColorSpace Role = (ImageSettings && ImageSettings->IntendedColorSpace != ETextureColorSpace::Auto)
                    ? ImageSettings->IntendedColorSpace
                    : ClassifyByFilename(RawPath.c_str());
                NewTexture->ColorSpace = Role;

                if (!CookDDS(NewTexture, DDSBytes, Role))
                {
                    LOG_WARN("TextureFactory: unsupported DDS format for '{}'; using neutral default.", RawPath.c_str());
                    NewTexture->ConditionalBeginDestroy();
                    return;
                }

                if (!ImageSettings || !ImageSettings->IsBytes())
                {
                    NewTexture->SourcePath = FString(RawPath.c_str());
                }

                CPackage* NewPackage = NewTexture->GetPackage();
                if (CPackage::SavePackage(NewPackage, NewPackage->GetPackagePath()))
                {
                    FAssetRegistry::Get().AssetCreated(NewTexture);
                }
                else
                {
                    LOG_ERROR("TextureFactory: failed to save imported DDS texture; asset will not be registered");
                }
                NewTexture->ConditionalBeginDestroy();
                return;
            }
        }

        // Bytes path = mesh-embedded; file path = direct or mesh-resolved URI.
        if (ImageSettings && ImageSettings->IsBytes())
        {
            MaybeResult = Import::Textures::ImportTexture(ImageSettings->Bytes, false);
        }
        else
        {
            MaybeResult = Import::Textures::ImportTexture(RawPath, false);
        }

        if (!MaybeResult.has_value())
        {
            // Refcount-safe teardown (matches the success path below): frees the unreferenced transient
            // texture, but won't dangle a holder if one ever takes a ref.
            NewTexture->ConditionalBeginDestroy();
            return;
        }

        const Import::Textures::FTextureImportResult& Result = MaybeResult.value();

        // Mesh-supplied role wins; otherwise classify by filename for direct drags.
        if (ImageSettings && ImageSettings->IntendedColorSpace != ETextureColorSpace::Auto)
        {
            NewTexture->ColorSpace = ImageSettings->IntendedColorSpace;
        }
        else if (NewTexture->ColorSpace == ETextureColorSpace::Auto)
        {
            NewTexture->ColorSpace = ClassifyByFilename(RawPath.c_str());
        }

        // Float-source data must take the Environment path; Basis would silently corrupt it.
        const bool bIsFloatSource =
            Result.Format == EFormat::R32_FLOAT    ||
            Result.Format == EFormat::RG32_FLOAT   ||
            Result.Format == EFormat::RGB32_FLOAT  ||
            Result.Format == EFormat::RGBA32_FLOAT;
        if (bIsFloatSource)
        {
            NewTexture->ColorSpace = ETextureColorSpace::Environment;
        }

#if USING(WITH_EDITOR)
        // HDR (Environment) sources are tonemapped inside; all texture types get a thumbnail.
        CreatePackageThumbnail(NewTexture, Result);
#endif

        // Persist source path for Recook; bytes-only imports leave it empty.
        if (!ImageSettings || !ImageSettings->IsBytes())
        {
            NewTexture->SourcePath = FString(RawPath.c_str());
        }

        bool bCooked = false;
        if (NewTexture->ColorSpace == ETextureColorSpace::Environment)
        {
            bCooked = CookEnvironmentTexture(NewTexture, Result);
        }
        else
        {
            // Batch importers pass a per-texture encode-thread budget (1 = single-threaded) so many textures
            // cooking in parallel don't each spawn a full basisu pool. Direct/standalone imports leave it 0 (auto).
            const uint32 EncodeThreads = ImageSettings ? ImageSettings->EncodeThreadBudget : 0u;
            TVector<uint8> Pixels = Move(Result.Pixels);
            bCooked = CookTexturePixels(NewTexture, Pixels, Result.Dimensions, NewTexture->ColorSpace, EncodeThreads);
        }

        if (!bCooked)
        {
            NewTexture->ConditionalBeginDestroy();
            return;
        }

        CPackage* NewPackage = NewTexture->GetPackage();
        if (CPackage::SavePackage(NewPackage, NewPackage->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetCreated(NewTexture);
        }
        else
        {
            LOG_ERROR("TextureFactory: failed to save imported texture; asset will not be registered");
        }

        NewTexture->ConditionalBeginDestroy();
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

        if (!CookTexturePixels(Texture, Pixels, FUIntVector2(Dim, Dim), ColorSpace))
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

        if (Texture->SourcePath.empty())
        {
            LOG_WARN("TextureFactory::Recook: '{0}' has no source path (likely mesh-embedded); cannot re-cook from disk.",
                     Texture->GetName().c_str());
            return false;
        }

        TOptional<Import::Textures::FTextureImportResult> MaybeResult = Import::Textures::ImportTexture(Texture->SourcePath, false);
        if (!MaybeResult.has_value())
        {
            LOG_WARN("TextureFactory::Recook: failed to load source file '{0}' for '{1}'.",
                     Texture->SourcePath.c_str(), Texture->GetName().c_str());
            return false;
        }

        const Import::Textures::FTextureImportResult& Result = MaybeResult.value();

        // Re-cook with Auto resolves like a fresh import.
        if (Texture->ColorSpace == ETextureColorSpace::Auto)
        {
            Texture->ColorSpace = ClassifyByFilename(Texture->SourcePath);
        }

        // Float-source data must stay on the Environment path even if user changed ColorSpace.
        const bool bIsFloatSource =
            Result.Format == EFormat::R32_FLOAT    ||
            Result.Format == EFormat::RG32_FLOAT   ||
            Result.Format == EFormat::RGB32_FLOAT  ||
            Result.Format == EFormat::RGBA32_FLOAT;
        if (bIsFloatSource)
        {
            Texture->ColorSpace = ETextureColorSpace::Environment;
        }

        bool bCooked = false;
        if (Texture->ColorSpace == ETextureColorSpace::Environment)
        {
            bCooked = CookEnvironmentTexture(Texture, Result);
        }
        else
        {
            TVector<uint8> Pixels = Move(Result.Pixels);
            bCooked = CookTexturePixels(Texture, Pixels, Result.Dimensions, Texture->ColorSpace);
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
