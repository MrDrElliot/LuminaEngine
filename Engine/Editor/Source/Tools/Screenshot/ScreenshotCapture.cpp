#include "Containers/StringFormat.h"
#include "Platform/Time/PlatformTime.h"
#include "EditorPCH.h"
#include "ScreenshotCapture.h"

#include <ctime>

#include "Tools/Image/ImageWrite.h"

#include "Core/Engine/Engine.h"
#include "Core/Math/Packing.h"
#include "Memory/Memcpy.h"
#include "Paths/Paths.h"
#include "Renderer/Format.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/Default/DefaultSceneRenderer.h"
#include "Log/Log.h"


namespace Lumina::Screenshot
{
    namespace
    {
        FString GenerateDefaultPath(ECaptureSource Source)
        {
            const FString Folder = GetScreenshotDirectory();
            Paths::CreateDirectories(FStringView(*Folder, Folder.Len()));

            const PlatformTime::FDateTime Now = PlatformTime::LocalNow();

            const FString Stamp = Lumina::Format("{:04}{:02}{:02}_{:02}{:02}{:02}",
                Now.Year, Now.Month, Now.Day, Now.Hour, Now.Minute, Now.Second);

            const char* Ext = (Source == ECaptureSource::SceneHDR) ? ".hdr" : ".png";
            return Folder + "/Screenshot_" + Stamp + Ext;
        }

        FSceneImage PickSourceImage(IRenderScene* Scene, ECaptureSource Source)
        {
            if (Scene == nullptr)
            {
                return {};
            }

            // Forward is currently the only IRenderScene implementation.
            auto* Renderer = static_cast<FDefaultSceneRenderer*>(Scene);

            if (Source == ECaptureSource::SceneHDR)
            {
                return Renderer->GetPrimaryNamedImage(FDefaultSceneRenderer::ENamedImage::HDR);
            }

            // Final display-referred output (what the editor viewport shows).
            return Renderer->GetDisplayImage();
        }

        bool ReadImageBytes(const FSceneImage& Image, TVector<uint8>& OutBytes, FString& OutError)
        {
            const uint64 Size = (uint64)Image.GetSizeX() * Image.GetSizeY() * RHI::Format::BytesPerBlock(Image.Desc.Format);
            const RHI::FGPUAllocation Readback = RHI::Malloc(Size, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            if (Readback.Gpu == 0)
            {
                OutError = "Failed to allocate a readback buffer.";
                return false;
            }
            RHI::SetDebugName(Readback.Gpu, "Readback.Screenshot");

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdCopyTextureToMemory(CL, Image.Texture, RHI::FTextureSlice{}, Readback, Image.GetSizeX());
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Host,
                RHI::EAccessFlags::HostRead);
            const uint64 CaptureValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
            RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), CaptureValue);

            const uint8* Mapped = Readback.CpuAs<const uint8>();
            if (Mapped != nullptr)
            {
                OutBytes.SetNum(IntCastChecked<int32>(Size));
                Memory::Memcpy(OutBytes.GetData(), Mapped, (size_t)Size);
            }
            RHI::Retire(Readback);

            if (Mapped == nullptr)
            {
                OutError = "Failed to map the readback buffer.";
                return false;
            }
            return true;
        }

        // Returns false when Format is not an 8-bit RGBA or BGRA variant.
        bool SwizzleToRGBA8(EFormat Format, TVector<uint8>& Pixels)
        {
            if (Format == EFormat::RGBA8_UNORM || Format == EFormat::SRGBA8_UNORM)
            {
                return true;
            }
            if (Format != EFormat::BGRA8_UNORM && Format != EFormat::SBGRA8_UNORM)
            {
                return false;
            }
            for (int32 i = 0; i + 3 < Pixels.Num(); i += 4)
            {
                const uint8 Blue = Pixels[i];
                Pixels[i] = Pixels[i + 2];
                Pixels[i + 2] = Blue;
            }
            return true;
        }

        bool WritePNG(const FString& OutPath, uint32 Width, uint32 Height, EFormat Format,
                      TVector<uint8>& Pixels, FString& OutError)
        {
            if (!SwizzleToRGBA8(Format, Pixels))
            {
                OutError = "Unsupported source format for PNG capture (need RGBA8/BGRA8 variant).";
                return false;
            }

            if (!ImageWrite::WritePngFile(*OutPath, Width, Height, 4, Pixels.GetData(), Width * 4))
            {
                OutError = "PNG encode failed (check the output path is writable).";
                return false;
            }
            return true;
        }

        bool WriteHDR(const FString& OutPath, uint32 Width, uint32 Height, EFormat Format,
                      const TVector<uint8>& Src, FString& OutError)
        {
            if (Format != EFormat::RGBA16_FLOAT)
            {
                OutError = "Unsupported source format for HDR capture (expected RGBA16_FLOAT).";
                return false;
            }

            const int32 NumPixels = IntCastChecked<int32>(static_cast<int64>(Width) * Height);
            const uint16* Halves = reinterpret_cast<const uint16*>(Src.GetData());
            TVector<float> Pixels(NumPixels * 3);

            for (int32 Pixel = 0; Pixel < NumPixels; ++Pixel)
            {
                Pixels[Pixel * 3 + 0] = Math::Detail::HalfToFloat(Halves[Pixel * 4 + 0]);
                Pixels[Pixel * 3 + 1] = Math::Detail::HalfToFloat(Halves[Pixel * 4 + 1]);
                Pixels[Pixel * 3 + 2] = Math::Detail::HalfToFloat(Halves[Pixel * 4 + 2]);
            }

            if (!ImageWrite::WriteHdrFile(*OutPath, Width, Height, 3, Pixels.GetData()))
            {
                OutError = "HDR encode failed (check the output path is writable).";
                return false;
            }
            return true;
        }
    }

    FString GetScreenshotDirectory()
    {
        if (GEngine != nullptr && GEngine->HasLoadedProject())
        {
            const FStringView ProjectPath = GEngine->GetProjectPath();
            return FString(ProjectPath.GetData(), ProjectPath.Len()) + "/Saved/Screenshots";
        }

        return Paths::GetEngineDirectory() + "/Saved/Screenshots";
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
        FString ResolvedPath = OutputPath.IsEmpty() ? GenerateDefaultPath(Source) : OutputPath;

        // Make sure the parent directory exists when the user passed an explicit path.
        {
            const FString Parent = Paths::Parent(FStringView(*ResolvedPath, ResolvedPath.Len()), true);
            if (!Parent.IsEmpty())
            {
                Paths::CreateDirectories(FStringView(*Parent, Parent.Len()));
            }
        }

        TVector<uint8> Pixels;
        bool bWriteOK = ReadImageBytes(SrcImage, Pixels, Out.ErrorMessage);
        if (bWriteOK && Source == ECaptureSource::SceneHDR)
        {
            bWriteOK = WriteHDR(ResolvedPath, Out.ResolutionX, Out.ResolutionY, SrcImage.Desc.Format, Pixels, Out.ErrorMessage);
        }
        else if (bWriteOK)
        {
            bWriteOK = WritePNG(ResolvedPath, Out.ResolutionX, Out.ResolutionY, SrcImage.Desc.Format, Pixels, Out.ErrorMessage);
        }

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

    FCaptureResult SaveWindowCapture(const TVector<uint8>& BGRA, uint32 Width, uint32 Height, const FString& OutputPath)
    {
        FCaptureResult Out;
        Out.ResolutionX = Width;
        Out.ResolutionY = Height;
        if (Width == 0 || Height == 0 || static_cast<size_t>(BGRA.Num()) < (size_t)Width * Height * 4u)
        {
            Out.ErrorMessage = "The window capture holds no pixels.";
            return Out;
        }

        const FString ResolvedPath = OutputPath.IsEmpty() ? GenerateDefaultPath(ECaptureSource::FinalLDR) : OutputPath;
        const FString Parent = Paths::Parent(FStringView(*ResolvedPath, ResolvedPath.Len()), true);
        if (!Parent.IsEmpty())
        {
            Paths::CreateDirectories(FStringView(*Parent, Parent.Len()));
        }

        // The window composites opaque, but UI passes leave arbitrary alpha behind, which a PNG would show as holes.
        TVector<uint8> Opaque = BGRA;
        for (int32 i = 3; i < Opaque.Num(); i += 4)
        {
            Opaque[i] = 255;
        }

        if (WritePNG(ResolvedPath, Width, Height, EFormat::BGRA8_UNORM, Opaque, Out.ErrorMessage))
        {
            Out.bSuccess   = true;
            Out.OutputPath = ResolvedPath;
            LOG_INFO("Saved editor window capture ({}x{}) to {}", Width, Height, ResolvedPath);
        }
        return Out;
    }

    IRenderScene* FindActiveRenderScene()
    {
        if (GWorldManager == nullptr)
        {
            return nullptr;
        }

        FWorldContext* Fallback = nullptr;
        for (const TUniquePtr<FWorldContext>& Ctx : GWorldManager->GetContexts())
        {
            if (!Ctx || !Ctx->World.IsValid())
            {
                continue;
            }

            if ((Ctx->Type == EWorldType::Game || Ctx->Type == EWorldType::Simulation) && Ctx->World->GetRenderer() != nullptr)
            {
                return Ctx->World->GetRenderer();
            }
            // A hidden tab's world has its renderer reclaimed, so the one still rendering is the one on screen.
            if (Ctx->Type == EWorldType::Editor && Fallback == nullptr && Ctx->World->GetRenderer() != nullptr)
            {
                Fallback = Ctx.get();
            }
        }

        return Fallback != nullptr && Fallback->World.IsValid() ? Fallback->World->GetRenderer() : nullptr;
    }

    bool ReadDisplayPixels(IRenderScene* Scene, TVector<uint8>& OutRGBA, uint32& OutWidth, uint32& OutHeight, FString& OutError)
    {
        const FSceneImage SrcImage = PickSourceImage(Scene, ECaptureSource::FinalLDR);
        if (!SrcImage.IsValid())
        {
            OutError = "No picture is available to read back.";
            return false;
        }

        OutWidth = SrcImage.GetSizeX();
        OutHeight = SrcImage.GetSizeY();
        if (!ReadImageBytes(SrcImage, OutRGBA, OutError))
        {
            return false;
        }
        if (!SwizzleToRGBA8(SrcImage.Desc.Format, OutRGBA))
        {
            OutError = "The display image is not an 8-bit color format.";
            return false;
        }
        return true;
    }

    FCaptureResult CaptureActiveWorld(ECaptureSource Source, const FString& OutputPath)
    {
        FCaptureResult Out;

        if (GWorldManager == nullptr)
        {
            Out.ErrorMessage = "WorldManager is not initialized.";
            return Out;
        }

        IRenderScene* Scene = FindActiveRenderScene();

        if (Scene == nullptr)
        {
            Out.ErrorMessage = "No active world with a render scene was found.";
            return Out;
        }

        return Capture(Scene, Source, OutputPath);
    }
}
