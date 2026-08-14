#include "RuntimePCH.h"
#include "RenderDocImpl.h"

#if defined(LE_PLATFORM_WINDOWS)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

#include "renderdoc_app.h"
#include "Core/Assertions/Assert.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Process/PlatformProcess.h"

namespace Lumina
{
    namespace
    {
    #if defined(LE_PLATFORM_WINDOWS)
        constexpr const TCHAR* kRenderDocLibrary = TEXT("renderdoc.dll");
    #else
        constexpr const TCHAR* kRenderDocLibrary = TEXT("librenderdoc.so");
    #endif

        bool IsLibraryAlreadyLoaded(const TCHAR* Name)
        {
        #if defined(LE_PLATFORM_WINDOWS)
            return ::GetModuleHandleW(Name) != nullptr;
        #else
            if (void* Handle = ::dlopen(Name, RTLD_NOLOAD | RTLD_LAZY))
            {
                ::dlclose(Handle);
                return true;
            }

            return false;
        #endif
        }
    }

    FRenderDoc::FRenderDoc()
    {
        void* Module = Platform::GetDLLHandle(kRenderDocLibrary);

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

    bool FRenderDoc::IsAttached()
    {
        return IsLibraryAlreadyLoaded(kRenderDocLibrary);
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
