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

        /** True when RenderDoc is already injected into this process.
         *
         *  A module LOOKUP, deliberately not a load: the constructor reaches RenderDoc through
         *  Platform::GetDLLHandle, which is LoadLibrary underneath and would PULL RenderDoc IN rather
         *  than detect it. Static, and touches no state, so it is safe to ask before the singleton (or
         *  the RHI device) exists -- which is the point, since device creation needs the answer. */
        RUNTIME_API static bool IsAttached();

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
