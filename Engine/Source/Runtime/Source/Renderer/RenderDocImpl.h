#pragma once
#include "renderdoc_app.h"
#include "Containers/String.h"

namespace Lumina
{
    class FRenderDoc
    {
    public:

        FRenderDoc();

        RUNTIME_API static FRenderDoc& Get();

        RUNTIME_API void StartFrameCapture() const;
        RUNTIME_API void EndFrameCapture() const;
        RUNTIME_API void TriggerCapture() const;
        RUNTIME_API const char* GetCaptureFilePath() const;
        RUNTIME_API void TryOpenRenderDocUI();
        
    private:

        FString RenderDocExePath;
        RENDERDOC_API_1_1_2* RenderDocAPI = nullptr;
    };
}
