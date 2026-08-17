#pragma once
#include "Containers/String.h"
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
}
