#include "RenderDocEditorModule.h"

#include "Core/CommandLine/CommandLine.h"
#include "Core/Module/ModuleManager.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Renderer/RHINative.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/Tools/ToolsMenuRegistry.h"

#if defined(LE_PLATFORM_WINDOWS)
    #include <windows.h>
    #include <cwchar>
#else
    #include <dlfcn.h>
#endif

using namespace Lumina;

IMPLEMENT_MODULE(FRenderDocEditorModule, "RenderDocEditor");

namespace
{
#if defined(LE_PLATFORM_WINDOWS)
    constexpr const wchar_t* kLibraryName   = L"renderdoc.dll";
    constexpr const wchar_t* kLayerManifest = L"renderdoc.json";

    void* FindLoadedRenderDoc()
    {
        return ::GetModuleHandleW(kLibraryName);
    }

    // The Vulkan loader loads the DLL beside the registered layer manifest, and any other copy would hook nothing.
    FWString FindRegisteredLayerLibrary()
    {
        for (HKEY Root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        {
            HKEY Key = nullptr;
            if (::RegOpenKeyExW(Root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ, &Key) != ERROR_SUCCESS)
            {
                continue;
            }

            FWString Found;
            wchar_t  Manifest[1024];
            for (DWORD Index = 0; Found.IsEmpty(); ++Index)
            {
                DWORD ManifestLength = (DWORD)(sizeof(Manifest) / sizeof(Manifest[0]));
                DWORD Type           = 0;
                DWORD Disabled       = 1;
                DWORD DataSize       = sizeof(Disabled);
                if (::RegEnumValueW(Key, Index, Manifest, &ManifestLength, nullptr, &Type, (LPBYTE)&Disabled, &DataSize) != ERROR_SUCCESS)
                {
                    break;
                }

                const wchar_t* Separator = std::wcsrchr(Manifest, L'\\');
                const wchar_t* FileName  = Separator != nullptr ? Separator + 1 : Manifest;
                if (Type == REG_DWORD && Disabled == 0 && ::_wcsicmp(FileName, kLayerManifest) == 0)
                {
                    Found = FWString(Manifest, (int32)(FileName - Manifest));
                    Found += kLibraryName;
                }
            }
            ::RegCloseKey(Key);

            if (!Found.IsEmpty())
            {
                return Found;
            }
        }
        return {};
    }

    void* LoadRenderDoc()
    {
        FWString Library = FindRegisteredLayerLibrary();
        if (Library.IsEmpty())
        {
            LOG_WARN("[RenderDoc] No registered RenderDoc Vulkan layer, so captures need RenderDoc installed. Trying External/RenderDoc.");
            Library = StringUtils::ToWideString(Paths::GetEngineInstallDirectory() + "/External/RenderDoc/renderdoc.dll");
        }

        void* Module = ::LoadLibraryW(*Library);
        LOG_INFO("[RenderDoc] Loading {}: {}", StringUtils::FromWideString(Library), Module != nullptr ? "ok" : "failed");
        return Module;
    }

    void* GetRenderDocProc(void* Module)
    {
        return (void*)::GetProcAddress((HMODULE)Module, "RENDERDOC_GetAPI");
    }
#else
    constexpr const char* kLibraryName = "librenderdoc.so";

    void* FindLoadedRenderDoc()
    {
        return ::dlopen(kLibraryName, RTLD_NOW | RTLD_NOLOAD);
    }

    void* LoadRenderDoc()
    {
        return ::dlopen(kLibraryName, RTLD_NOW);
    }

    void* GetRenderDocProc(void* Module)
    {
        return ::dlsym(Module, "RENDERDOC_GetAPI");
    }
#endif
}

void FRenderDocEditorModule::StartupModule()
{
    void* Module = FindLoadedRenderDoc();
    const bool bInjected = Module != nullptr;
    if (!bInjected && Global::GCommandLine != nullptr && Global::GCommandLine->Has("renderdoc"))
    {
        Module = LoadRenderDoc();
    }

    if (Module != nullptr)
    {
        auto GetAPI = (pRENDERDOC_GetAPI)GetRenderDocProc(Module);
        if (GetAPI == nullptr || GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&RenderDocAPI) != 1)
        {
            RenderDocAPI = nullptr;
            LOG_WARN("[RenderDoc] The loaded RenderDoc does not offer API 1.6, so captures are off.");
        }
    }

    if (RenderDocAPI != nullptr)
    {
        // Captures of buffer device addresses only replay when the device was created able to replay them.
        RHI::Native::FDeviceCreationRequest Request;
        Request.bBufferDeviceAddressCaptureReplay = true;
        RHI::Native::RegisterDeviceCreationRequest(Request);

        RenderDocAPI->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
        LOG_INFO("[RenderDoc] Ready ({}). Capture from Tools > RenderDoc Capture.", bInjected ? "launched from RenderDoc" : "loaded by -renderdoc");
    }

    FToolsMenuEntry Entry;
    Entry.Label    = LE_ICON_CAMERA " RenderDoc Capture";
    Entry.OnToggle = [this]()
    {
        CaptureNextFrame();
    };
    ToolsMenuHandle = FToolsMenuRegistry::Get().Register(Move(Entry));
}

void FRenderDocEditorModule::ShutdownModule()
{
    FToolsMenuRegistry::Get().Unregister(ToolsMenuHandle);
    ToolsMenuHandle = 0;
    RenderDocAPI    = nullptr;
}

void FRenderDocEditorModule::CaptureNextFrame() const
{
    if (RenderDocAPI == nullptr)
    {
        LOG_WARN("[RenderDoc] Not loaded. Launch the editor with -renderdoc, or from RenderDoc, to capture frames.");
        return;
    }

    RenderDocAPI->TriggerCapture();

    // A connected replay UI lists the capture once it lands, so it only has to be brought forward.
    if (RenderDocAPI->IsTargetControlConnected())
    {
        RenderDocAPI->ShowReplayUI();
    }
    else
    {
        RenderDocAPI->LaunchReplayUI(1, nullptr);
    }
    LOG_INFO("[RenderDoc] Capturing the next frame into {}.", RenderDocAPI->GetCaptureFilePathTemplate());
}
