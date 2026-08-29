#include "NsightPerfEditorModule.h"

#include "Core/Module/ModuleManager.h"
#include "Log/Log.h"

// Vulkan lives only in this plugin, and RHINative hands out opaque handles reinterpreted here.
#include <volk/volk.h>
#include "Renderer/RHINative.h"

// Nsight Perf SDK (header-only utility layer over nvperf_grfx_host).
#include "NvPerfInit.h"
#include "NvPerfVulkan.h"
#include "NvPerfPeriodicSamplerVulkan.h"

// Editor tool wiring.
#include "Core/Engine/Engine.h"                  // FEngine::GetDevelopmentToolsUI
#include "LuminaEditor.h"                         // GEditorEngine
#include "UI/EditorUI.h"                          // FEditorUI::ToggleTool / IsToolActive
#include "UI/Tools/ToolsMenuRegistry.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"      // LE_ICON_*
#include "Tools/UI/ImGui/ImGuiModule.h"           // LUMINA_MODULE_IMGUI
#include "NsightPerfTool.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Delegates/CoreDelegates.h"

#include <vector>

using namespace Lumina;

IMPLEMENT_MODULE(FNsightPerfEditorModule, "NsightPerfEditor");
LUMINA_MODULE_IMGUI();

namespace
{
    FEditorUI* GetEditorUI()
    {
        if (GEditorEngine == nullptr)
        {
            return nullptr;
        }
        return static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI());
    }
}

void FNsightPerfEditorModule::StartupModule()
{
    if (nv::perf::InitializeNvPerf())
    {
        constexpr uint32 ApiVersion = VK_API_VERSION_1_4;   // engine creates a Vulkan 1.4 instance/device

        std::vector<const char*> InstanceExts;
        std::vector<const char*> DeviceExts;
        nv::perf::VulkanAppendInstanceRequiredExtensions(InstanceExts, ApiVersion);
        nv::perf::sampler::PeriodicSamplerTimeHistoryVulkan::AppendDeviceRequiredExtensions(ApiVersion, DeviceExts);

        RHI::Native::FDeviceCreationRequest Request;
        for (const char* Name : InstanceExts)
        {
            Request.InstanceExtensions.push_back(Name);
        }
        for (const char* Name : DeviceExts)
        {
            Request.DeviceExtensions.push_back(Name);
        }
        RHI::Native::RegisterDeviceCreationRequest(Request);

        LOG_INFO("[NsightPerf] NvPerf initialized; requested {} instance + {} device extension(s).",
            (int)InstanceExts.size(), (int)DeviceExts.size());
    }
    else
    {
        LOG_WARN("[NsightPerf] NvPerf host failed to initialize (no supported NVIDIA GPU/driver?). "
                 "The HUD tool will open but report itself unavailable.");
    }
    
    FToolsMenuEntry Entry;
    Entry.Label    = LE_ICON_GAUGE " Nsight Perf HUD";
    Entry.IsActive = []() -> bool
    {
        FEditorUI* UI = GetEditorUI();
        return UI != nullptr && UI->IsToolActive<FNsightPerfTool>();
    };
    Entry.OnToggle = []()
    {
        if (FEditorUI* UI = GetEditorUI())
        {
            UI->ToggleTool<FNsightPerfTool>(UI);
        }
    };
    ToolsMenuHandle = FToolsMenuRegistry::Get().Register(Move(Entry));

    // The editor UI does not exist yet, so the tool can only be opened once the engine is up.
    if (GCommandLine != nullptr && GCommandLine->Has("nsightexport"))
    {
        FCoreDelegates::OnPostEngineInit.AddLambda([]
        {
            FEditorUI* UI = GetEditorUI();
            if (UI != nullptr && !UI->IsToolActive<FNsightPerfTool>())
            {
                UI->ToggleTool<FNsightPerfTool>(UI);
                LOG_INFO("[NsightPerf] -nsightexport opened the HUD and armed the CSV dump.");
            }
        });
    }

    LOG_INFO("[NsightPerf] Editor module ready");
}

void FNsightPerfEditorModule::ShutdownModule()
{
    FToolsMenuRegistry::Get().Unregister(ToolsMenuHandle);
    ToolsMenuHandle = 0;

    LOG_INFO("[NsightPerf] Editor module shutdown");
}
