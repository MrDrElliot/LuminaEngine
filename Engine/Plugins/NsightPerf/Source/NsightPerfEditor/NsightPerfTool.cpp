#include "NsightPerfTool.h"

// volk defines VK_NO_PROTOTYPES + the Vulkan types; it MUST precede every NvPerf Vulkan header.
// Vulkan lives only in this plugin: RHINative.h hands out opaque handles we reinterpret below.
#include <volk/volk.h>
#include "Renderer/RHINative.h"

// Nsight Perf SDK. NvPerfMetricConfigurationsHAL pulls the per-architecture metric/HUD configs the
// data model needs; the ImPlot renderer draws into our (engine-owned) ImPlot + ImGui context.
#include <NvPerfVulkan.h>
#include <NvPerfPeriodicSamplerVulkan.h>
#include <NvPerfMetricConfigurationsHAL.h>
#include <NvPerfHudDataModel.h>
#include <NvPerfHudImPlotRenderer.h>

#include "imgui.h"
#include "Tools/UI/ImGui/EditorColors.h"

#include <string>

namespace Lumina
{
    // Holds all NvPerf state. One-shot: built in OnInitialize, torn down in OnDeinitialize.
    struct FNsightPerfState
    {
        bool        bSamplerInitialized = false;   // Sampler.Initialize succeeded
        bool        bSessionActive      = false;   // BeginSession + HUD ready
        std::string StatusMessage       = "Initializing...";
        std::string ChipName;

        nv::perf::sampler::PeriodicSamplerTimeHistoryVulkan Sampler;
        nv::perf::hud::HudPresets                           HudPresets;
        nv::perf::hud::HudDataModel                         HudDataModel;
        nv::perf::hud::HudImPlotRenderer                    HudRenderer;
    };

    void FNsightPerfTool::OnInitialize()
    {
        State = new FNsightPerfState();
        CreateToolWindow("Nsight Perf", [this](bool bFocused) { DrawWindow(bFocused); });

        const RHI::Native::FNativeDeviceHandles H = RHI::Native::GetNativeDeviceHandles();
        if (H.Backend != RHI::EBackend::Vulkan || H.Device == nullptr)
        {
            State->StatusMessage = "RHI Vulkan device is not available.";
            return;
        }

        // Reinterpret the engine's opaque native handles back to Vulkan types (this plugin is the
        // Vulkan-coupled consumer). volk builds with VK_NO_PROTOTYPES, so pass the proc-addr getters.
        const VkInstance       Instance       = static_cast<VkInstance>(H.Instance);
        const VkPhysicalDevice PhysicalDevice = static_cast<VkPhysicalDevice>(H.PhysicalDevice);
        const VkDevice         Device         = static_cast<VkDevice>(H.Device);
        const VkQueue          GraphicsQueue  = static_cast<VkQueue>(H.GraphicsQueue);
        const auto GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(H.GetInstanceProcAddr);
        const auto GetDeviceProcAddr   = reinterpret_cast<PFN_vkGetDeviceProcAddr>(H.GetDeviceProcAddr);

        // NvPerf init (Initialize/BeginSession) submits to the shared graphics queue. The engine
        // submits frames from engine threads; hold the RHI submit lock for the whole setup so the
        // two never race. One-time hitch on tool open is fine. RAII releases on every return path.
        const RHI::Native::FScopedSubmitLock SubmitLock;

        if (!State->Sampler.Initialize(Instance, PhysicalDevice, Device, GetInstanceProcAddr, GetDeviceProcAddr))
        {
            State->StatusMessage = "Sampler initialization failed. The GPU may be unsupported, or the "
                                   "NvPerf device extensions weren't enabled at device creation.";
            return;
        }
        State->bSamplerInitialized = true;

        const nv::perf::DeviceIdentifiers Ids = State->Sampler.GetGpuDeviceIdentifiers();
        State->ChipName = (Ids.pChipName != nullptr) ? Ids.pChipName : "";

        constexpr auto SamplingFrequency   = 60;
        constexpr auto SamplingIntervalNs  = 1000u * 1000u * 1000u / SamplingFrequency;
        constexpr auto MaxDecodeLatencyNs  = 1000u * 1000u * 1000u;
        constexpr auto MaxFrameLatency     = 32;

        if (!State->Sampler.BeginSession(GraphicsQueue, H.GraphicsQueueFamily, SamplingIntervalNs, MaxDecodeLatencyNs, MaxFrameLatency))
        {
            State->StatusMessage = "BeginSession failed (counter access may be restricted; ensure "
                                   "developer-mode GPU profiling is permitted).";
            return;
        }

        // Build the HUD data model from the "Graphics General Triage" preset and its metric config.
        State->HudPresets.Initialize(Ids.pChipName);
        const double PlotTimeWidthSeconds = 4.0;
        State->HudDataModel.Load(State->HudPresets.GetPreset("Graphics General Triage"));

        std::string MetricConfigName;
        nv::perf::MetricConfigObject MetricConfigObject;
        if (nv::perf::MetricConfigurations::GetMetricConfigNameBasedOnHudConfigurationName(MetricConfigName, Ids.pChipName, "Graphics General Triage"))
        {
            nv::perf::MetricConfigurations::LoadMetricConfigObject(MetricConfigObject, Ids.pChipName, MetricConfigName);
        }
        State->HudDataModel.Initialize(1.0 / (double)SamplingFrequency, PlotTimeWidthSeconds, MetricConfigObject);
        State->Sampler.SetConfig(&State->HudDataModel.GetCounterConfiguration());
        State->HudDataModel.PrepareSampleProcessing(State->Sampler.GetCounterData());
        
        ImGuiStyle& Style = ImGui::GetStyle();
        const ImVec4 SavedWindowBg    = Style.Colors[ImGuiCol_WindowBg];
        const ImVec4 SavedScrollbarBg = Style.Colors[ImGuiCol_ScrollbarBg];
        const ImVec4 SavedPopupBg     = Style.Colors[ImGuiCol_PopupBg];
        const ImVec4 SavedBorder      = Style.Colors[ImGuiCol_Border];
        const ImVec4 SavedFrameBg     = Style.Colors[ImGuiCol_FrameBg];

        nv::perf::hud::HudImPlotRenderer::SetStyle();

        Style.Colors[ImGuiCol_WindowBg]    = SavedWindowBg;
        Style.Colors[ImGuiCol_ScrollbarBg] = SavedScrollbarBg;
        Style.Colors[ImGuiCol_PopupBg]     = SavedPopupBg;
        Style.Colors[ImGuiCol_Border]      = SavedBorder;
        Style.Colors[ImGuiCol_FrameBg]     = SavedFrameBg;

        State->HudRenderer.Initialize(State->HudDataModel);

        State->bSessionActive = true;
        State->StatusMessage  = "Sampling.";
    }

    void FNsightPerfTool::OnDeinitialize(const FUpdateContext& /*UpdateContext*/)
    {
        if (State == nullptr)
        {
            return;
        }
        {
            // EndSession/Reset also touch the shared queue; serialize with engine submission.
            const RHI::Native::FScopedSubmitLock SubmitLock;
            if (State->bSessionActive)
            {
                State->Sampler.EndSession();
            }
            if (State->bSamplerInitialized)
            {
                State->Sampler.Reset();
            }
        }
        delete State;
        State = nullptr;
    }

    void FNsightPerfTool::DrawWindow(bool /*bIsFocused*/)
    {
        if (State == nullptr)
        {
            return;
        }

        if (!State->bSessionActive)
        {
            ImGui::TextColored(EditorColors::Warning(), "Nsight Perf HUD unavailable");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", State->StatusMessage.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextWrapped("This tool needs an NVIDIA GPU and the Vulkan device created with NvPerf's "
                               "required extensions (registered by this plugin at the Core loading phase). "
                               "Disable the plugin per-project in your .lproject if you don't want it.");
            return;
        }
        {
            const RHI::Native::FScopedSubmitLock SubmitLock;

            // Drain GPU counter samples decoded since last frame into the HUD model.
            State->Sampler.DecodeCounters();
            State->Sampler.ConsumeSamples([this](const uint8_t* pCounterDataImage, size_t counterDataImageSize, uint32_t rangeIndex, bool& stop) -> bool
            {
                stop = false;
                return State->HudDataModel.AddSample(pCounterDataImage, counterDataImageSize, rangeIndex);
            });
            for (const auto& Delimiter : State->Sampler.GetFrameDelimiters())
            {
                State->HudDataModel.AddFrameDelimiter(Delimiter.frameEndTime);
            }

            // Marks the frame boundary (submits a timestamp). Inside the lock for the same reason.
            State->Sampler.OnFrameEnd();
        }

        if (!State->ChipName.empty())
        {
            ImGui::TextColored(EditorColors::TextDim(), "GPU: %s", State->ChipName.c_str());
            ImGui::Separator();
        }

        State->HudRenderer.Render();
    }
}
