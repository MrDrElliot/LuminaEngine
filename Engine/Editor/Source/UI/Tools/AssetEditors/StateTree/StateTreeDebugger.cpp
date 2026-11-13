#include "StateTreeDebugger.h"

#include "Containers/Algorithm.h"
#include "Containers/Format.h"
#include "Containers/Pair.h"
#include "Core/Math/Math.h"
#include "HierarchicalTags/HierarchicalTagRegistry.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/EditorTool.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/StateTreeComponent.h"

namespace Lumina
{
    namespace
    {
        constexpr const char* TargetComboId  = "##DebugTarget";
        constexpr const char* StatusLabels[] = { "Running", "Succeeded", "Failed" };
        constexpr const char* KindLabels[]   = { "Enter", "Exit", "Transition", "Event", "Finished" };

        // A linked tree's states have no card of their own, the linking state above them stands for them.
        bool IsAuthoredState(const FStateTreeRuntimeState& State, FName Name)
        {
            return State.Scope == 0 && State.Name == Name;
        }

        FString RuntimeStateName(const FStateTreeRuntime& Runtime, uint16 State)
        {
            return State < Runtime.States.Num() ? Runtime.States[State].Name.ToString() : FString();
        }

        // The runtime node the authored Selection compiled into, none when the running tree no longer matches it.
        uint16 FindRuntimeNode(const FStateTreeRuntime& Runtime, CStateTree& Tree, const FStateTreeSelection& Selection)
        {
            const FInstancedStruct* Authored = StateTreeEditing::FindNode(Tree, Selection);
            if (Authored == nullptr)
            {
                return kStateTreeNoIndex;
            }

            uint16 First = 0;
            uint16 Num = Runtime.NumEvaluators;
            if (Selection.Slot != EStateTreeSlot::Evaluator)
            {
                const FName Name = Tree.States[Selection.State].Name;
                const int32 State = Algo::IndexOfIf(Runtime.States, [Name](const FStateTreeRuntimeState& Candidate) { return IsAuthoredState(Candidate, Name); });
                if (State == Constants::kIndexNone)
                {
                    return kStateTreeNoIndex;
                }
                const FStateTreeRuntimeState& Compiled = Runtime.States[State];
                if (Selection.Slot == EStateTreeSlot::Task)
                {
                    First = Compiled.FirstTask;
                    Num = Compiled.NumTasks;
                }
                else if (Selection.Slot == EStateTreeSlot::EnterCondition)
                {
                    First = Compiled.FirstEnterCondition;
                    Num = Compiled.NumEnterConditions;
                }
                else if (Selection.Transition < Compiled.NumTransitions)
                {
                    const FStateTreeRuntimeTransition& Transition = Runtime.Transitions[Compiled.FirstTransition + Selection.Transition];
                    First = Transition.FirstCondition;
                    Num = Transition.NumConditions;
                }
                else
                {
                    return kStateTreeNoIndex;
                }
            }
            const uint16 Node = (uint16)(First + Selection.Index);
            return Selection.Index < Num && Runtime.Nodes[Node].Definition.GetScriptStruct() == Authored->GetScriptStruct() ? Node : kStateTreeNoIndex;
        }

        FFixedString EntityLabel(CWorld& World, ECS::FEntity Entity)
        {
            const SNameComponent* Name = World.TryGetComponent<SNameComponent>(Entity);
            return FFixedString(*Format("{} ({})", Name != nullptr ? Name->Name.ToString() : FString("Entity"), World.GetName().ToString()));
        }
    }

    ImVec4 FStateTreeDebugger::GetStatusColor(EStateTreeRunStatus Status)
    {
        switch (Status)
        {
            case EStateTreeRunStatus::Succeeded: return EditorColors::Success();
            case EStateTreeRunStatus::Failed:    return EditorColors::Danger();
            default:                             return EditorColors::Warning();
        }
    }

    void FStateTreeDebugger::SetTarget(CWorld* InWorld, ECS::FEntity InEntity)
    {
        if (FStateTreeRunningInstance* Previous = FindTarget())
        {
            Previous->Instance->SetTraced(false);
        }
        World = InWorld;
        Entity = InEntity;
        bLive = true;
        if (FStateTreeRunningInstance* Running = FindTarget())
        {
            Running->Instance->SetTraced(true);
        }
    }

    FStateTreeRunningInstance* FStateTreeDebugger::FindTarget() const
    {
        CWorld* Target = World.Get();
        if (Target == nullptr || !Target->IsValidEntity(Entity))
        {
            return nullptr;
        }
        SStateTreeComponent* Component = Target->TryGetComponent<SStateTreeComponent>(Entity);
        return Component != nullptr && Component->Running.Instance != nullptr ? &Component->Running : nullptr;
    }

    FStateTreeInstance* FStateTreeDebugger::FindInstance(const CStateTree& Tree) const
    {
        FStateTreeRunningInstance* Running = FindTarget();
        return Running != nullptr && Running->Tree.Get() == &Tree ? Running->Instance.get() : nullptr;
    }

    TOptional<EStateTreeRunStatus> FStateTreeDebugger::GetStateStatus(const CStateTree& Tree, int32 State) const
    {
        const FStateTreeInstance* Instance = FindInstance(Tree);
        if (Instance == nullptr)
        {
            return {};
        }
        TSpan<const FStateTreeActiveState> Path = Instance->GetActivePath();
        if (const FStateTreeTrace* Trace = Instance->GetTrace(); Trace != nullptr && !bLive)
        {
            const int32 Record = Trace->FindAt(ScrubTime);
            Path = Record != Constants::kIndexNone ? Trace->GetRecords()[Record].GetPath() : TSpan<const FStateTreeActiveState>();
        }
        const FName Name = Tree.States[State].Name;
        const FStateTreeRuntime& Runtime = *Instance->GetRuntime();
        for (const FStateTreeActiveState& Active : Path)
        {
            if (Active.State < Runtime.States.Num() && IsAuthoredState(Runtime.States[Active.State], Name))
            {
                return Active.Status;
            }
        }
        return {};
    }

    FStateTreeSelection FStateTreeDebugger::GetLastTransition(const CStateTree& Tree) const
    {
        const FStateTreeInstance* Instance = FindInstance(Tree);
        const FStateTreeTrace* Trace = Instance != nullptr ? Instance->GetTrace() : nullptr;
        if (Trace == nullptr)
        {
            return {};
        }
        const TRingBuffer<FStateTreeTraceRecord>& Records = Trace->GetRecords();
        const FStateTreeRuntime& Runtime = *Instance->GetRuntime();
        for (int32 Record = bLive ? Records.Num() - 1 : Trace->FindAt(ScrubTime); Record >= 0; --Record)
        {
            if (Records[Record].Kind != EStateTreeTraceKind::Transition)
            {
                continue;
            }
            const uint32 Taken = Records[Record].Value;
            const int32 Source = Algo::IndexOfIf(Runtime.States, [Taken](const FStateTreeRuntimeState& Candidate)
            {
                return Candidate.Scope == 0 && Taken >= Candidate.FirstTransition && Taken < (uint32)(Candidate.FirstTransition + Candidate.NumTransitions);
            });
            const int32 State = Source != Constants::kIndexNone ? Algo::IndexOf(Tree.States, Runtime.States[Source].Name, &SStateTreeState::Name) : Constants::kIndexNone;
            return State != Constants::kIndexNone ? FStateTreeSelection{ State, EStateTreeSlot::Transition, (int32)(Taken - Runtime.States[Source].FirstTransition) } : FStateTreeSelection();
        }
        return {};
    }

    void FStateTreeDebugger::ScrubTo(double Time)
    {
        ScrubTime = Time;
        bLive = false;
    }

    void FStateTreeDebugger::Draw(const CStateTree& Tree)
    {
        DrawTargetPicker(Tree);
        FStateTreeInstance* Instance = FindInstance(Tree);
        if (Instance == nullptr)
        {
            FEditorTool::DrawEmptyState(LE_ICON_BUG, "Not debugging", "Pick an entity running this tree during Play.");
            return;
        }
        // Restarting the component during Play makes a new instance, which is traced from here on.
        Instance->SetTraced(true);
        if (const FStateTreeTrace* Trace = Instance->GetTrace())
        {
            DrawTimeline(*Instance, *Trace);
        }
    }

    void FStateTreeDebugger::DrawTargetPicker(const CStateTree& Tree)
    {
        TVector<TPair<CWorld*, ECS::FEntity>> Targets;
        int32 Current = Constants::kIndexNone;
        if (Global::GWorldManager != nullptr && ImGuiX::IsSearchableComboOpen(TargetComboId))
        {
            Global::GWorldManager->ForEachWorld([&](CWorld& Candidate)
            {
                for (ECS::FEntity Running : Candidate.View<SStateTreeComponent>())
                {
                    const FStateTreeRunningInstance& Instance = Candidate.GetComponent<SStateTreeComponent>(Running).Running;
                    if (Instance.Instance != nullptr && Instance.Tree.Get() == &Tree)
                    {
                        Current = World.Get() == &Candidate && Entity == Running ? Targets.Num() : Current;
                        Targets.Add({ &Candidate, Running });
                    }
                }
            });
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(LE_ICON_BUG " Entity");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const FFixedString Preview = FindInstance(Tree) != nullptr ? EntityLabel(*World.Get(), Entity) : FFixedString("None");
        const int32 Picked = ImGuiX::SearchableCombo(TargetComboId, *Preview, Targets.Num(), Current,
            [&Targets](int32 Index) { return EntityLabel(*Targets[Index].Key, Targets[Index].Value); });
        if (Picked != Constants::kIndexNone)
        {
            SetTarget(Targets[Picked].Key, Targets[Picked].Value);
        }
    }

    void FStateTreeDebugger::DrawTimeline(const FStateTreeInstance& Instance, const FStateTreeTrace& Trace)
    {
        const TRingBuffer<FStateTreeTraceRecord>& Records = Trace.GetRecords();
        if (Records.IsEmpty())
        {
            ImGui::TextDisabled("Waiting for the next state change.");
            return;
        }

        const double First = Records.First().Time;
        const double Last = Records.Last().Time;
        double Shown = bLive ? Last : Math::Clamp(ScrubTime, First, Last);
        if (ImGui::Button(bLive ? LE_ICON_PAUSE " Pause" : LE_ICON_PLAY " Live"))
        {
            if (bLive)
            {
                ScrubTo(Last);
            }
            else
            {
                ResumeLive();
            }
        }
        ImGuiX::TextTooltip("Pausing holds the outline at the selected change while the trace keeps recording.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderScalar("##Scrub", ImGuiDataType_Double, &Shown, &First, &Last, "%.3f s"))
        {
            ScrubTo(Shown);
        }

        const int32 Cursor = Trace.FindAt(Shown);
        const FStateTreeRuntime& Runtime = *Instance.GetRuntime();
        constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;
        if (!ImGui::BeginTable("##Timeline", 4, TableFlags))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Change");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Detail");
        ImGui::TableHeadersRow();

        ImGuiListClipper Clipper;
        Clipper.Begin(Records.Num());
        while (Clipper.Step())
        {
            for (int32 Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
            {
                const FStateTreeTraceRecord& Record = Records[Index];
                ImGui::PushID(Index);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, Index > Cursor ? EditorColors::TextDim() : EditorColors::TextPrimary());
                if (ImGui::Selectable(*Format("{:.3f}", Record.Time), Index == Cursor, ImGuiSelectableFlags_SpanAllColumns))
                {
                    ScrubTo(Record.Time);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(KindLabels[(int32)Record.Kind]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(*RuntimeStateName(Runtime, Record.State));
                ImGui::TableNextColumn();
                switch (Record.Kind)
                {
                    case EStateTreeTraceKind::Event:
                        ImGui::TextUnformatted(*FHierarchicalTagRegistry::Get().GetName(Record.Value));
                        break;
                    case EStateTreeTraceKind::Transition:
                        if (Record.Value < (uint32)Runtime.Transitions.Num() && Runtime.Transitions[Record.Value].Target != EStateTreeTransitionTarget::State)
                        {
                            ImGui::TextUnformatted(Runtime.Transitions[Record.Value].Target == EStateTreeTransitionTarget::Succeeded ? "Succeed" : "Fail");
                        }
                        break;
                    default:
                        ImGui::TextColored(GetStatusColor(Record.Status), "%s", StatusLabels[(int32)Record.Status]);
                        break;
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        if (bLive && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }

    void FStateTreeDebugger::DrawInstanceData(CStateTree& Tree, const FStateTreeSelection& Selection)
    {
        const FStateTreeInstance* Instance = FindInstance(Tree);
        const uint16 Node = Instance != nullptr ? FindRuntimeNode(*Instance->GetRuntime(), Tree, Selection) : kStateTreeNoIndex;
        CStruct* DataType = Node != kStateTreeNoIndex ? Instance->GetRuntime()->Nodes[Node].InstanceDataType.Get() : nullptr;
        if (DataType == nullptr)
        {
            return;
        }
        ImGui::SeparatorText(LE_ICON_BUG " Live Instance Data");
        void* Data = Instance->GetNodeInstanceData(Node);
        if (InstanceDataTable.GetObject() != Data || InstanceDataTable.GetType() != DataType)
        {
            InstanceDataTable.SetObject(Data, DataType);
        }
        InstanceDataTable.DrawTree(true);
    }
}
