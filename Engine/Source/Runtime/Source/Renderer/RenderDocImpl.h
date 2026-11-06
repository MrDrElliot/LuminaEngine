#pragma once
#include "renderdoc_app.h"

namespace Lumina
{
    class FRenderDoc
    {
    public:

        FRenderDoc();

        RUNTIME_API static FRenderDoc& Get();

        RUNTIME_API static bool IsAttached();

        RUNTIME_API void StartFrameCapture() const;
        RUNTIME_API void EndFrameCapture() const;
        RUNTIME_API void TriggerCapture() const;
        RUNTIME_API const char* GetCaptureFilePath() const;
        RUNTIME_API void TryOpenRenderDocUI();
        
    private:

        RENDERDOC_API_1_1_2* RenderDocAPI = nullptr;
    };
}
