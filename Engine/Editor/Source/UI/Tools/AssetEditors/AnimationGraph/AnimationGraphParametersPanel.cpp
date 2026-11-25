#include "AnimationGraphEditorTool.h"

#include <cctype>
#include <cfloat>

#include "Animation/AnimGraphOps.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "Memory/Memcpy.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Scripting/ScriptStruct.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Widgets/ComponentPicker.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        constexpr float kAddButtonWidth   = 90.0f;

        const char* LookupLabel(EAnimGraphInputLookup Lookup)
        {
            return Lookup == EAnimGraphInputLookup::Self ? "This entity" : "This or a parent";
        }

        const char* LookupHint(EAnimGraphInputLookup Lookup)
        {
            return Lookup == EAnimGraphInputLookup::Self
                ? "Read only from the entity the graph animates."
                : "Read from the entity the graph animates, or else the nearest parent that has one, as for a mesh under its character.";
        }

        // SAlsLocomotionComponent reads as AlsLocomotion, which is what its paths start with.
        FString SuggestInputName(const CStruct& Struct)
        {
            const FString Full = Struct.GetName().ToString();
            FStringView Name(Full);
            const int32 Dot = Name.FindLast('.');
            if (Dot != Constants::kIndexNone)
            {
                Name = Name.Mid(Dot + 1);
            }
            if (Name.Len() > 1 && Name[0] == 'S' && std::isupper((unsigned char)Name[1]))
            {
                Name = Name.Mid(1);
            }
            const FStringView Suffix = "Component";
            if (Name.Len() > Suffix.Len() && Name.EndsWith(Suffix))
            {
                Name = Name.LeftChop(Suffix.Len());
            }
            return Name.IsEmpty() ? FString("Input") : FString(Name);
        }

        FName UniqueInputName(const CAnimationGraph& Graph, const FString& Base, int32 Ignore = Constants::kIndexNone)
        {
            FName Candidate(*Base);
            for (int32 Suffix = 2;; ++Suffix)
            {
                const int32 Existing = Graph.FindInput(Candidate);
                if (Existing == Constants::kIndexNone || Existing == Ignore)
                {
                    return Candidate;
                }
                Candidate = FName(*Format("{}{}", Base, Suffix));
            }
        }

        // The component an input reads on the entity or, when its lookup allows, on the nearest parent holding one.
        void MutedWrapped(const char* Text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
            ImGui::TextWrapped("%s", Text);
            ImGui::PopStyleColor();
        }
    }

    void FAnimationGraphEditorTool::DrawParameters(CAnimationGraph* Graph)
    {
        // A debugged live entity shows its own values, and otherwise the preview does, holding each input's component so it can be tried here.
        CWorld* ValuesWorld = World.Get();
        ECS::FEntity ValuesEntity = MeshEntity;
        bool bPreview = true;

        CWorld* TargetWorld = nullptr;
        ECS::FEntity TargetEntity = ECS::NullEntity;
        if (bDebugEnabled && ResolveDebugTarget(TargetWorld, TargetEntity) && TargetWorld != nullptr && TargetWorld != World.Get())
        {
            ValuesWorld  = TargetWorld;
            ValuesEntity = TargetEntity;
            bPreview     = false;
        }
        if (ValuesWorld == nullptr || !ValuesWorld->IsValidEntity(ValuesEntity))
        {
            ValuesEntity = ECS::NullEntity;
        }

        DrawInputs(Graph, ValuesWorld, ValuesEntity, bPreview);
        ImGui::Spacing();
        DrawGraphParameters(Graph);
    }

    void FAnimationGraphEditorTool::DrawInputs(CAnimationGraph* Graph, CWorld* ValuesWorld, ECS::FEntity ValuesEntity, bool bPreview)
    {
        const float RightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const bool bOpen = ImGui::CollapsingHeader(LE_ICON_PUZZLE_OUTLINE "  Inputs##InputsSection", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

        ImGui::SameLine(Math::Max(RightEdge - kAddButtonWidth, ImGui::GetCursorPosX()));
        const FPickerItem* Picked = InputPicker.DrawButton("##AddInput", LE_ICON_PLUS " Add", [Graph](TVector<FPickerItem>& Out)
        {
            ComponentPicker::GatherItems(Out, [Graph](CStruct* Struct)
            {
                return Algo::AnyOf(Graph->Inputs, [Struct](const SAnimGraphInput& Input) { return Input.Component == Struct->GetName(); });
            });
        }, kAddButtonWidth);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal("Read a component straight off the entity, such as the one holding your character's movement");
        }
        if (Picked != nullptr)
        {
            if (CStruct* Struct = ComponentPicker::FindComponent(Picked->Key))
            {
                AddInput(Graph, *Struct);
            }
        }

        if (!bOpen)
        {
            return;
        }

        if (Graph->Inputs.IsEmpty())
        {
            MutedWrapped("Inputs read components straight off the entity each update, so gameplay never copies values into the graph. "
                "Add the component that holds your character's data, then pick its fields in Get Parameter nodes, read as Input.Field.");
            return;
        }

        MutedWrapped(bPreview ? "Preview values. Edit them to try the graph." : "Live values from the debugged entity.");
        ImGui::Spacing();

        int32 Remove = Constants::kIndexNone;
        for (int32 Index = 0; Index < Graph->Inputs.Num(); ++Index)
        {
            DrawInput(Graph, Index, ValuesWorld, ValuesEntity, bPreview, Remove);
        }
        if (Remove != Constants::kIndexNone)
        {
            RemoveInput(Graph, Remove);
        }
    }

    void FAnimationGraphEditorTool::DrawInput(CAnimationGraph* Graph, int32 Index, CWorld* ValuesWorld, ECS::FEntity ValuesEntity, bool bPreview, int32& OutRemove)
    {
        SAnimGraphInput& Input = Graph->Inputs[Index];
        CStruct* Struct = Input.Component.IsNone() ? nullptr : FindComponentStruct(Input.Component.ToString());
        const char* Icon = Struct == nullptr ? LE_ICON_ALERT_CIRCLE_OUTLINE
            : (Cast<CScriptStruct>(Struct) != nullptr ? LE_ICON_LANGUAGE_CSHARP : LE_ICON_LANGUAGE_CPP);

        ImGui::PushID(Index);
        const float RightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const bool bOpen = ImGui::TreeNodeEx("##Input",
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding,
            "%s  %s", Icon, *Input.Name);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            RenamingInput = Index;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Struct != nullptr ? *Struct->GetName() : "missing component");

        const float MenuWidth = ImGui::GetFrameHeight();
        const float ComboWidth = ImGui::CalcTextSize(LookupLabel(EAnimGraphInputLookup::SelfOrAncestor)).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 3.0f;
        ImGui::SameLine(Math::Max(RightEdge - ComboWidth - MenuWidth - ImGui::GetStyle().ItemSpacing.x, ImGui::GetCursorPosX()));
        ImGui::SetNextItemWidth(ComboWidth);
        if (ImGui::BeginCombo("##Lookup", LookupLabel(Input.Lookup)))
        {
            for (const EAnimGraphInputLookup Option : { EAnimGraphInputLookup::SelfOrAncestor, EAnimGraphInputLookup::Self })
            {
                if (ImGui::Selectable(LookupLabel(Option), Option == Input.Lookup) && Option != Input.Lookup)
                {
                    BeginAssetTransaction("Change Input Lookup");
                    Input.Lookup = Option;
                    EndAssetTransaction();
                    NotifyAssetDataChanged();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGuiX::TextTooltip_Internal(LookupHint(Option));
                }
            }
            ImGui::EndCombo();
        }
        else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGuiX::TextTooltip_Internal(LookupHint(Input.Lookup));
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_DOTS_VERTICAL "##Menu", ImVec2(MenuWidth, 0.0f)))
        {
            ImGui::OpenPopup("InputMenu");
        }
        if (ImGui::BeginPopup("InputMenu"))
        {
            if (ImGui::MenuItem(LE_ICON_PENCIL "  Rename"))
            {
                RenamingInput = Index;
            }
            if (ImGui::MenuItem(LE_ICON_DELETE "  Remove"))
            {
                OutRemove = Index;
            }
            ImGui::EndPopup();
        }

        if (RenamingInput == Index)
        {
            const FString Current = Input.Name.ToString();
            const int32 Count = Math::Min((int32)sizeof(RenameBuffer) - 1, (int32)Current.Len());
            Memory::Memcpy(RenameBuffer, *Current, (SIZE_T)Count);
            RenameBuffer[Count] = 0;
            ImGui::OpenPopup("RenameInput");
            RenamingInput = Constants::kIndexNone;
        }
        if (ImGui::BeginPopup("RenameInput"))
        {
            ImGui::TextDisabled("Nodes reading %s.<field> follow the new name.", *Input.Name);
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputText("##Name", RenameBuffer, sizeof(RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank))
            {
                RenameInput(Graph, Index, FName(RenameBuffer));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (bOpen)
        {
            if (Struct == nullptr)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Warning());
                ImGui::TextWrapped("Component '%s' no longer exists, so its fields read their defaults. Remove this input or add the component it replaces.", *Input.Component);
                ImGui::PopStyleColor();
            }
            else
            {
                DrawInputValues(Graph, Index, *Struct, ValuesWorld, ValuesEntity, bPreview);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void FAnimationGraphEditorTool::DrawInputValues(CAnimationGraph* Graph, int32 Index, CStruct& Struct, CWorld* ValuesWorld, ECS::FEntity ValuesEntity, bool bPreview)
    {
        const SAnimGraphInput& Input = Graph->Inputs[Index];
        void* Data = (ValuesWorld != nullptr && !ValuesEntity.IsNull()) ? AnimGraphOps::FindInputComponent(*ValuesWorld, ValuesEntity, Struct, Input.Lookup) : nullptr;

        // The preview mesh takes a default of the component, so the graph has something to read and its values can be tried.
        if (Data == nullptr && bPreview && ValuesWorld != nullptr && !ValuesEntity.IsNull())
        {
            if (const FComponentOps* Ops = Struct.GetComponentOps())
            {
                Data = Ops->EmplaceDefault(ValuesWorld->GetEntityRegistry(), ValuesEntity);
            }
        }

        if (Data == nullptr)
        {
            MutedWrapped(bPreview
                ? "Pick a preview mesh to try values here."
                : "The debugged entity has none on it or its parents, so the graph reads defaults.");
            InputTables.Remove(Input.Name);
            return;
        }

        DrawInputReads(*Graph, Index, static_cast<uint8*>(Data), bPreview);

        // A component usually holds far more than its graph reads, so the whole of it waits behind a fold.
        if (ImGui::TreeNodeEx("##AllFields", ImGuiTreeNodeFlags_SpanAvailWidth, "All fields of %s", *Struct.GetName()))
        {
            TUniquePtr<FPropertyTable>& Table = InputTables.FindOrAdd(Input.Name);
            if (Table == nullptr)
            {
                Table = MakeUnique<FPropertyTable>(Data, &Struct);
                Table->SetShowSearchBar(true);
                Table->SetContext(&PropertyContext);
            }
            else if (Table->GetObject() != Data || Table->GetType() != &Struct)
            {
                Table->SetObject(Data, &Struct);
            }

            if (Table->PrepareAndTestFilter())
            {
                Table->DrawTree(true);
            }
            ImGui::TreePop();
        }
    }

    void FAnimationGraphEditorTool::DrawInputReads(const CAnimationGraph& Graph, int32 Index, uint8* Data, bool bPreview)
    {
        const FString Prefix = Graph.Inputs[Index].Name.ToString() + ".";
        TVector<TPair<FName, bool>> Reads;
        for (const FAnimGraphParameter& Parameter : Graph.Parameters)
        {
            if (FStringView(Parameter.Name.ToString()).StartsWith(Prefix))
            {
                Reads.Emplace(Parameter.Name, false);
            }
        }
        for (const SAnimGraphObjectParameter& Parameter : Graph.ObjectParameters)
        {
            if (FStringView(Parameter.Name.ToString()).StartsWith(Prefix))
            {
                Reads.Emplace(Parameter.Name, true);
            }
        }

        Algo::Sort(Reads, [](const TPair<FName, bool>& A, const TPair<FName, bool>& B) { return A.Key.ToString() < B.Key.ToString(); });

        if (Reads.IsEmpty())
        {
            MutedWrapped(*Format("No node reads this yet. Pick its fields in Get Parameter nodes, as {}<field>.", Prefix));
            return;
        }

        const uint8* Bases[kMaxAnimGraphInputs] = {};
        Bases[Index] = Data;
        if (!ImGui::BeginTable("##Reads", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            return;
        }
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.45f);

        for (const auto& [Path, bObject] : Reads)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(*Path);
            ImGui::TextUnformatted(*FString(FStringView(Path.ToString()).Mid(Prefix.Len())));
            ImGui::TableNextColumn();

            FAnimGraphParamBinding Binding;
            const FProperty* Leaf = nullptr;
            const uint8* Base = Graph.ResolveParameterPath(Path, bObject, Binding, nullptr, &Leaf) ? ResolveAnimParamBase(Binding, nullptr, Bases) : nullptr;
            if (Base == nullptr)
            {
                ImGui::TextDisabled(bObject ? "None" : "default");
            }
            else if (bObject)
            {
                const CObject* Object = ReadAnimParamObject(Base, Binding);
                ImGui::TextUnformatted(Object != nullptr ? *Object->GetName() : "None");
            }
            else
            {
                // Editable only in place, since a value reached through an asset belongs to that asset.
                float Value = ReadAnimParamScalar(Base, Binding);
                const bool bEditable = bPreview && Binding.HopOffset == kAnimNoHop;
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::BeginDisabled(!bEditable);
                bool bChanged = false;
                CEnum* Enum = Leaf != nullptr && Leaf->GetType() == EPropertyTypeFlags::Enum ? static_cast<const FEnumProperty*>(Leaf)->GetEnum() : nullptr;
                if (Binding.Type == EAnimParamValueType::Bool)
                {
                    bool bValue = Value > 0.5f;
                    bChanged = ImGui::Checkbox("##Value", &bValue);
                    Value = bValue ? 1.0f : 0.0f;
                }
                else if (Enum != nullptr)
                {
                    const uint64 Current = (uint64)Value;
                    if (ImGui::BeginCombo("##Value", *Enum->GetNameAtValue(Current)))
                    {
                        for (const TPair<FName, uint64>& Entry : Enum->Names)
                        {
                            if (ImGui::Selectable(*Entry.Key, Entry.Value == Current))
                            {
                                Value = (float)Entry.Value;
                                bChanged = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                else if (Binding.Type != EAnimParamValueType::Float && Binding.Type != EAnimParamValueType::Double)
                {
                    int32 Whole = (int32)Value;
                    bChanged = ImGui::DragInt("##Value", &Whole);
                    Value = (float)Whole;
                }
                else
                {
                    bChanged = ImGui::DragFloat("##Value", &Value, 0.01f, 0.0f, 0.0f, "%.3f");
                }
                ImGui::EndDisabled();
                if (bChanged && bEditable)
                {
                    WriteAnimParamScalar(const_cast<uint8*>(Base), Binding, Value);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void FAnimationGraphEditorTool::DrawGraphParameters(CAnimationGraph* Graph)
    {
        if (!ImGui::CollapsingHeader(LE_ICON_TUNE_VARIANT "  Graph Parameters##ParametersSection", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        CStruct* Struct = Graph->GetParameterStruct();
        if (Struct == nullptr)
        {
            MutedWrapped("Optional. A Parameter Struct in Graph Properties gives each graph instance values of its own, which code sets "
                "through GetParameters on the Animation Graph Component. Read with a plain field name.");
            return;
        }

        MutedWrapped("This graph instance's own values, set from code through GetParameters. Read with a plain field name.");

        SAnimationGraphComponent* Comp = (World.IsValid() && MeshEntity != ECS::NullEntity && World->IsValidEntity(MeshEntity))
            ? World->TryGetComponent<SAnimationGraphComponent>(MeshEntity)
            : nullptr;

        void* Base = Comp != nullptr ? Comp->GetParameterMemory() : nullptr;
        if (Base == nullptr)
        {
            ParameterTable.Reset();
            MutedWrapped("Pick a preview mesh to try values here.");
            return;
        }

        if (ParameterTable == nullptr)
        {
            ParameterTable = MakeUnique<FPropertyTable>(Base, Struct);
            ParameterTable->SetShowSearchBar(false);
            ParameterTable->SetContext(&PropertyContext);
        }
        else if (ParameterTable->GetObject() != Base || ParameterTable->GetType() != Struct)
        {
            ParameterTable->SetObject(Base, Struct);
        }

        if (!ParameterTable->PrepareAndTestFilter())
        {
            MutedWrapped("The struct declares no fields a graph can read.");
            return;
        }

        ParameterTable->DrawTree(true);
    }

    void FAnimationGraphEditorTool::AddInput(CAnimationGraph* Graph, CStruct& Component)
    {
        BeginAssetTransaction("Add Input");
        SAnimGraphInput& Input = Graph->Inputs.EmplaceGetRef();
        Input.Component = Component.GetName();
        Input.Name      = UniqueInputName(*Graph, SuggestInputName(Component));
        EndAssetTransaction();

        Graph->LinkParameters();
        NotifyAssetDataChanged();
    }

    void FAnimationGraphEditorTool::RemoveInput(CAnimationGraph* Graph, int32 Index)
    {
        if (!Graph->Inputs.IsValidIndex(Index))
        {
            return;
        }

        InputTables.Remove(Graph->Inputs[Index].Name);
        BeginAssetTransaction("Remove Input");
        Graph->Inputs.RemoveAt(Index);
        EndAssetTransaction();

        Graph->LinkParameters();
        NotifyAssetDataChanged();
    }

    void FAnimationGraphEditorTool::RenameInput(CAnimationGraph* Graph, int32 Index, FName NewName)
    {
        if (!Graph->Inputs.IsValidIndex(Index) || NewName.IsNone() || NewName == Graph->Inputs[Index].Name)
        {
            return;
        }

        const FName OldName = Graph->Inputs[Index].Name;
        NewName = UniqueInputName(*Graph, NewName.ToString(), Index);

        // One undo step, the asset's input and every graph whose nodes read it.
        BeginAssetTransaction("Rename Input");
        Graph->Inputs[Index].Name = NewName;
        for (CEdNodeGraph* Changed : AnimGraphOps::RenameInputPaths(NodeGraph.Get(), OldName, NewName))
        {
            Changed->CommitEdit("Rename Input");
            Changed->NotifyContentChanged();
        }
        EndAssetTransaction();

        if (TUniquePtr<FPropertyTable>* Table = InputTables.Find(OldName))
        {
            TUniquePtr<FPropertyTable> Moved = Move(*Table);
            InputTables.Remove(OldName);
            InputTables.FindOrAdd(NewName) = Move(Moved);
        }

        Graph->LinkParameters();
        NotifyAssetDataChanged();
    }
}
