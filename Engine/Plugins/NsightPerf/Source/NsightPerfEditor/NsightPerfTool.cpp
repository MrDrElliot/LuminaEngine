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

        // View state for the detail sections below the HUD.
        bool  bShowPlots      = true;
        bool  bShowMetrics    = false;
        char  MetricFilter[128] = {};

        // Sampling parameters, kept so the session panel can report what the numbers actually mean
        // rather than restating the constants from OnInitialize.
        double SamplingIntervalSeconds = 0.0;
        double PlotTimeWidthSeconds    = 0.0;

        nv::perf::sampler::PeriodicSamplerTimeHistoryVulkan Sampler;
        nv::perf::hud::HudPresets                           HudPresets;
        nv::perf::hud::HudDataModel                         HudDataModel;
        nv::perf::hud::HudImPlotRenderer                    HudRenderer;
    };

    namespace
    {
        // Current value plus min/avg/max across the retained window.
        //
        // Reads valBuffer EXACTLY as HudImPlotRenderer does: MetricSignal::AddSample already multiplied by
        // `multiplier` before pushing (NvPerfHudDataModel.h), so the buffer is in display units and must
        // not be scaled again here -- doing so would silently disagree with the plot beside it.
        struct FSignalStats
        {
            bool   bValid = false;
            double Current = 0.0;
            double Min = 0.0;
            double Max = 0.0;
            double Avg = 0.0;
            size_t Samples = 0;
        };

        FSignalStats ReadSignal(const nv::perf::hud::MetricSignal& Signal)
        {
            FSignalStats Out;

            const auto& Buffer = Signal.valBuffer;
            const size_t Count = Buffer.Size();
            if (Count == 0)
            {
                return Out;
            }

            Out.bValid  = true;
            Out.Samples = Count;
            Out.Current = Buffer.Front();   // Front() is the MOST RECENT write, not the oldest

            double Sum = 0.0;
            Out.Min = Buffer.Get(0);
            Out.Max = Buffer.Get(0);
            for (size_t i = 0; i < Count; ++i)
            {
                const double V = Buffer.Get(i);
                Sum += V;
                Out.Min = V < Out.Min ? V : Out.Min;
                Out.Max = V > Out.Max ? V : Out.Max;
            }
            Out.Avg = Sum / (double)Count;
            return Out;
        }

        bool PassesFilter(const std::string& Label, const std::string& Metric, const char* Filter)
        {
            if (Filter == nullptr || Filter[0] == '\0')
            {
                return true;
            }
            std::string Needle(Filter);
            auto Lower = [](std::string S)
            {
                for (char& C : S) { C = (char)((C >= 'A' && C <= 'Z') ? (C - 'A' + 'a') : C); }
                return S;
            };
            Needle = Lower(Needle);
            return Lower(Label).find(Needle) != std::string::npos
                || Lower(Metric).find(Needle) != std::string::npos;
        }
    }

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
        State->SamplingIntervalSeconds = 1.0 / (double)SamplingFrequency;
        State->PlotTimeWidthSeconds    = PlotTimeWidthSeconds;

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
        }

        ImGui::Checkbox("Plots", &State->bShowPlots);
        ImGui::SameLine();
        ImGui::Checkbox("Metric Values", &State->bShowMetrics);
        ImGui::Separator();

        DrawSessionInfo();

        if (State->bShowPlots)
        {
            State->HudRenderer.Render();
        }

        if (State->bShowMetrics)
        {
            DrawMetricTable();
        }
    }

    void FNsightPerfTool::DrawSessionInfo()
    {
        if (!ImGui::CollapsingHeader("Session"))
        {
            return;
        }

        // Counted rather than stored: the preset decides how many of each, and a hardcoded number here
        // would quietly go stale the moment the preset changes.
        size_t PanelCount  = 0;
        size_t SignalCount = 0;
        for (const auto& Config : State->HudDataModel.GetConfigurations())
        {
            PanelCount += Config.panels.size();
            for (const auto& Panel : Config.panels)
            {
                for (const auto& pWidget : Panel.widgets)
                {
                    if (pWidget->type == nv::perf::hud::Widget::Type::ScalarText)
                    {
                        ++SignalCount;
                    }
                    else if (pWidget->type == nv::perf::hud::Widget::Type::TimePlot)
                    {
                        const auto& Plot = static_cast<const nv::perf::hud::TimePlot&>(*pWidget);
                        SignalCount += Plot.signals.size() + Plot.stackedSignals.size();
                    }
                }
            }
        }

        const double IntervalMs = State->SamplingIntervalSeconds * 1000.0;

        ImGui::Indent(12.0f);
        ImGui::TextColored(EditorColors::TextDim(), "Sample interval");
        ImGui::SameLine(200.0f);
        ImGui::Text("%.2f ms (%.0f Hz)", IntervalMs,
            State->SamplingIntervalSeconds > 0.0 ? 1.0 / State->SamplingIntervalSeconds : 0.0);

        ImGui::TextColored(EditorColors::TextDim(), "Plot window");
        ImGui::SameLine(200.0f);
        ImGui::Text("%.1f s", State->PlotTimeWidthSeconds);

        ImGui::TextColored(EditorColors::TextDim(), "Panels / metrics");
        ImGui::SameLine(200.0f);
        ImGui::Text("%zu / %zu", PanelCount, SignalCount);

        // The periodic sampler is asynchronous: these are GPU-side counter samples, NOT engine frames, so
        // a rate far below the interval above means samples are being dropped rather than the GPU idling.
        ImGui::TextColored(EditorColors::TextDim(), "Status");
        ImGui::SameLine(200.0f);
        ImGui::TextUnformatted(State->StatusMessage.c_str());
        ImGui::Unindent(12.0f);
        ImGui::Spacing();
    }

    void FNsightPerfTool::DrawMetricTable()
    {
        ImGui::Spacing();
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##metric_filter", "Filter metrics...",
            State->MetricFilter, sizeof(State->MetricFilter));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
        {
            State->MetricFilter[0] = '\0';
        }
        ImGui::Spacing();

        constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                             | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

        // Emitted per signal. Split out so the ScalarText and TimePlot walks below cannot drift in how
        // they format or filter -- they differ only in where the signal came from.
        auto DrawSignalRow = [this](const nv::perf::hud::MetricSignal& Signal)
        {
            if (!PassesFilter(Signal.label.text, Signal.metric, State->MetricFilter))
            {
                return;
            }

            const FSignalStats Stats = ReadSignal(Signal);

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Signal.label.text.empty() ? Signal.metric.c_str() : Signal.label.text.c_str());
            if (ImGui::IsItemHovered() && !Signal.description.empty())
            {
                ImGui::SetTooltip("%s\n\n%s", Signal.metric.c_str(), Signal.description.c_str());
            }
            else if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", Signal.metric.c_str());
            }

            ImGui::TableNextColumn();
            if (Stats.bValid)
            {
                ImGui::Text("%.2f", Stats.Current);
            }
            else
            {
                // No samples yet is a normal state for the first frames after the session opens.
                ImGui::TextColored(EditorColors::TextDim(), "--");
            }

            ImGui::TableNextColumn();
            ImGui::TextColored(EditorColors::TextDim(), "%s",
                Signal.unit.empty() ? "" : Signal.unit.c_str());

            ImGui::TableNextColumn();
            if (Stats.bValid)
            {
                ImGui::TextColored(EditorColors::TextDim(), "%.2f / %.2f / %.2f", Stats.Min, Stats.Avg, Stats.Max);
            }
            else
            {
                ImGui::TextColored(EditorColors::TextDim(), "--");
            }
        };

        int ScopeId = 0;
        for (const auto& Config : State->HudDataModel.GetConfigurations())
        {
            for (const auto& Panel : Config.panels)
            {
                // A CollapsingHeader derives its ID from its LABEL, and nothing stops two configurations
                // from carrying a panel of the same name -- they would then open and close together, and
                // their tables would share scroll state. Scoping by position makes each one its own.
                ImGui::PushID(ScopeId++);

                const std::string Header = Panel.label.text.empty() ? Panel.name : Panel.label.text;
                if (!ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::PopID();
                    continue;
                }

                if (!ImGui::BeginTable("##metrics", 4, TableFlags))
                {
                    ImGui::PopID();
                    continue;
                }

                ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 0.50f);
                ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch, 0.14f);
                ImGui::TableSetupColumn("Unit",   ImGuiTableColumnFlags_WidthStretch, 0.10f);
                ImGui::TableSetupColumn("Min / Avg / Max", ImGuiTableColumnFlags_WidthStretch, 0.26f);
                ImGui::TableHeadersRow();

                for (const auto& pWidget : Panel.widgets)
                {
                    if (pWidget->type == nv::perf::hud::Widget::Type::ScalarText)
                    {
                        DrawSignalRow(static_cast<const nv::perf::hud::ScalarText&>(*pWidget).signal);
                    }
                    else if (pWidget->type == nv::perf::hud::Widget::Type::TimePlot)
                    {
                        const auto& Plot = static_cast<const nv::perf::hud::TimePlot&>(*pWidget);
                        for (const auto& Signal : Plot.signals)
                        {
                            DrawSignalRow(Signal);
                        }
                        // Stacked signals are a separate list on the same plot, not a subset of the above.
                        for (const auto& Signal : Plot.stackedSignals)
                        {
                            DrawSignalRow(Signal);
                        }
                    }
                }

                ImGui::EndTable();
                ImGui::PopID();
            }
        }
    }
}
