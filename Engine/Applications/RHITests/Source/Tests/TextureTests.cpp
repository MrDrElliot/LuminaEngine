#include "RHITestHarness.h"

#include "Renderer/RHITexture.h"
#include "Renderer/RHIUpload.h"

namespace Lumina::RHITests
{
    RHI_TEST(Textures, Create2D)
    {
        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(64, EFormat::RGBA8_UNORM), "RHITests.Tex2D");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const RHI::FTextureDesc Read = RHI::GetTextureDesc(Texture);
        RHI_CHECK_EQ(Read.Dimension.x, 64u);
        RHI_CHECK_EQ(Read.Dimension.y, 64u);
    }

    RHI_TEST(Textures, Create2DArray)
    {
        RHI::FTextureDesc Desc;
        Desc.Type       = RHI::ETextureType::Tex2DArray;
        Desc.Dimension  = FUIntVector3(32, 32, 1);
        Desc.LayerCount = 4;
        Desc.Format     = EFormat::RGBA8_UNORM;
        Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;

        const RHI::FTextureH Texture = Ctx.CreateTexture(Desc, "RHITests.Tex2DArray");
        RHI_REQUIRE(RHI::IsValid(Texture));
        RHI_CHECK_EQ(RHI::GetTextureDesc(Texture).LayerCount, 4u);
    }

    RHI_TEST(Textures, Create3D)
    {
        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex3D;
        Desc.Dimension = FUIntVector3(16, 16, 16);
        Desc.Format    = EFormat::RGBA8_UNORM;
        Desc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;

        const RHI::FTextureH Texture = Ctx.CreateTexture(Desc, "RHITests.Tex3D");
        RHI_REQUIRE(RHI::IsValid(Texture));
        RHI_CHECK_EQ(RHI::GetTextureDesc(Texture).Dimension.z, 16u);
    }

    RHI_TEST(Textures, CreateCube)
    {
        RHI::FTextureDesc Desc;
        Desc.Type       = RHI::ETextureType::TexCube;
        Desc.Dimension  = FUIntVector3(32, 32, 1);
        Desc.LayerCount = 6;
        Desc.Format     = EFormat::RGBA16_FLOAT;
        Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;

        RHI_CHECK(RHI::IsValid(Ctx.CreateTexture(Desc, "RHITests.TexCube")));
    }

    RHI_TEST(Textures, CreateMipChain)
    {
        RHI::FTextureDesc Desc = MakeSampledDesc(64, EFormat::RGBA8_UNORM, RHI::EImageUsageFlags::Storage);
        Desc.MipCount = 7;   // 64 -> 1

        const RHI::FTextureH Texture = Ctx.CreateTexture(Desc, "RHITests.TexMips");
        RHI_REQUIRE(RHI::IsValid(Texture));
        RHI_CHECK_EQ(RHI::GetTextureDesc(Texture).MipCount, 7u);
    }

    RHI_TEST(Textures, ClearColorReadsBack)
    {
        constexpr uint32 Size = 8;
        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(Size, EFormat::RGBA8_UNORM), "RHITests.Clear");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const uint64 Bytes = (uint64)Size * Size * 4;
        const RHI::FGPUAllocation Readback = Ctx.Malloc(Bytes, RHI::EMemoryType::CPURead, "RHITests.ClearReadback");
        RHI_REQUIRE(Readback.Gpu != 0);

        const RHI::FCmdListH CL = Ctx.OpenCL();

        const float Clear[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        RHI::Barriers::AllToTransfer(CL);
        RHI::CmdClearTexture(CL, Texture, Clear);
        RHI::Barriers::TransferToTransfer(CL);

        RHI::FTextureSlice Slice;
        Slice.Extent = FUIntVector3(Size, Size, 1);
        RHI::CmdCopyTextureToMemory(CL, Texture, Slice, Readback.Gpu, Size);
        RHI::Barriers::TransferToAll(CL);

        Ctx.SubmitAndWait(CL);

        const auto* Pixels = Readback.CpuAs<const uint8>();
        RHI_REQUIRE(Pixels != nullptr);
        RHI_CHECK_EQ(Pixels[0], 255u);   // R
        RHI_CHECK_EQ(Pixels[1], 0u);     // G
        RHI_CHECK_EQ(Pixels[3], 255u);   // A
    }

    // The depth aspect goes down vkCmdClearDepthStencilImage, the call the Reset Pass makes.
    RHI_TEST(Textures, ClearDepth)
    {
        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(64, 64, 1);
        Desc.Format    = EFormat::D32;
        Desc.Usage     = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled
                       | RHI::EImageUsageFlags::TransferDst;

        const RHI::FTextureH Texture = Ctx.CreateTexture(Desc, "RHITests.ClearDepth");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const RHI::FCmdListH CL = Ctx.OpenCL();
        const float Clear[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        RHI::Barriers::AllToTransfer(CL);
        RHI::CmdClearTexture(CL, Texture, Clear);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);
    }

    RHI_TEST(Textures, ClearUInt)
    {
        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(16, EFormat::R32_UINT), "RHITests.ClearUInt");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const RHI::FCmdListH CL = Ctx.OpenCL();
        const uint32 Clear[4] = { 0xFFFFFFFFu, 0u, 0u, 0u };
        RHI::Barriers::AllToTransfer(CL);
        RHI::CmdClearTextureUInt(CL, Texture, Clear);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);
    }

    RHI_TEST(Textures, CopyTextureToTexture)
    {
        constexpr uint32 Size = 16;
        const RHI::FTextureH Source = Ctx.CreateTexture(MakeSampledDesc(Size, EFormat::RGBA8_UNORM), "RHITests.CopySrcTex");
        const RHI::FTextureH Dest   = Ctx.CreateTexture(MakeSampledDesc(Size, EFormat::RGBA8_UNORM), "RHITests.CopyDstTex");
        RHI_REQUIRE(RHI::IsValid(Source) && RHI::IsValid(Dest));

        RHI::FTextureSlice Slice;
        Slice.Extent = FUIntVector3(Size, Size, 1);

        const RHI::FCmdListH CL = Ctx.OpenCL();
        const float Clear[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        RHI::Barriers::AllToTransfer(CL);
        RHI::CmdClearTexture(CL, Source, Clear);
        RHI::Barriers::TransferToTransfer(CL);
        RHI::CmdCopyTexture(CL, Source, Slice, Dest, Slice);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);
    }

    RHI_TEST(Textures, BlitDownsample)
    {
        const RHI::FTextureH Source = Ctx.CreateTexture(MakeSampledDesc(32, EFormat::RGBA8_UNORM), "RHITests.BlitSrc");
        const RHI::FTextureH Dest   = Ctx.CreateTexture(MakeSampledDesc(16, EFormat::RGBA8_UNORM), "RHITests.BlitDst");
        RHI_REQUIRE(RHI::IsValid(Source) && RHI::IsValid(Dest));

        RHI::FTextureSlice SourceSlice;
        SourceSlice.Extent = FUIntVector3(32, 32, 1);
        RHI::FTextureSlice DestSlice;
        DestSlice.Extent = FUIntVector3(16, 16, 1);

        const RHI::FCmdListH CL = Ctx.OpenCL();
        const float Clear[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        RHI::Barriers::AllToTransfer(CL);
        RHI::CmdClearTexture(CL, Source, Clear);
        RHI::Barriers::TransferToTransfer(CL);
        RHI::CmdBlitTexture(CL, Source, SourceSlice, Dest, DestSlice, RHI::EFilter::Linear);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);
    }

    RHI_TEST(Textures, UploadAndReadBack)
    {
        constexpr uint32 Size = 4;
        uint8 Source[Size * Size * 4];
        for (uint32 i = 0; i < sizeof(Source); ++i)
        {
            Source[i] = (uint8)(i * 3u);
        }

        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(Size, EFormat::RGBA8_UNORM), "RHITests.Upload");
        RHI_REQUIRE(RHI::IsValid(Texture));

        RHI::UploadTexture(Texture, 0, 0, Source, sizeof(Source), Size);
        RHI::FlushUploadsAndWait();

        const RHI::FGPUAllocation Readback = Ctx.Malloc(sizeof(Source), RHI::EMemoryType::CPURead, "RHITests.UploadReadback");
        RHI_REQUIRE(Readback.Gpu != 0);

        RHI::FTextureSlice Slice;
        Slice.Extent = FUIntVector3(Size, Size, 1);

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::Barriers::AllToTransfer(CL);
        RHI::CmdCopyTextureToMemory(CL, Texture, Slice, Readback.Gpu, Size);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);

        const auto* Pixels = Readback.CpuAs<const uint8>();
        RHI_REQUIRE(Pixels != nullptr);
        RHI_CHECK_EQ(Pixels[0], Source[0]);
        RHI_CHECK_EQ(Pixels[sizeof(Source) - 1], Source[sizeof(Source) - 1]);
    }

    RHI_TEST(Textures, ManagedCreateUploadRelease)
    {
        RHI::FManagedTexture Managed = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = 8,
            .Height = 8,
            .Format = EFormat::RGBA8_UNORM,
            .DebugName = "RHITests.Managed",
        });
        RHI_REQUIRE(Managed.IsValid());
        RHI_CHECK(Managed.SampledSlot != RHI::kInvalidHeapSlot);

        uint8 Pixels[8 * 8 * 4] = {};
        RHI::Textures::Upload(Managed, 0, Pixels, sizeof(Pixels), 8);
        RHI::FlushUploadsAndWait();

        RHI::Textures::Release(Managed);
        RHI_CHECK(!Managed.IsValid());
    }

    // A re-cook replaces the image but keeps the published index, so baked ResourceIDs stay correct.
    RHI_TEST(Textures, RecreateKeepsSampledSlot)
    {
        RHI::FManagedTexture Managed = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = 8,
            .Height = 8,
            .Format = EFormat::RGBA8_UNORM,
            .DebugName = "RHITests.RecreateBefore",
        });
        RHI_REQUIRE(Managed.IsValid());

        const uint32 OriginalSlot    = Managed.SampledSlot;
        const RHI::FTextureH Original = Managed.Texture;

        RHI::Textures::Recreate(Managed, RHI::FTexture2DDesc
        {
            .Width  = 16,
            .Height = 16,
            .Format = EFormat::RGBA8_UNORM,
            .DebugName = "RHITests.RecreateAfter",
        });

        RHI_CHECK_EQ(Managed.SampledSlot, OriginalSlot);
        RHI_CHECK(Managed.Texture.Handle != Original.Handle);
        RHI_CHECK_EQ(RHI::GetTextureDesc(Managed.Texture).Dimension.x, 16u);

        RHI::Textures::Release(Managed);
    }

    RHI_TEST(Textures, StorageSlotIsStable)
    {
        RHI::FManagedTexture Managed = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width    = 8,
            .Height   = 8,
            .Format   = EFormat::RGBA8_UNORM,
            .bStorage = true,
            .DebugName = "RHITests.StorageSlot",
        });
        RHI_REQUIRE(Managed.IsValid());

        const uint32 First  = RHI::Textures::StorageSlot(Managed, 0);
        const uint32 Second = RHI::Textures::StorageSlot(Managed, 0);
        RHI_CHECK(First != RHI::kInvalidHeapSlot);
        RHI_CHECK_EQ(Second, First);   // cached, not re-registered

        RHI::Textures::Release(Managed);
    }
}
