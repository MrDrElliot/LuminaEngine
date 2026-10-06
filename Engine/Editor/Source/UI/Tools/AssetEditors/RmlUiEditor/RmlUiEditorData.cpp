#include "RmlUiEditorTool.h"

#include <algorithm>
#include <string>
#include "Scripting/DotNet/DotNetHost.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "UI/UIScript.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/RmlUiBridge.h"
#include "Core/Reflection/PropertyText.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include <imgui.h>

namespace Lumina
{
    // Named rather than anonymous, so a unity build merging this with the editor tool cannot collide on helper names.
    namespace RmlDataPanel
    {
        constexpr int32 kMaxPreviewCommands = 12;
        constexpr ImVec4 kDesignErrorColor{1.00f, 0.45f, 0.40f, 1.00f};
        constexpr ImVec4 kDesignInfoColor{0.95f, 0.80f, 0.40f, 1.00f};
        constexpr ImVec4 kDesignDimColor{0.60f, 0.62f, 0.68f, 1.00f};

        const char* DesignTypeLabel(EUIVarType Type)
        {
            switch (Type)
            {
                case EUIVarType::Bool:   return "bool";
                case EUIVarType::Int:    return "int";
                case EUIVarType::Float:  return "float";
                case EUIVarType::Double: return "double";
                default:                 return "string";
            }
        }

        FString DesignMemberKey(const FString& Model, const FString& Member)
        {
            return Model + "." + Member;
        }

        // Grows a list with placeholder rows or trims it, so a data-for can be previewed at any length.
        void ResizeDesignRows(FUIDesignList& List, int32 Count)
        {
            Count = std::max(0, Count);
            while ((int32)List.Rows.size() > Count)
            {
                List.Rows.pop_back();
            }
            while ((int32)List.Rows.size() < Count)
            {
                TVector<FString>& Cells = List.Rows.emplace_back();
                for (const FString& Member : List.Members)
                {
                    Cells.push_back(Member + " " + std::to_string(List.Rows.size()).c_str());
                }
                if (List.Members.empty())
                {
                    Cells.push_back(List.Name + " " + std::to_string(List.Rows.size()).c_str());
                }
            }
        }

        constexpr size_t kPlaceholderRows = 3;

        // Text members read "Name 2" and numbers take the row number, so each repeated item is told apart in the preview.
        void FillPlaceholderItem(const FProperty* Inner, void* Item, size_t Row, const FString& ListName)
        {
            auto Fill = [Row](const FProperty* Field, void* Container, const char* Label)
            {
                const EPropertyTypeFlags Type = Field->GetType();
                const std::string Text = (Type == EPropertyTypeFlags::String || Type == EPropertyTypeFlags::Name)
                    ? std::string(Label) + " " + std::to_string(Row)
                    : std::to_string(Row);
                if (Type != EPropertyTypeFlags::Bool)
                {
                    Reflection::FromText(Field, Container, FStringView(Text.c_str(), Text.size()));
                }
            };

            if (Inner->GetType() == EPropertyTypeFlags::Struct)
            {
                for (const FProperty* Member : static_cast<const FStructProperty*>(Inner)->GetStruct()->GetProperties())
                {
                    Fill(Member, Item, Member->GetPropertyName().c_str());
                }
            }
            else
            {
                Fill(Inner, Item, ListName.c_str());
            }
        }

        // An empty list on the preview instance gets placeholder items, so a data-for shows its layout before the game fills it.
        void FillPreviewLists(CObject* Preview, const FString& ModelName, const THashMap<FString, int32>& RowCounts)
        {
            TVector<UIBinding::FBoundValue> Values;
            UIBinding::GetBoundValues(Preview->GetClass(), Values);
            for (const UIBinding::FBoundValue& Bound : Values)
            {
                if (Bound.Property->GetType() != EPropertyTypeFlags::Vector)
                {
                    continue;
                }
                const FArrayProperty* Array = static_cast<const FArrayProperty*>(Bound.Property);
                void* Items = Bound.Property->GetValuePtr<void>(Preview);
                const size_t Existing = Array->GetNum(Items);
                auto Count = RowCounts.find(DesignMemberKey(ModelName, Bound.Name));
                const size_t Wanted = Count != RowCounts.end() ? (size_t)Count->second : (Existing == 0 ? kPlaceholderRows : Existing);
                Array->GetOps()->Resize(Items, Wanted);
                for (size_t Row = Existing; Row < Wanted; ++Row)
                {
                    FillPlaceholderItem(Array->GetInternalProperty(), Array->GetAt(Items, Row), Row + 1, Bound.Name);
                }
            }
        }

        std::string DesignListSnippet(const FUIDesignList& List)
        {
            std::string Snippet = "<div data-for=\"item : " + std::string(List.Name.c_str()) + "\">";
            for (const FString& Member : List.Members)
            {
                Snippet += "{{ item." + std::string(Member.c_str()) + " }} ";
            }
            if (!List.Members.empty())
            {
                Snippet.pop_back();
            }
            Snippet += "</div>";
            return Snippet;
        }

        void DataSectionHeader(const char* Icon, const char* Label)
        {
            ImGui::TextColored(ImVec4(0.60f, 0.85f, 1.00f, 1.00f), "%s  %s", Icon, Label);
        }

        std::string DesignCommandSnippet(const FUIDesignCommand& Command)
        {
            std::string Snippet = "data-event-click=\"" + std::string(Command.Name.c_str()) + "(";
            for (size_t Index = 0; Index < Command.Params.size(); ++Index)
            {
                Snippet += (Index > 0 ? ", " : "") + std::string("'") + Command.Params[Index].c_str() + "'";
            }
            return Snippet + ")\"";
        }
    }

    using namespace RmlDataPanel;

    void FRmlUiEditorTool::ReleaseDesignObjects()
    {
        if (PreviewContext != nullptr)
        {
            RmlUi::SetEditorDesignModels(PreviewContext, TVector<FUIDesignModel>());
        }
        for (void* Model : DesignObjectModels)
        {
            RmlUi::DestroyObjectModel(Model);
        }
        DesignObjectModels.clear();
        DesignObjects.clear();
    }

    void FRmlUiEditorTool::RefreshDesignModels(const std::string& Body)
    {
        RmlBinding::Scan(Body, Bindings);

        ReleaseDesignObjects();
        DesignModels.clear();
        DesignDescribed.clear();

        TVector<FUIDesignModel> Inferred;
        for (const FString& Name : Bindings.Models)
        {
            const bool bSeen = std::any_of(DesignModels.begin(), DesignModels.end(), [&Name](const FUIDesignModel& Model) { return Model.Name == Name; });
            if (bSeen || Name.empty())
            {
                continue;
            }

            // A transient instance of the class, bound the way the game binds it, so the preview shows the class's own defaults.
            FUIDesignModel Model;
            CObject* Preview = nullptr;
            if (CClass* Class = UIBinding::FindModelClass(FStringView(Name.c_str(), Name.size())))
            {
                Preview = NewObject(Class, nullptr, NAME_None, FGuid::New(), OF_Transient);
            }

            if (Preview != nullptr)
            {
                for (auto& [Key, Value] : DesignOverrides)
                {
                    const FString Prefix = Name + ".";
                    if (Key.rfind(Prefix, 0) == 0)
                    {
                        void* Container = nullptr;
                        const FStringView Member(Key.c_str() + Prefix.size(), Key.size() - Prefix.size());
                        if (const FProperty* Property = UIBinding::ResolvePath(Preview, Member, Container))
                        {
                            Reflection::FromText(Property, Container, FStringView(Value.c_str(), Value.size()));
                        }
                    }
                }
                FillPreviewLists(Preview, Name, DesignRowCounts);
                UIBinding::Describe(Preview, Model);
                Model.Name = Name;
            }
            else
            {
                Model = RmlBinding::Infer(Name, Bindings);
                for (FUIDesignScalar& Scalar : Model.Scalars)
                {
                    auto Override = DesignOverrides.find(DesignMemberKey(Model.Name, Scalar.Name));
                    if (Override != DesignOverrides.end())
                    {
                        Scalar.Value = Override->second;
                    }
                }
                for (FUIDesignList& List : Model.Lists)
                {
                    auto Count = DesignRowCounts.find(DesignMemberKey(Model.Name, List.Name));
                    if (Count != DesignRowCounts.end())
                    {
                        ResizeDesignRows(List, Count->second);
                    }
                }
                Inferred.push_back(Model);
            }

            DesignModels.push_back(Move(Model));
            DesignDescribed.push_back(Preview != nullptr);
            DesignObjects.push_back(TStrongObjectPtr<CObject>(Preview));
            DesignObjectModels.push_back(nullptr);
        }

        RmlBinding::Lint(Bindings, DesignModels, DesignDescribed, BindingProblems);
        ApplyBindingMarkers();

        if (PreviewContext != nullptr)
        {
            RmlUi::SetEditorDesignModels(PreviewContext, Inferred);
            for (size_t Index = 0; Index < DesignObjects.size(); ++Index)
            {
                if (CObject* Preview = DesignObjects[Index].Get())
                {
                    const FString& Name = DesignModels[Index].Name;
                    DesignObjectModels[Index] = RmlUi::CreateObjectModel(PreviewContext, FStringView(Name.c_str(), Name.size()), Preview, true);
                }
            }
        }
        UIBinding::EnumerateModelClasses(AvailableModels);
        SeenScriptGeneration = DotNet::IsInitialized() ? DotNet::GetScriptGeneration() : -1;
    }

    void FRmlUiEditorTool::ApplyBindingMarkers()
    {
        CodeEditor.ClearMarkers();
        for (const RmlBinding::FProblem& Problem : BindingProblems)
        {
            // The number carries the color and the line only a faint tint, so the marked text stays readable.
            ImVec4 Tint = Problem.bError ? kDesignErrorColor : kDesignInfoColor;
            const ImU32 NumberColor = ImGui::ColorConvertFloat4ToU32(Tint);
            Tint.w = 0.14f;
            const ImU32 LineColor = ImGui::ColorConvertFloat4ToU32(Tint);
            const std::string_view Message(Problem.Message.c_str(), Problem.Message.size());
            CodeEditor.AddMarker(Problem.Line - 1, NumberColor, LineColor, Message, Message);
        }
    }

    void FRmlUiEditorTool::PollDesignActivity()
    {
        if (PreviewContext == nullptr || bIsStylesheet)
        {
            return;
        }

        TVector<FString> Fired;
        RmlUi::ConsumeEditorDesignCommands(PreviewContext, Fired);
        for (FString& Call : Fired)
        {
            ImGuiX::Notifications::NotifyInfo("Preview ran {}", Call.c_str());
            FiredCommands.push_back(Move(Call));
        }
        if ((int32)FiredCommands.size() > kMaxPreviewCommands)
        {
            FiredCommands.erase(FiredCommands.begin(), FiredCommands.begin() + (FiredCommands.size() - kMaxPreviewCommands));
        }

        // A script reload can add or rename members, so the preview picks the new model up on its own.
        const int32 Generation = DotNet::IsInitialized() ? DotNet::GetScriptGeneration() : -1;
        if (Generation != SeenScriptGeneration)
        {
            ReloadDocument();
        }
    }

    void FRmlUiEditorTool::JumpToLine(int32 Line)
    {
        const int Target = std::max(0, std::min(CodeEditor.GetLineCount() - 1, Line - 1));
        CodeEditor.SetCursor(Target, 0);
        CodeEditor.ScrollToLine(Target, TextEditor::Scroll::alignMiddle);
        CodeEditor.SetFocus();
    }

    void FRmlUiEditorTool::ConfigureAutoComplete()
    {
        auto ContextOf = [this](const TextEditor::AutoCompleteState& State)
        {
            const std::string Line = CodeEditor.GetLineText((int)State.line);
            const size_t End = std::min(Line.size(), State.searchTermStartIndex);
            return RmlBinding::ClassifyCompletion(std::string_view(Line.data(), End));
        };

        AutoCompleteConfig.triggerOnTyping = true;
        AutoCompleteConfig.triggerInStrings = false;
        AutoCompleteConfig.stringTriggerFilter = [ContextOf](const TextEditor::AutoCompleteState& State)
        {
            return ContextOf(State).Kind != RmlBinding::ECompletion::None;
        };
        AutoCompleteConfig.triggerFilter = [ContextOf](const TextEditor::AutoCompleteState& State)
        {
            const RmlBinding::ECompletion Kind = ContextOf(State).Kind;
            if (Kind == RmlBinding::ECompletion::Attribute)
            {
                // Only once the word looks like a data attribute, so typing class or id stays quiet.
                return State.searchTerm.rfind("d", 0) == 0;
            }
            return Kind != RmlBinding::ECompletion::None;
        };
        AutoCompleteConfig.callback = [this](TextEditor::AutoCompleteState& State)
        {
            OnAutoComplete(State);
        };
        CodeEditor.SetAutoCompleteConfig(bIsStylesheet ? nullptr : &AutoCompleteConfig);
    }

    void FRmlUiEditorTool::OnAutoComplete(TextEditor::AutoCompleteState& State)
    {
        State.suggestions.clear();
        State.suggestionKinds.clear();
        State.suggestionDetails.clear();
        State.suggestionInsertText.clear();

        const std::string Line = CodeEditor.GetLineText((int)State.line);
        const size_t End = std::min(Line.size(), State.searchTermStartIndex);
        const RmlBinding::FCompletionContext Context = RmlBinding::ClassifyCompletion(std::string_view(Line.data(), End));
        const std::string& Term = State.searchTerm;

        auto Matches = [&Term](const std::string& Candidate)
        {
            if (Term.empty())
            {
                return true;
            }
            return std::search(Candidate.begin(), Candidate.end(), Term.begin(), Term.end(),
                [](char A, char B) { return std::tolower((unsigned char)A) == std::tolower((unsigned char)B); }) != Candidate.end();
        };
        auto Add = [&](const std::string& Name, char Kind, const std::string& Detail, const std::string& Insert)
        {
            if (!Matches(Name))
            {
                return;
            }
            State.suggestions.push_back(Name);
            State.suggestionKinds.push_back(Kind);
            State.suggestionDetails.push_back(Detail);
            State.suggestionInsertText.push_back(Insert);
        };

        switch (Context.Kind)
        {
            case RmlBinding::ECompletion::Attribute:
            {
                // The word under the caret may already be past "data-", which the editor does not count as part of it.
                const bool bAfterDash = End >= 5 && std::string_view(Line.data() + End - 5, 5) == "data-";
                for (const auto& [Name, Description] : RmlBinding::KnownAttributes())
                {
                    const std::string Full(Name);
                    const std::string Shown = bAfterDash ? Full.substr(5) : Full;
                    const bool bOpenEnded = Full.back() == '-';
                    Add(Shown, 'a', Description, bOpenEnded ? Shown : Shown + "=\"\v\"");
                }
                break;
            }
            case RmlBinding::ECompletion::Model:
            {
                for (const FUIModelInfo& Info : AvailableModels)
                {
                    Add(std::string(Info.Name.c_str()), 'm', std::string(Info.bScript ? "UIScript " : "ViewModel ") + Info.TypeName.c_str(), "");
                }
                break;
            }
            case RmlBinding::ECompletion::AliasMember:
            {
                const FString Alias = Context.Alias;
                for (const RmlBinding::FForScope& Scope : Bindings.ForScopes)
                {
                    if (Scope.Alias != Alias)
                    {
                        continue;
                    }
                    for (const FUIDesignModel& Model : DesignModels)
                    {
                        for (const FUIDesignList& List : Model.Lists)
                        {
                            if (List.Name != Scope.List)
                            {
                                continue;
                            }
                            for (const FString& Member : List.Members)
                            {
                                Add(std::string(Member.c_str()), 'v', std::string("item of ") + List.Name.c_str(), "");
                            }
                        }
                    }
                }
                break;
            }
            case RmlBinding::ECompletion::ListSource:
            case RmlBinding::ECompletion::Value:
            case RmlBinding::ECompletion::Command:
            {
                const bool bCommands = Context.Kind == RmlBinding::ECompletion::Command;
                for (const FUIDesignModel& Model : DesignModels)
                {
                    if (bCommands)
                    {
                        for (const FUIDesignCommand& Command : Model.Commands)
                        {
                            std::string Params;
                            for (size_t Index = 0; Index < Command.Params.size(); ++Index)
                            {
                                Params += (Index > 0 ? ", " : "") + std::string(Command.Params[Index].c_str());
                            }
                            const std::string Name(Command.Name.c_str());
                            Add(Name, 'f', "(" + Params + ")", Command.Params.empty() ? Name + "()" : Name + "(\v)");
                        }
                    }
                    for (const FUIDesignList& List : Model.Lists)
                    {
                        Add(std::string(List.Name.c_str()), 'l', "collection of " + std::to_string(List.Members.size()) + " members", "");
                    }
                    if (Context.Kind == RmlBinding::ECompletion::ListSource)
                    {
                        continue;
                    }
                    for (const FUIDesignScalar& Scalar : Model.Scalars)
                    {
                        Add(std::string(Scalar.Name.c_str()), 'v', std::string(DesignTypeLabel(Scalar.Type)) + " = " + Scalar.Value.c_str(), "");
                    }
                }
                for (const RmlBinding::FForScope& Scope : Bindings.ForScopes)
                {
                    Add(std::string(Scope.Alias.c_str()), 't', std::string("each item of ") + Scope.List.c_str(), "");
                }
                break;
            }
            default:
                break;
        }
    }

    void FRmlUiEditorTool::DrawDataPanel()
    {
        if (bIsStylesheet)
        {
            ImGui::TextWrapped(LE_ICON_INFORMATION_OUTLINE " Stylesheets bind no data. Open the .rml that links this sheet.");
            return;
        }

        if (ImGui::SmallButton(LE_ICON_REFRESH " Refresh"))
        {
            ReloadDocument(/*bAlwaysReport*/ true);
        }
        ImGuiX::TextTooltip("Describe the models again, after editing their C# without a script reload.");
        ImGui::SameLine();
        ImGui::BeginDisabled(DesignOverrides.empty() && DesignRowCounts.empty());
        if (ImGui::SmallButton(LE_ICON_RESTORE " Defaults"))
        {
            DesignOverrides.clear();
            DesignRowCounts.clear();
            ReloadDocument();
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("Forget the preview values typed here and go back to the C# defaults.");
        ImGui::Separator();

        if (!BindingProblems.empty())
        {
            DataSectionHeader(LE_ICON_ALERT_CIRCLE_OUTLINE, "Problems");
            for (size_t Index = 0; Index < BindingProblems.size(); ++Index)
            {
                const RmlBinding::FProblem& Problem = BindingProblems[Index];
                ImGui::PushID((int)Index);
                ImGui::PushStyleColor(ImGuiCol_Text, Problem.bError ? kDesignErrorColor : kDesignInfoColor);
                char Label[48];
                std::snprintf(Label, sizeof(Label), "%s Line %d", Problem.bError ? LE_ICON_CLOSE_CIRCLE : LE_ICON_INFORMATION, Problem.Line);
                if (ImGui::Selectable(Label, false, ImGuiSelectableFlags_AllowOverlap))
                {
                    JumpToLine(Problem.Line);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", Problem.Message.c_str());
                ImGui::PopID();
            }
            ImGui::Spacing();
        }

        if (DesignModels.empty())
        {
            DataSectionHeader(LE_ICON_DATABASE_OUTLINE, "No data model");
            ImGui::TextWrapped("This document binds no data. Put data-model=\"Name\" on its body, where Name is a class with Bind members, "
                "a UIScript or CUIScript for instance, or the name its DataModel gives. Then use {{ Member }} and the data-* attributes inside it.");
            if (!AvailableModels.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Models in your scripts");
                for (size_t Index = 0; Index < AvailableModels.size(); ++Index)
                {
                    const FUIModelInfo& Info = AvailableModels[Index];
                    ImGui::PushID((int)Index);
                    if (ImGui::SmallButton(LE_ICON_CONTENT_COPY))
                    {
                        const std::string Attribute = "data-model=\"" + std::string(Info.Name.c_str()) + "\"";
                        ImGui::SetClipboardText(Attribute.c_str());
                    }
                    ImGuiX::TextTooltip("Copy data-model=\"{}\"", Info.Name.c_str());
                    ImGui::SameLine();
                    ImGui::Text("%s", Info.Name.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(kDesignDimColor, "%s %s", Info.bScript ? "UI script" : "Bound class", Info.TypeName.c_str());
                    ImGui::PopID();
                }
            }
        }

        for (int32 Index = 0; Index < (int32)DesignModels.size(); ++Index)
        {
            DrawModelSection(Index);
        }
        if (bDesignReloadPending)
        {
            bDesignReloadPending = false;
            ReloadDocument();
        }

        if (!FiredCommands.empty())
        {
            ImGui::Spacing();
            DataSectionHeader(LE_ICON_GESTURE_TAP, "Commands the preview ran");
            for (size_t Index = FiredCommands.size(); Index > 0; --Index)
            {
                ImGui::TextColored(Index == FiredCommands.size() ? ImVec4(0.55f, 0.90f, 0.60f, 1.0f) : kDesignDimColor, "%s", FiredCommands[Index - 1].c_str());
            }
            if (ImGui::SmallButton("Clear"))
            {
                FiredCommands.clear();
            }
        }
    }

    void FRmlUiEditorTool::DrawModelSection(int32 ModelIndex)
    {
        FUIDesignModel& Model = DesignModels[ModelIndex];
        const bool bDescribed = ModelIndex < (int32)DesignDescribed.size() && DesignDescribed[ModelIndex];

        ImGui::PushID(ModelIndex);
        char Header[160];
        std::snprintf(Header, sizeof(Header), LE_ICON_DATABASE " %s", Model.Name.c_str());
        if (!ImGui::CollapsingHeader(Header, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PopID();
            return;
        }

        if (bDescribed)
        {
            ImGui::TextColored(kDesignDimColor, "%s %s", Model.bFromScript ? "UI script" : "Bound class", Model.SourceType.c_str());
        }
        else
        {
            ImGui::TextColored(kDesignInfoColor, "Inferred from this document. No class registers '%s' yet.", Model.Name.c_str());
        }
        ImGui::TextColored(kDesignDimColor, "Values here only feed the preview. Insert puts the binding at the code cursor.");

        const FStringView ModelName(Model.Name.c_str(), Model.Name.size());
        if (!Model.Scalars.empty() && ImGui::BeginTable("##scalars", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("##insert", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            for (size_t Index = 0; Index < Model.Scalars.size(); ++Index)
            {
                FUIDesignScalar& Scalar = Model.Scalars[Index];
                ImGui::PushID((int)Index);
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(Scalar.Name.c_str());
                ImGuiX::TextTooltip("{} {}{}", DesignTypeLabel(Scalar.Type), Scalar.Name.c_str(), Scalar.bWritable ? ", two-way" : ", display only");

                // Read back each frame, since a two-way control in the preview can change it.
                const FStringView Member(Scalar.Name.c_str(), Scalar.Name.size());
                void* ObjectModel = ModelIndex < (int32)DesignObjectModels.size() ? DesignObjectModels[ModelIndex] : nullptr;
                FString Current = ObjectModel != nullptr ? RmlUi::GetObjectModelValue(ObjectModel, Member)
                    : (PreviewContext != nullptr ? RmlUi::GetEditorDesignValue(PreviewContext, ModelName, Member) : Scalar.Value);
                if (ObjectModel != nullptr && !Scalar.bWritable)
                {
                    Current = Scalar.Value;
                }
                bool bChanged = false;

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                switch (Scalar.Type)
                {
                    case EUIVarType::Bool:
                    {
                        bool bValue = Current == "1" || Current == "true";
                        if (ImGui::Checkbox("##v", &bValue))
                        {
                            Current = bValue ? "1" : "0";
                            bChanged = true;
                        }
                        break;
                    }
                    case EUIVarType::Int:
                    {
                        int Value = std::atoi(Current.c_str());
                        if (ImGui::DragInt("##v", &Value))
                        {
                            Current = std::to_string(Value).c_str();
                            bChanged = true;
                        }
                        break;
                    }
                    case EUIVarType::Float:
                    case EUIVarType::Double:
                    {
                        float Value = (float)std::atof(Current.c_str());
                        if (ImGui::DragFloat("##v", &Value, 0.1f))
                        {
                            Current = std::to_string(Value).c_str();
                            bChanged = true;
                        }
                        break;
                    }
                    default:
                    {
                        char Buffer[256];
                        std::snprintf(Buffer, sizeof(Buffer), "%s", Current.c_str());
                        if (ImGui::InputText("##v", Buffer, sizeof(Buffer)))
                        {
                            Current = Buffer;
                            bChanged = true;
                        }
                        break;
                    }
                }
                if (bChanged && PreviewContext != nullptr)
                {
                    Scalar.Value = Current;
                    DesignOverrides[DesignMemberKey(Model.Name, Scalar.Name)] = Current;
                    if (ObjectModel != nullptr)
                    {
                        RmlUi::SetObjectModelValue(ObjectModel, Member, FStringView(Current.c_str(), Current.size()));
                    }
                    else
                    {
                        RmlUi::SetEditorDesignValue(PreviewContext, ModelName, Member, FStringView(Current.c_str(), Current.size()));
                    }
                }

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(LE_ICON_ARROW_LEFT_BOTTOM))
                {
                    const std::string Snippet = "{{ " + std::string(Scalar.Name.c_str()) + " }}";
                    InsertSnippet(Snippet.c_str());
                }
                ImGuiX::TextTooltip("Insert {{ {} }} at the code cursor", Scalar.Name.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        for (size_t Index = 0; Index < Model.Lists.size(); ++Index)
        {
            FUIDesignList& List = Model.Lists[Index];
            ImGui::PushID(1000 + (int)Index);
            ImGui::AlignTextToFramePadding();
            ImGui::Text(LE_ICON_FORMAT_LIST_BULLETED " %s", List.Name.c_str());
            ImGui::SameLine();
            int Rows = (int)List.Rows.size();
            ImGui::SetNextItemWidth(90.0f);
            const bool bClassBacked = ModelIndex < (int32)DesignObjectModels.size() && DesignObjectModels[ModelIndex] != nullptr;
            if (ImGui::InputInt("rows", &Rows))
            {
                Rows = std::clamp(Rows, 0, 64);
                DesignRowCounts[DesignMemberKey(Model.Name, List.Name)] = Rows;
                if (bClassBacked)
                {
                    // The instance's own array is what the preview repeats, so it is rebuilt once the panel is drawn.
                    bDesignReloadPending = true;
                }
                else
                {
                    ResizeDesignRows(List, Rows);
                    if (PreviewContext != nullptr)
                    {
                        RmlUi::SetEditorDesignRows(PreviewContext, ModelName, FStringView(List.Name.c_str(), List.Name.size()), List.Rows);
                    }
                }
            }
            ImGuiX::TextTooltip("How many items the preview repeats the data-for element for.");
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_ARROW_LEFT_BOTTOM))
            {
                InsertSnippet(DesignListSnippet(List).c_str());
            }
            ImGuiX::TextTooltip("Insert a data-for element showing every member at the code cursor");

            std::string Members;
            for (const FString& Member : List.Members)
            {
                Members += (Members.empty() ? "" : ", ") + std::string(Member.c_str());
            }
            if (Members.empty())
            {
                ImGui::TextColored(kDesignDimColor, "    each item is the value itself");
            }
            else
            {
                ImGui::TextColored(kDesignDimColor, "    item.%s", Members.c_str());
            }
            ImGui::PopID();
        }

        for (size_t Index = 0; Index < Model.Commands.size(); ++Index)
        {
            const FUIDesignCommand& Command = Model.Commands[Index];
            ImGui::PushID(2000 + (int)Index);
            const bool bJustRan = !FiredCommands.empty() && FiredCommands.back().rfind(Command.Name + "(", 0) == 0;
            std::string Params;
            for (size_t Param = 0; Param < Command.Params.size(); ++Param)
            {
                Params += (Param > 0 ? ", " : "") + std::string(Command.Params[Param].c_str());
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(bJustRan ? ImVec4(0.55f, 0.90f, 0.60f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                LE_ICON_FUNCTION " %s(%s)", Command.Name.c_str(), Params.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_ARROW_LEFT_BOTTOM))
            {
                InsertSnippet(DesignCommandSnippet(Command).c_str());
            }
            ImGuiX::TextTooltip("Insert data-event-click=\"{}(...)\" at the code cursor", Command.Name.c_str());
            ImGui::PopID();
        }

        ImGui::PopID();
    }

    void FRmlUiEditorTool::DrawElementBindings()
    {
        if (SelectedOpenLt == ~size_t(0))
        {
            return;
        }

        TVector<RmlBinding::FAttribute> Attributes;
        RmlBinding::ReadDataAttributes(std::string_view(CompAssignText.data(), CompAssignText.size()), SelectedOpenLt, Attributes);
        if (Attributes.empty())
        {
            return;
        }

        ImGui::Spacing();
        ImGui::TextDisabled(LE_ICON_LINK_VARIANT " Bindings");
        const FUIDesignModel* Model = DesignModels.empty() ? nullptr : &DesignModels.front();
        for (const RmlBinding::FAttribute& Attribute : Attributes)
        {
            ImGui::TextColored(kDesignDimColor, "%s", Attribute.Name.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(Attribute.Value.c_str());

            // A bare member name shows what the preview currently feeds it.
            if (Model != nullptr && PreviewContext != nullptr && Model->FindScalar(FStringView(Attribute.Value.c_str(), Attribute.Value.size())) != nullptr)
            {
                const FStringView Member(Attribute.Value.c_str(), Attribute.Value.size());
                const FString Current = (!DesignObjectModels.empty() && DesignObjectModels.front() != nullptr)
                    ? RmlUi::GetObjectModelValue(DesignObjectModels.front(), Member)
                    : RmlUi::GetEditorDesignValue(PreviewContext, FStringView(Model->Name.c_str(), Model->Name.size()), Member);
                ImGui::SameLine();
                ImGui::TextColored(kDesignInfoColor, "= %s", Current.c_str());
            }
        }
    }
}
