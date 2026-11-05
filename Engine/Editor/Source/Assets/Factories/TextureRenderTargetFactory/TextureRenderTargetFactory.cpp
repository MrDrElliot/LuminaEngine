#include "EditorPCH.h"
#include "TextureRenderTargetFactory.h"
#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"
#include "Core/Math/Math.h"
#include "Core/Math/Packing.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Renderer/Format.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RHIUpload.h"
#include "Renderer/TextureData.h"
#include "Thumbnails/ThumbnailUtils.h"

namespace Lumina
{
    namespace
    {
        // Contents are not serialized, so the capture is whatever the target holds, clear color if fresh.
        void GenerateRenderTargetThumbnail(CTextureRenderTarget* RenderTarget, CPackage* Package)
        {
            const FTextureResource& Resource = RenderTarget->GetTextureResource();
            if (!Resource.NewTexture.IsValid())
            {
                return;
            }

            const uint32 SourceWidth  = Resource.ImageDescription.Extent.x;
            const uint32 SourceHeight = Resource.ImageDescription.Extent.y;
            const bool   bHalfFloat   = Resource.ImageDescription.Format == EFormat::RGBA16_FLOAT;
            const uint64 TexelCount   = (uint64)SourceWidth * SourceHeight;
            const uint64 BytesPerTexel = bHalfFloat ? 8u : 4u;

            const RHI::FGPUAllocation Readback = RHI::Malloc(TexelCount * BytesPerTexel, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            if (Readback.Gpu == 0)
            {
                return;
            }
            RHI::SetDebugName(Readback.Gpu, "Readback.RenderTargetThumbnail");

            // The clear queued by BuildResource only lands at the next BeginFrame otherwise.
            RHI::FlushUploads();

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdCopyTextureToMemory(CL, Resource.NewTexture.Texture, RHI::FTextureSlice{}, Readback, SourceWidth);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Host,
                RHI::EAccessFlags::HostRead);

            const uint64 CaptureValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
            RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), CaptureValue);

            if (const void* MappedMemory = Readback.Cpu)
            {
                if (bHalfFloat)
                {
                    const uint16* Halves = static_cast<const uint16*>(MappedMemory);
                    const int32 ChannelCount = static_cast<int32>(TexelCount * 4u);
                    TVector<uint8> Converted;
                    Converted.SetNum(ChannelCount);
                    for (int32 Channel = 0; Channel < ChannelCount; ++Channel)
                    {
                        Converted[Channel] = static_cast<uint8>(Math::Saturate(Math::UnpackHalf2x16(Halves[Channel]).x) * 255.0f + 0.5f);
                    }
                    ThumbnailUtils::StoreDownsampledRGBA(*Package->GetPackageThumbnail(), Converted.GetData(), SourceWidth, SourceHeight, (size_t)SourceWidth * 4u);
                }
                else
                {
                    ThumbnailUtils::StoreDownsampledRGBA(*Package->GetPackageThumbnail(), static_cast<const uint8*>(MappedMemory), SourceWidth, SourceHeight, (size_t)SourceWidth * 4u);
                }
            }

            RHI::Retire(Readback);
        }
    }

    CObject* CTextureRenderTargetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CTextureRenderTarget* RenderTarget = NewObject<CTextureRenderTarget>(Package, Name);
        // Allocate + clear the GPU image now so the asset is sampleable the moment it's created.
        RenderTarget->BuildResource();
        // Capture the freshly-built image so the content browser has a thumbnail instead of the generic icon.
        GenerateRenderTargetThumbnail(RenderTarget, Package);
        return RenderTarget;
    }
}
