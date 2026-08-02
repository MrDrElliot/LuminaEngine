#include "EditorPCH.h"
#include "ScreenshotCapture.h"

#include <chrono>
#include <ctime>

#include "Tools/Image/ImageWrite.h"

#include "Paths/Paths.h"
#include "Renderer/Format.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/Forward/ForwardRenderScene.h"
#include "Log/Log.h"


namespace Lumina::Screenshot
{
    namespace
    {
        FString GenerateDefaultPath(ECaptureSource Source)
        {
            const FString Folder = Paths::GetEngineDirectory() + "/Saved/Screenshots";
            Paths::CreateDirectories(FStringView(Folder.c_str(), Folder.size()));

            const auto Now = std::chrono::system_clock::now();
            const std::time_t T = std::chrono::system_clock::to_time_t(Now);
            std::tm Tm{};
        #if defined(_WIN32)
            localtime_s(&Tm, &T);
        #else
            localtime_r(&T, &Tm);
        #endif

            char Stamp[64];
            std::strftime(Stamp, sizeof(Stamp), "%Y%m%d_%H%M%S", &Tm);

            const char* Ext = (Source == ECaptureSource::SceneHDR) ? ".hdr" : ".png";
            return Folder + "/Screenshot_" + Stamp + Ext;
        }

        // IEEE 754 binary16 -> binary32.
        FORCEINLINE float HalfToFloat(uint16 H)
        {
            const uint32 Sign = (H & 0x8000u) << 16;
            uint32 Exp        = (H & 0x7C00u) >> 10;
            uint32 Mant       = (H & 0x03FFu);

            uint32 Bits;
            if (Exp == 0)
            {
                if (Mant == 0)
                {
                    Bits = Sign;
                }
                else
                {
                    // Subnormal -- normalize.
                    Exp = 1;
                    while ((Mant & 0x0400u) == 0)
                    {
                        Mant <<= 1;
                        --Exp;
                    }
                    Mant &= 0x03FFu;
                    Bits = Sign | ((Exp + (127 - 15)) << 23) | (Mant << 13);
                }
            }
            else if (Exp == 0x1F)
            {
                // Inf / NaN.
                Bits = Sign | 0x7F800000u | (Mant << 13);
            }
            else
            {
                Bits = Sign | ((Exp + (127 - 15)) << 23) | (Mant << 13);
            }

            float Out;
            std::memcpy(&Out, &Bits, sizeof(Out));
            return Out;
        }

        FSceneImage PickSourceImage(IRenderScene* Scene, ECaptureSource Source)
        {
            if (Scene == nullptr)
            {
                return {};
            }

            // Forward is currently the only IRenderScene implementation.
            auto* Forward = static_cast<FForwardRenderScene*>(Scene);

            if (Source == ECaptureSource::SceneHDR)
            {
                return Forward->GetPrimaryNamedImage(FForwardRenderScene::ENamedImage::HDR);
            }

            // Final display-referred output (what the editor viewport shows).
            return Forward->GetDisplayImage();
        }

        bool WritePNG(const FString& OutPath, uint32 Width, uint32 Height, EFormat Format,
                      const uint8* Src, size_t RowPitch, FString& OutError)
        {
            TVector<uint8> Pixels(static_cast<size_t>(Width) * Height * 4u);

            const bool bIsBGRA = (Format == EFormat::BGRA8_UNORM) || (Format == EFormat::SBGRA8_UNORM);
            const bool bIsRGBA = (Format == EFormat::RGBA8_UNORM) || (Format == EFormat::SRGBA8_UNORM);

            if (!bIsBGRA && !bIsRGBA)
            {
                OutError = "Unsupported source format for PNG capture (need RGBA8/BGRA8 variant).";
                return false;
            }

            for (uint32 Y = 0; Y < Height; ++Y)
            {
                const uint8* SrcRow = Src + static_cast<size_t>(Y) * RowPitch;
                uint8*       DstRow = Pixels.data() + static_cast<size_t>(Y) * Width * 4u;

                if (bIsBGRA)
                {
                    for (uint32 X = 0; X < Width; ++X)
                    {
                        DstRow[X * 4 + 0] = SrcRow[X * 4 + 2];
                        DstRow[X * 4 + 1] = SrcRow[X * 4 + 1];
                        DstRow[X * 4 + 2] = SrcRow[X * 4 + 0];
                        DstRow[X * 4 + 3] = SrcRow[X * 4 + 3];
                    }
                }
                else
                {
                    std::memcpy(DstRow, SrcRow, static_cast<size_t>(Width) * 4u);
                }
            }

            if (!ImageWrite::WritePngFile(OutPath.c_str(), Width, Height, 4, Pixels.data(), Width * 4))
            {
                OutError = "PNG encode failed (check the output path is writable).";
                return false;
            }
            return true;
        }

        bool WriteHDR(const FString& OutPath, uint32 Width, uint32 Height, EFormat Format,
                      const uint8* Src, size_t RowPitch, FString& OutError)
        {
            if (Format != EFormat::RGBA16_FLOAT)
            {
                OutError = "Unsupported source format for HDR capture (expected RGBA16_FLOAT).";
                return false;
            }

            TVector<float> Pixels(static_cast<size_t>(Width) * Height * 3u);

            for (uint32 Y = 0; Y < Height; ++Y)
            {
                const uint16* SrcRow = reinterpret_cast<const uint16*>(Src + static_cast<size_t>(Y) * RowPitch);
                float*        DstRow = Pixels.data() + static_cast<size_t>(Y) * Width * 3u;

                for (uint32 X = 0; X < Width; ++X)
                {
                    DstRow[X * 3 + 0] = HalfToFloat(SrcRow[X * 4 + 0]);
                    DstRow[X * 3 + 1] = HalfToFloat(SrcRow[X * 4 + 1]);
                    DstRow[X * 3 + 2] = HalfToFloat(SrcRow[X * 4 + 2]);
                }
            }

            if (!ImageWrite::WriteHdrFile(OutPath.c_str(), Width, Height, 3, Pixels.data()))
            {
                OutError = "HDR encode failed (check the output path is writable).";
                return false;
            }
            return true;
        }
    }

    FCaptureResult Capture(IRenderScene* Scene, ECaptureSource Source, const FString& OutputPath)
    {
        FCaptureResult Out;

        const FSceneImage SrcImage = PickSourceImage(Scene, Source);
        if (!SrcImage.IsValid())
        {
            Out.ErrorMessage = "No source image available for capture.";
            return Out;
        }

        Out.ResolutionX = SrcImage.GetSizeX();
        Out.ResolutionY = SrcImage.GetSizeY();

        // Resolve output path.
        FString ResolvedPath = OutputPath.empty() ? GenerateDefaultPath(Source) : OutputPath;

        // Make sure the parent directory exists when the user passed an explicit path.
        {
            const FString Parent = Paths::Parent(FStringView(ResolvedPath.c_str(), ResolvedPath.size()), true);
            if (!Parent.empty())
            {
                Paths::CreateDirectories(FStringView(Parent.c_str(), Parent.size()));
            }
        }

        // Tightly-packed host readback of the whole image.
        const uint64 TexelBytes = RHI::Format::BytesPerBlock(SrcImage.Desc.Format);
        const uint64 ReadbackSize = (uint64)Out.ResolutionX * Out.ResolutionY * TexelBytes;
        const RHI::GPUPtr Readback = RHI::Malloc(ReadbackSize, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
        if (Readback == 0)
        {
            Out.ErrorMessage = "Failed to allocate readback buffer.";
            return Out;
        }

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
        RHI::CmdCopyTextureToMemory(CL, SrcImage.Texture, RHI::FTextureSlice{}, Readback, Out.ResolutionX);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Host);
        RHI::Submit(CL);
        RHI::WaitDeviceIdle();
        RHI::ResetCommandList(CL);

        const size_t RowPitch = (size_t)Out.ResolutionX * TexelBytes;
        const uint8* Mapped = static_cast<const uint8*>(RHI::ToHost(Readback));

        bool bWriteOK = false;
        if (Mapped == nullptr)
        {
            Out.ErrorMessage = "Failed to map readback buffer.";
        }
        else if (Source == ECaptureSource::SceneHDR)
        {
            bWriteOK = WriteHDR(ResolvedPath, Out.ResolutionX, Out.ResolutionY, SrcImage.Desc.Format,
                                Mapped, RowPitch, Out.ErrorMessage);
        }
        else
        {
            bWriteOK = WritePNG(ResolvedPath, Out.ResolutionX, Out.ResolutionY, SrcImage.Desc.Format,
                                Mapped, RowPitch, Out.ErrorMessage);
        }

        RHI::Free(Readback);

        if (bWriteOK)
        {
            Out.bSuccess = true;
            Out.OutputPath = ResolvedPath;
            LOG_INFO("Saved screenshot ({}x{}) to {}", Out.ResolutionX, Out.ResolutionY, ResolvedPath);
        }
        else
        {
            LOG_WARN("Screenshot capture failed: {}", Out.ErrorMessage);
        }

        return Out;
    }

    FCaptureResult CaptureActiveWorld(ECaptureSource Source, const FString& OutputPath)
    {
        FCaptureResult Out;

        if (GWorldManager == nullptr)
        {
            Out.ErrorMessage = "WorldManager is not initialized.";
            return Out;
        }

        // Prefer an active Game/Simulation world (PIE), fall back to the Editor world.
        IRenderScene* Scene = nullptr;
        FWorldContext* Fallback = nullptr;
        for (const TUniquePtr<FWorldContext>& Ctx : GWorldManager->GetContexts())
        {
            if (!Ctx || !Ctx->World.IsValid())
            {
                continue;
            }

            if (Ctx->Type == EWorldType::Game || Ctx->Type == EWorldType::Simulation)
            {
                Scene = Ctx->World->GetRenderer();
                if (Scene)
                {
                    break;
                }
            }
            else if (Ctx->Type == EWorldType::Editor && Fallback == nullptr)
            {
                Fallback = Ctx.get();
            }
        }

        if (Scene == nullptr && Fallback != nullptr && Fallback->World.IsValid())
        {
            Scene = Fallback->World->GetRenderer();
        }

        if (Scene == nullptr)
        {
            Out.ErrorMessage = "No active world with a render scene was found.";
            return Out;
        }

        return Capture(Scene, Source, OutputPath);
    }
}
