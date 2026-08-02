#include "RuntimePCH.h"
#include "RenderDocImpl.h"

#include <windows.h>

#include "renderdoc_app.h"
#include "Core/Assertions/Assert.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Process/PlatformProcess.h"

namespace Lumina
{
    FRenderDoc::FRenderDoc()
    {
        void* Module = Platform::GetDLLHandle(TEXT("renderdoc.dll"));

        if (Module)
        {
            auto RENDERDOC_GetAPI = Platform::LumGetProcAddress<pRENDERDOC_GetAPI>(Module, "RENDERDOC_GetAPI");
            int Ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void**)&RenderDocAPI);
            ASSERT(Ret);
        }

        // @TODO - Post settings, let user manually enter render doc UI path.
        if (FileHelper::DoesDirectoryExist("C:Program Files/RenderDoc/renderdocui.exe"))
        {
            RenderDocExePath = "C:Program Files/RenderDoc/renderdocui.exe";
        }
    }

    FRenderDoc& FRenderDoc::Get()
    {
        static FRenderDoc GSingleton;
        return GSingleton;
    }

    void FRenderDoc::StartFrameCapture() const
    {
        if (!RenderDocAPI)
        {
            return;
        }
        
        RenderDocAPI->StartFrameCapture(nullptr, nullptr);
    }

    void FRenderDoc::EndFrameCapture() const
    {
        if (!RenderDocAPI)
        {
            return;
        }
        
        RenderDocAPI->EndFrameCapture(nullptr, nullptr);
    }

    void FRenderDoc::TriggerCapture() const
    {
        if (!RenderDocAPI)
        {
            return;
        }
        
        RenderDocAPI->TriggerCapture();
    }

    const char* FRenderDoc::GetCaptureFilePath() const
    {
        if (!RenderDocAPI)
        {
            return nullptr;
        }
        
        return RenderDocAPI->GetCaptureFilePathTemplate();
    }

    void FRenderDoc::TryOpenRenderDocUI()
    {
        if (!RenderDocAPI)
        {
            return;
        }
    }
}
