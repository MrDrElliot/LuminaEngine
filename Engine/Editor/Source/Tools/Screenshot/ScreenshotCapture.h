#pragma once
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    class IRenderScene;
}

namespace Lumina::Screenshot
{
    enum class ECaptureSource : uint8
    {
        // Final post-tonemap render target produced by IRenderScene::GetRenderTarget().
        // Stored as 8-bit RGBA -- exported as PNG.
        FinalLDR,

        // Pre-tonemap linear HDR scene color (ENamedImage::HDR on FDefaultSceneRenderer).
        // RGBA16_FLOAT -- exported as Radiance .hdr.
        SceneHDR,
    };

    struct FCaptureResult
    {
        bool        bSuccess = false;
        FString     OutputPath;
        FString     ErrorMessage;
        uint32      ResolutionX = 0;
        uint32      ResolutionY = 0;
    };

    // <Project>/Saved/Screenshots, or <EngineDir>/Saved/Screenshots when no project is loaded.
    EDITOR_API FString GetScreenshotDirectory();

    // Blocks on the GPU so the readback reflects the latest frame; empty OutputPath means a timestamped default.
    EDITOR_API FCaptureResult Capture(IRenderScene* Scene, ECaptureSource Source, const FString& OutputPath = {});

    // Picks the best available world's render scene (Game > Editor) and captures it.
    EDITOR_API FCaptureResult CaptureActiveWorld(ECaptureSource Source, const FString& OutputPath = {});

    // The render scene the capture functions pick, a playing world's before the editor's.
    EDITOR_API IRenderScene* FindActiveRenderScene();

    // Reads Scene's final picture back as tightly packed RGBA8, top row first, waiting on the GPU to do it.
    EDITOR_API bool ReadDisplayPixels(IRenderScene* Scene, TVector<uint8>& OutRGBA, uint32& OutWidth, uint32& OutHeight, FString& OutError);
}
