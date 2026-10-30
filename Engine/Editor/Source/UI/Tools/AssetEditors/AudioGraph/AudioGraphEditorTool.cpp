#include "EditorPCH.h"
#include "AudioGraphEditorTool.h"

#include "imgui.h"
#include "Assets/AssetTypes/Audio/AudioGraph.h"
#include "Audio/AudioGlobals.h"
#include "Audio/Graph/AudioGraphInstance.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Audio/AudioGraphCompiler.h"
#include "UI/Tools/NodeGraph/Audio/AudioNodeGraph.h"

namespace Lumina
{
    static const char* AudioGraphWindowName      = "Audio Graph";
    static const char* AudioPropertiesWindowName = "Properties";
    static const char* AudioTransportWindowName  = "Preview";

    static const char* GAudioNodeGraphObjectName = "AudioNodeGraph";

    FAudioGraphEditorTool::FAudioGraphEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FAudioGraphEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(AudioGraphWindowName,      [&](bool) { DrawGraphWindow(); });
        CreateToolWindow(AudioPropertiesWindowName, [&](bool) { DrawPropertiesWindow(); });
        CreateToolWindow(AudioTransportWindowName,  [&](bool) { DrawTransportWindow(); });

        NodeGraph = Cast<CAudioNodeGraph>(Asset->GetPackage()->LoadObjectByName(FName(GAudioNodeGraphObjectName)));
        if (NodeGraph == nullptr)
        {
            NodeGraph = NewObject<CAudioNodeGraph>(Asset->GetPackage(), FName(GAudioNodeGraphObjectName));
        }

        NodeGraph->SetAudioGraph(GetAsset<CAudioGraph>());
        NodeGraph->Initialize();
        NodeGraph->SetTransactionManager(&GetTransactionManager());

        NodeGraph->SetNodeSelectedCallback([this](CEdGraphNode* Node)
        {
            if (Node == SelectedNode)
            {
                return;
            }

            SelectedNode = Node;

            if (SelectedNode == nullptr)
            {
                GetPropertyTable()->SetObject(Asset.Get(), Asset->GetClass());
            }
            else
            {
                GetPropertyTable()->SetObject(Node, Node->GetClass());
            }
        });

        NodeGraph->SetPreNodeDeletedCallback([this](const CEdGraphNode* Node)
        {
            if (Node == SelectedNode)
            {
                SelectedNode = nullptr;
                GetPropertyTable()->SetObject(nullptr, nullptr);
            }
        });
    }

    void FAudioGraphEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        StopPreview();

        if (NodeGraph)
        {
            NodeGraph->Shutdown();
            NodeGraph = nullptr;
        }
    }

    void FAudioGraphEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (NodeGraph == nullptr)
        {
            return;
        }

        // A live preview keeps rendering the program it was built from, so an edit has to rebuild it.
        if (NodeGraph->NeedsCompile())
        {
            const bool bWasPreviewing = PreviewHandle.IsValid();

            Compile(false);

            if (bWasPreviewing)
            {
                StartPreview();
            }
        }

        if (PreviewHandle.IsValid() && Audio::HasDevice()
            && Audio::Context().GetVoiceState(PreviewHandle) == EAudioVoiceState::Free)
        {
            PreviewHandle = SAudioHandle::Invalid();
            PreviewInstance.reset();
        }
    }

    void FAudioGraphEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::MenuItem(LE_ICON_RECEIPT_TEXT " Compile"))
        {
            Compile();
        }
    }

    void FAudioGraphEditorTool::DrawGraphWindow()
    {
        if (NodeGraph)
        {
            NodeGraph->DrawGraph();
        }
    }

    void FAudioGraphEditorTool::DrawPropertiesWindow()
    {
        GetPropertyTable()->DrawTree();
    }

    void FAudioGraphEditorTool::DrawTransportWindow()
    {
        CAudioGraph* Graph = GetAsset<CAudioGraph>();
        if (Graph == nullptr)
        {
            return;
        }

        const bool bPlaying = PreviewHandle.IsValid();

        if (ImGui::Button(bPlaying ? LE_ICON_STOP " Stop" : LE_ICON_PLAY " Play", ImVec2(90.0f, 0.0f)))
        {
            bPlaying ? StopPreview() : StartPreview();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("%u nodes, %u inputs, %u outputs",
            (uint32)Graph->GetProgram().Nodes.size(),
            (uint32)Graph->GetInputs().size(),
            (uint32)Graph->GetOutputs().size());

        ImGui::Separator();

        if (bHasErrors)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        }

        for (const FString& Message : CompileMessages)
        {
            ImGui::TextWrapped("%s", Message.c_str());
        }

        if (bHasErrors)
        {
            ImGui::PopStyleColor();
        }

        if (!PreviewInstance)
        {
            return;
        }

        DrawLevelMeters();

        ImGui::Separator();
        ImGui::TextUnformatted("Parameters");

        for (FPreviewParameter& Parameter : PreviewParameters)
        {
            ImGui::PushID(&Parameter);

            const FString Label = FString(Parameter.Name.c_str());

            switch (Parameter.Type)
            {
            case EAudioGraphType::Trigger:
                if (ImGui::Button(Label.c_str(), ImVec2(140.0f, 0.0f)))
                {
                    PreviewInstance->TriggerParameter(Parameter.Name);
                }
                break;

            case EAudioGraphType::Float:
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::DragFloat(Label.c_str(), &Parameter.FloatValue, 0.01f))
                {
                    PreviewInstance->SetFloatParameter(Parameter.Name, Parameter.FloatValue);
                }
                break;

            case EAudioGraphType::Int32:
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::DragInt(Label.c_str(), &Parameter.IntValue))
                {
                    PreviewInstance->SetIntParameter(Parameter.Name, Parameter.IntValue);
                }
                break;

            case EAudioGraphType::Bool:
                if (ImGui::Checkbox(Label.c_str(), &Parameter.BoolValue))
                {
                    PreviewInstance->SetBoolParameter(Parameter.Name, Parameter.BoolValue);
                }
                break;

            default:
                ImGui::TextDisabled("%s", Label.c_str());
                break;
            }

            ImGui::PopID();
        }

        if (!PreviewInstance->GetOutputs().empty())
        {
            ImGui::Separator();
            ImGui::TextUnformatted("Outputs");

            for (const FAudioGraphParameterDecl& Output : PreviewInstance->GetOutputs())
            {
                ImGui::Text("%s  %.3f", Output.Name.c_str(), PreviewInstance->GetFloatOutput(Output.Name));
            }
        }
    }

    void FAudioGraphEditorTool::DrawLevelMeters()
    {
        // Peaks fall back slowly, so a short transient stays readable for a moment.
        constexpr float kMeterFallPerFrame = 0.92f;
        constexpr float kDecibelsPerNeper  = 8.6858896f;
        constexpr const char* ChannelLabels[] = { "L", "R" };

        ImGui::Separator();
        for (uint32 Channel = 0; Channel < 2; ++Channel)
        {
            MeterLevels[Channel] = Math::Max(PreviewInstance->GetOutputPeak(Channel), MeterLevels[Channel] * kMeterFallPerFrame);

            const float Decibels = MeterLevels[Channel] > 1.0e-5f ? kDecibelsPerNeper * Math::Log(MeterLevels[Channel]) : -100.0f;
            const auto Overlay = Format("{}  {:.1f} dB", ChannelLabels[Channel], Decibels);

            const bool bClipping = MeterLevels[Channel] >= 1.0f;
            if (bClipping)
            {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.25f, 0.2f, 1.0f));
            }
            ImGui::ProgressBar(Math::Min(MeterLevels[Channel], 1.0f), ImVec2(-1.0f, 0.0f), Overlay.c_str());
            if (bClipping)
            {
                ImGui::PopStyleColor();
            }
        }
    }

    bool FAudioGraphEditorTool::Compile(bool bMarkPackageDirty)
    {
        CAudioGraph* Graph = GetAsset<CAudioGraph>();
        if (Graph == nullptr || NodeGraph == nullptr)
        {
            return false;
        }

        FAudioGraphCompileResult Result = FAudioGraphCompiler::Compile(NodeGraph.Get());

        CompileMessages.clear();
        for (const FString& Error : Result.Errors)
        {
            CompileMessages.push_back(Error);
        }
        for (const FString& Warning : Result.Warnings)
        {
            CompileMessages.push_back(Warning);
        }

        bHasErrors = !Result.bSuccess;
        NodeGraph->MarkCompiled();

        if (!Result.bSuccess)
        {
            return false;
        }

        Graph->SetProgram(Move(Result.Program), Move(Result.Waves));

        if (bMarkPackageDirty)
        {
            NotifyAssetDataChanged();
        }

        return true;
    }

    void FAudioGraphEditorTool::StartPreview()
    {
        const TVector<FPreviewParameter> Previous = PreviewParameters;
        StopPreview();

        CAudioGraph* Graph = GetAsset<CAudioGraph>();
        if (Graph == nullptr || !Graph->IsCompiled() || !Audio::HasDevice())
        {
            return;
        }

        const FAudioDeviceInfo DeviceInfo = Audio::Context().GetDeviceInfo();
        const uint32 SampleRate = DeviceInfo.SampleRate != 0 ? DeviceInfo.SampleRate : 48000;

        PreviewInstance = Graph->CreateInstance(SampleRate, 2);
        if (!PreviewInstance)
        {
            return;
        }

        SAudioPlayParams Params;
        Params.Volume = 1.0f;
        Params.Bus    = EAudioBus::UI;

        PreviewHandle = Audio::Context().PlayAudioGraph(PreviewInstance, Params);

        if (!PreviewHandle.IsValid())
        {
            PreviewInstance.reset();
            return;
        }

        PreviewParameters.clear();
        for (const FAudioGraphParameterDecl& Input : PreviewInstance->GetInputs())
        {
            FPreviewParameter Parameter;
            Parameter.Name       = Input.Name;
            Parameter.Type       = Input.Type;
            Parameter.FloatValue = Input.DefaultFloat;
            Parameter.IntValue   = Input.DefaultInt;
            Parameter.BoolValue  = Input.DefaultBool;

            for (const FPreviewParameter& Old : Previous)
            {
                if (Old.Name != Parameter.Name || Old.Type != Parameter.Type)
                {
                    continue;
                }
                Parameter.FloatValue = Old.FloatValue;
                Parameter.IntValue   = Old.IntValue;
                Parameter.BoolValue  = Old.BoolValue;
                switch (Parameter.Type)
                {
                case EAudioGraphType::Float: PreviewInstance->SetFloatParameter(Parameter.Name, Parameter.FloatValue); break;
                case EAudioGraphType::Int32: PreviewInstance->SetIntParameter(Parameter.Name, Parameter.IntValue);     break;
                case EAudioGraphType::Bool:  PreviewInstance->SetBoolParameter(Parameter.Name, Parameter.BoolValue);   break;
                default: break;
                }
                break;
            }
            PreviewParameters.push_back(Parameter);
        }
    }

    void FAudioGraphEditorTool::StopPreview()
    {
        if (PreviewHandle.IsValid() && Audio::HasDevice())
        {
            Audio::Context().StopSound(PreviewHandle, EAudioStopMode::Immediate);
        }

        PreviewHandle = SAudioHandle::Invalid();
        PreviewInstance.reset();
        PreviewParameters.clear();
        MeterLevels[0] = 0.0f;
        MeterLevels[1] = 0.0f;
    }

    void FAudioGraphEditorTool::OnSave()
    {
        Compile();
        FAssetEditorTool::OnSave();
    }

    void FAudioGraphEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0;
        ImGuiID RightDockID = 0;
        ImGuiID RightBottomDockID = 0;

        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.30f, &RightDockID, &LeftDockID);
        ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Down, 0.45f, &RightBottomDockID, &RightDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(AudioGraphWindowName).c_str(),      LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(AudioPropertiesWindowName).c_str(), RightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(AudioTransportWindowName).c_str(),  RightBottomDockID);
    }

    CEdNodeGraph* FAudioGraphEditorTool::GetActiveNodeGraph()
    {
        return NodeGraph.Get();
    }
}
