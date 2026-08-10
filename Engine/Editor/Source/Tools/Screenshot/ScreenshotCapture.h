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

    // Captures Scene's render target to disk; blocks on the GPU so the readback reflects the latest frame.
    // OutputPath needs the extension (.png for FinalLDR, .hdr for SceneHDR); empty = timestamped path under <EngineDir>/Saved/Screenshots.
    EDITOR_API FCaptureResult Capture(IRenderScene* Scene, ECaptureSource Source, const FString& OutputPath = {});

    // Picks the best available world's render scene (Game > Editor) and captures it.
    EDITOR_API FCaptureResult CaptureActiveWorld(ECaptureSource Source, const FString& OutputPath = {});
}
