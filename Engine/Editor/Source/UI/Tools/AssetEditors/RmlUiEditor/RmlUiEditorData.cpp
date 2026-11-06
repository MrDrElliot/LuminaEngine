#include "RmlUiEditorTool.h"

#include <algorithm>
#include <string>
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "UI/UIScript.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/RmlUiBridge.h"
#include "Core/Reflection/PropertyText.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Containers/StringFormat.h"
#include "FileSystem/FileSystem.h"
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
            Count = Math::Max(0, Count);
            while (List.Rows.Num() > Count)
            {
                List.Rows.Pop();
            }
            while (List.Rows.Num() < Count)
            {
                TVector<FString>& Cells = List.Rows.EmplaceGetRef();
                for (const FString& Member : List.Members)
                {
                    Cells.Add(Member + " " + std::to_string(List.Rows.Num()).c_str());
                }
                if (List.Members.IsEmpty())
                {
                    Cells.Add(List.Name + " " + std::to_string(List.Rows.Num()).c_str());
                }
            }
        }

        constexpr int32 kPlaceholderRows = 3;

        // Text members read "Name 2" and numbers take the row number, so each repeated item is told apart in the preview.
        void FillPlaceholderItem(const FProperty* Inner, void* Item, int32 Row, const FString& ListName)
        {
            auto Fill = [Row](const FProperty* Field, void* Container, const char* Label)
            {
                const EPropertyTypeFlags Type = Field->GetType();
                const std::string Text = (Type == EPropertyTypeFlags::String || Type == EPropertyTypeFlags::Name)
                    ? std::string(Label) + " " + std::to_string(Row)
                    : std::to_string(Row);
                if (Type != EPropertyTypeFlags::Bool)
                {
                    Reflection::FromText(Field, Container, FStringView(Text.c_str(), IntCastChecked<int32>(Text.size())));
                }
            };

            if (Inner->GetType() == EPropertyTypeFlags::Struct)
            {
                for (const FProperty* Member : static_cast<const FStructProperty*>(Inner)->GetStruct()->GetProperties())
                {
                    Fill(Member, Item, *Member->GetPropertyName());
                }
            }
            else
            {
                Fill(Inner, Item, *ListName);
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
                const int32 Existing = Array->Num(Items);
                const int32* Count = RowCounts.Find(DesignMemberKey(ModelName, Bound.Name));
                const int32 Wanted = Count != nullptr ? *Count : (Existing == 0 ? kPlaceholderRows : Existing);
                Array->SetNum(Items, Wanted);
                for (int32 Row = Existing; Row < Wanted; ++Row)
                {
                    FillPlaceholderItem(Array->GetInternalProperty(), Array->GetAt(Items, Row), Row + 1, Bound.Name);
                }
            }
        }

        std::string DesignListSnippet(const FUIDesignList& List)
        {
            std::string Snippet = "<div data-for=\"item : " + std::string(*List.Name) + "\">";
            for (const FString& Member : List.Members)
            {
                Snippet += "{{ item." + std::string(*Member) + " }} ";
            }
            if (!List.Members.IsEmpty())
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
            std::string Snippet = "data-event-click=\"" + std::string(*Command.Name) + "(";
            for (int32 Index = 0; Index < Command.Params.Num(); ++Index)
            {
                Snippet += (Index > 0 ? ", " : "") + std::string("'") + *Command.Params[Index] + "'";
            }
            return Snippet + ")\"";
        }
    }


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
        DesignObjectModels.Reset();
        DesignObjects.Reset();
    }

    void FRmlUiEditorTool::RefreshDesignModels(const std::string& Body)
    {
        RmlBinding::Scan(Body, Bindings);

        ReleaseDesignObjects();
        DesignModels.Reset();
        DesignDescribed.Reset();

        TVector<FUIDesignModel> Inferred;
        for (const FString& Name : Bindings.Models)
        {
            const bool bSeen = Algo::Contains(DesignModels, Name, &FUIDesignModel::Name);
            if (bSeen || Name.IsEmpty())
            {
                continue;
            }

            // A transient instance of the class, bound the way the game binds it, so the preview shows the class's own defaults.
            FUIDesignModel Model;
            CObject* Preview = nullptr;
            if (CClass* Class = UIBinding::FindModelClass(FStringView(*Name, Name.Len())))
            {
                Preview = NewObject(Class, nullptr, NAME_None, FGuid::New(), OF_Transient);
            }

            if (Preview != nullptr)
            {
                for (auto& [Key, Value] : DesignOverrides)
                {
                    const FString Prefix = Name + ".";
                    if (Key.FindLast(Prefix, 0) == 0)
                    {
                        void* Container = nullptr;
                        const FStringView Member(*Key + Prefix.Len(), Key.Len() - Prefix.Len());
                        if (const FProperty* Property = UIBinding::ResolvePath(Preview, Member, Container))
                        {
                            Reflection::FromText(Property, Container, FStringView(*Value, Value.Len()));
                        }
                    }
                }
                RmlDataPanel::FillPreviewLists(Preview, Name, DesignRowCounts);
                UIBinding::Describe(Preview, Model);
                Model.Name = Name;
            }
            else
            {
                Model = RmlBinding::Infer(Name, Bindings);
                for (FUIDesignScalar& Scalar : Model.Scalars)
                {
                    auto Override = DesignOverrides.Find(RmlDataPanel::DesignMemberKey(Model.Name, Scalar.Name));
                    if (Override != nullptr)
                    {
                        Scalar.Value = (*Override);
                    }
                }
                for (FUIDesignList& List : Model.Lists)
                {
                    auto Count = DesignRowCounts.Find(RmlDataPanel::DesignMemberKey(Model.Name, List.Name));
                    if (Count != nullptr)
                    {
                        RmlDataPanel::ResizeDesignRows(List, (*Count));
                    }
                }
                Inferred.Add(Model);
            }

            DesignModels.Add(Move(Model));
            DesignDescribed.Add(Preview != nullptr);
            DesignObjects.Emplace(Preview);
            DesignObjectModels.Add(nullptr);
        }

        RmlBinding::Lint(Bindings, DesignModels, DesignDescribed, BindingProblems);
        ApplyBindingMarkers();

        if (PreviewContext != nullptr)
        {
            RmlUi::SetEditorDesignModels(PreviewContext, Inferred);
            for (int32 Index = 0; Index < DesignObjects.Num(); ++Index)
            {
                if (CObject* Preview = DesignObjects[Index].Get())
                {
                    const FString& Name = DesignModels[Index].Name;
                    DesignObjectModels[Index] = RmlUi::CreateObjectModel(PreviewContext, FStringView(*Name, Name.Len()), Preview, true);
                }
            }
        }
        UIBinding::EnumerateModelClasses(AvailableModels);
        bScriptsReloaded = false;
    }

    void FRmlUiEditorTool::ApplyBindingMarkers()
    {
        TVector<FCodeDiagnostic> Diagnostics;
        Diagnostics.Reserve(BindingProblems.Num());
        for (const RmlBinding::FProblem& Problem : BindingProblems)
        {
            FCodeDiagnostic& Diagnostic = Diagnostics.EmplaceGetRef();
            Diagnostic.Range = { { Problem.Line - 1, 0 }, { Problem.Line, 0 } };
            Diagnostic.Message = Problem.Message;
            Diagnostic.Severity = Problem.bError ? ECodeSeverity::Error : ECodeSeverity::Warning;
        }
        CodeEditor.SetExternalDiagnostics(Move(Diagnostics));
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
            ImGuiX::Notifications::NotifyInfo("Preview ran {}", *Call);
            FiredCommands.Add(Move(Call));
        }
        if (FiredCommands.Num() > RmlDataPanel::kMaxPreviewCommands)
        {
            FiredCommands.RemoveAt(0, FiredCommands.Num() - RmlDataPanel::kMaxPreviewCommands);
        }

        if (bScriptsReloaded)
        {
            ReloadDocument();
        }
    }

    void FRmlUiEditorTool::JumpToLine(int32 Line)
    {
        CodeEditor.SetCursor({ Math::Clamp(Line, 1, CodeEditor.GetLineCount()) - 1, 0 });
        CodeEditor.Focus();
    }

    void FRmlUiEditorTool::ConfigureAutoComplete()
    {
        Language.SetStylesheetResolver([this](FStringView Href, FString& OutText)
        {
            const FString Path = Href.StartsWith("/") ? FString(Href) : ParentDir + "/" + FString(Href);
            return VFS::ReadFile(OutText, Path.View());
        });
        if (bIsStylesheet)
        {
            return;
        }

        // The language completes data-* attribute names itself, so the model only answers inside binding expressions.
        FCodeCompletionSource Source;
        Source.IsTrigger = [](char Typed, const FCodeCompletionRequest& Request)
        {
            const FStringView Prefix = Request.LinePrefix.Mid(0, Request.WordStart.Column);
            const RmlBinding::ECompletion Kind = RmlBinding::ClassifyCompletion(std::string_view(Prefix.GetData(), Prefix.Len())).Kind;
            return Kind != RmlBinding::ECompletion::None && Kind != RmlBinding::ECompletion::Attribute;
        };
        Source.Gather = [this](const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out)
        {
            GatherBindingCompletions(Request, Out);
        };
        CodeEditor.SetCompletionSource(Move(Source));
    }

    void FRmlUiEditorTool::GatherBindingCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out)
    {
        const FStringView Prefix = Request.LinePrefix.Mid(0, Request.WordStart.Column);
        const RmlBinding::FCompletionContext Context = RmlBinding::ClassifyCompletion(std::string_view(Prefix.GetData(), Prefix.Len()));

        auto Add = [&Out](const FString& Name, ECodeCompletionKind Kind, FString Detail, FString Insert, int32 Boost)
        {
            FCodeCompletionItem& Item = Out.EmplaceGetRef();
            Item.Label = Name;
            Item.Kind = Kind;
            Item.Detail = Move(Detail);
            Item.InsertText = Move(Insert);
            Item.SortBoost = Boost;
        };

        switch (Context.Kind)
        {
            case RmlBinding::ECompletion::Model:
            {
                for (const FUIModelInfo& Info : AvailableModels)
                {
                    Add(Info.Name, ECodeCompletionKind::Module, Format("{} {}", Info.bScript ? "UIScript" : "ViewModel", *Info.TypeName), {}, 20);
                }
                break;
            }
            case RmlBinding::ECompletion::AliasMember:
            {
                for (const RmlBinding::FForScope& Scope : Bindings.ForScopes)
                {
                    if (Scope.Alias != Context.Alias)
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
                                Add(Member, ECodeCompletionKind::Field, Format("item of {}", *List.Name), {}, 20);
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
                            FString Params;
                            for (int32 Index = 0; Index < Command.Params.Num(); ++Index)
                            {
                                Params += Index > 0 ? ", " : "";
                                Params += Command.Params[Index];
                            }
                            Add(Command.Name, ECodeCompletionKind::Function, "(" + Params + ")", Command.Params.IsEmpty() ? Command.Name + "()" : Command.Name + "($0)", 20);
                        }
                    }
                    for (const FUIDesignList& List : Model.Lists)
                    {
                        Add(List.Name, ECodeCompletionKind::Field, Format("list of {} members", List.Members.Num()), {}, 10);
                    }
                    if (Context.Kind == RmlBinding::ECompletion::ListSource)
                    {
                        continue;
                    }
                    for (const FUIDesignScalar& Scalar : Model.Scalars)
                    {
                        Add(Scalar.Name, ECodeCompletionKind::Variable, Format("{} = {}", RmlDataPanel::DesignTypeLabel(Scalar.Type), *Scalar.Value), {}, 15);
                    }
                }
                for (const RmlBinding::FForScope& Scope : Bindings.ForScopes)
                {
                    Add(Scope.Alias, ECodeCompletionKind::Variable, Format("each item of {}", *Scope.List), {}, 15);
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
        ImGui::BeginDisabled(DesignOverrides.IsEmpty() && DesignRowCounts.IsEmpty());
        if (ImGui::SmallButton(LE_ICON_RESTORE " Defaults"))
        {
            DesignOverrides.Reset();
            DesignRowCounts.Reset();
            ReloadDocument();
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("Forget the preview values typed here and go back to the C# defaults.");
        ImGui::Separator();

        if (!BindingProblems.IsEmpty())
        {
            RmlDataPanel::DataSectionHeader(LE_ICON_ALERT_CIRCLE_OUTLINE, "Problems");
            for (int32 Index = 0; Index < BindingProblems.Num(); ++Index)
            {
                const RmlBinding::FProblem& Problem = BindingProblems[Index];
                ImGui::PushID((int)Index);
                ImGui::PushStyleColor(ImGuiCol_Text, Problem.bError ? RmlDataPanel::kDesignErrorColor : RmlDataPanel::kDesignInfoColor);
                char Label[48];
                std::snprintf(Label, sizeof(Label), "%s Line %d", Problem.bError ? LE_ICON_CLOSE_CIRCLE : LE_ICON_INFORMATION, Problem.Line);
                if (ImGui::Selectable(Label, false, ImGuiSelectableFlags_AllowOverlap))
                {
                    JumpToLine(Problem.Line);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", *Problem.Message);
                ImGui::PopID();
            }
            ImGui::Spacing();
        }

        if (DesignModels.IsEmpty())
        {
            RmlDataPanel::DataSectionHeader(LE_ICON_DATABASE_OUTLINE, "No data model");
            ImGui::TextWrapped("This document binds no data. Put data-model=\"Name\" on its body, where Name is a class with Bind members, "
                "a UIScript or CUIScript for instance, or the name its DataModel gives. Then use {{ Member }} and the data-* attributes inside it.");
            if (!AvailableModels.IsEmpty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Models in your scripts");
                for (int32 Index = 0; Index < AvailableModels.Num(); ++Index)
                {
                    const FUIModelInfo& Info = AvailableModels[Index];
                    ImGui::PushID((int)Index);
                    if (ImGui::SmallButton(LE_ICON_CONTENT_COPY))
                    {
                        const std::string Attribute = "data-model=\"" + std::string(*Info.Name) + "\"";
                        ImGui::SetClipboardText(Attribute.c_str());
                    }
                    ImGuiX::TextTooltip("Copy data-model=\"{}\"", *Info.Name);
                    ImGui::SameLine();
                    ImGui::Text("%s", *Info.Name);
                    ImGui::SameLine();
                    ImGui::TextColored(RmlDataPanel::kDesignDimColor, "%s %s", Info.bScript ? "UI script" : "Bound class", *Info.TypeName);
                    ImGui::PopID();
                }
            }
        }

        for (int32 Index = 0; Index < DesignModels.Num(); ++Index)
        {
            DrawModelSection(Index);
        }
        if (bDesignReloadPending)
        {
            bDesignReloadPending = false;
            ReloadDocument();
        }

        if (!FiredCommands.IsEmpty())
        {
            ImGui::Spacing();
            RmlDataPanel::DataSectionHeader(LE_ICON_GESTURE_TAP, "Commands the preview ran");
            for (int32 Index = FiredCommands.Num(); Index > 0; --Index)
            {
                ImGui::TextColored(Index == FiredCommands.Num() ? ImVec4(0.55f, 0.90f, 0.60f, 1.0f) : RmlDataPanel::kDesignDimColor, "%s", *FiredCommands[Index - 1]);
            }
            if (ImGui::SmallButton("Clear"))
            {
                FiredCommands.Reset();
            }
        }
    }

    void FRmlUiEditorTool::DrawModelSection(int32 ModelIndex)
    {
        FUIDesignModel& Model = DesignModels[ModelIndex];
        const bool bDescribed = ModelIndex < DesignDescribed.Num() && DesignDescribed[ModelIndex];

        ImGui::PushID(ModelIndex);
        char Header[160];
        std::snprintf(Header, sizeof(Header), LE_ICON_DATABASE " %s", *Model.Name);
        if (!ImGui::CollapsingHeader(Header, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PopID();
            return;
        }

        if (bDescribed)
        {
            ImGui::TextColored(RmlDataPanel::kDesignDimColor, "%s %s", Model.bFromScript ? "UI script" : "Bound class", *Model.SourceType);
        }
        else
        {
            ImGui::TextColored(RmlDataPanel::kDesignInfoColor, "Inferred from this document. No class registers '%s' yet.", *Model.Name);
        }
        ImGui::TextColored(RmlDataPanel::kDesignDimColor, "Values here only feed the preview. Insert puts the binding at the code cursor.");

        const FStringView ModelName(*Model.Name, Model.Name.Len());
        if (!Model.Scalars.IsEmpty() && ImGui::BeginTable("##scalars", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("##insert", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            for (int32 Index = 0; Index < Model.Scalars.Num(); ++Index)
            {
                FUIDesignScalar& Scalar = Model.Scalars[Index];
                ImGui::PushID((int)Index);
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(*Scalar.Name);
                ImGuiX::TextTooltip("{} {}{}", RmlDataPanel::DesignTypeLabel(Scalar.Type), *Scalar.Name, Scalar.bWritable ? ", two-way" : ", display only");

                // Read back each frame, since a two-way control in the preview can change it.
                const FStringView Member(*Scalar.Name, Scalar.Name.Len());
                void* ObjectModel = ModelIndex < DesignObjectModels.Num() ? DesignObjectModels[ModelIndex] : nullptr;
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
                        int Value = std::atoi(*Current);
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
                        float Value = (float)std::atof(*Current);
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
                        std::snprintf(Buffer, sizeof(Buffer), "%s", *Current);
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
                    DesignOverrides[RmlDataPanel::DesignMemberKey(Model.Name, Scalar.Name)] = Current;
                    if (ObjectModel != nullptr)
                    {
                        RmlUi::SetObjectModelValue(ObjectModel, Member, FStringView(*Current, Current.Len()));
                    }
                    else
                    {
                        RmlUi::SetEditorDesignValue(PreviewContext, ModelName, Member, FStringView(*Current, Current.Len()));
                    }
                }

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(LE_ICON_ARROW_LEFT_BOTTOM))
                {
                    const std::string Snippet = "{{ " + std::string(*Scalar.Name) + " }}";
                    InsertSnippet(Snippet.c_str());
                }
                ImGuiX::TextTooltip("Insert {{ {} }} at the code cursor", *Scalar.Name);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        for (int32 Index = 0; Index < Model.Lists.Num(); ++Index)
        {
            FUIDesignList& List = Model.Lists[Index];
            ImGui::PushID(1000 + (int)Index);
            ImGui::AlignTextToFramePadding();
            ImGui::Text(LE_ICON_FORMAT_LIST_BULLETED " %s", *List.Name);
            ImGui::SameLine();
            int Rows = List.Rows.Num();
            ImGui::SetNextItemWidth(90.0f);
            const bool bClassBacked = ModelIndex < DesignObjectModels.Num() && DesignObjectModels[ModelIndex] != nullptr;
            if (ImGui::InputInt("rows", &Rows))
            {
                Rows = std::clamp(Rows, 0, 64);
                DesignRowCounts[RmlDataPanel::DesignMemberKey(Model.Name, List.Name)] = Rows;
                if (bClassBacked)
                {
                    // The instance's own array is what the preview repeats, so it is rebuilt once the panel is drawn.
                    bDesignReloadPending = true;
                }
                else
                {
                    RmlDataPanel::ResizeDesignRows(List, Rows);
                    if (PreviewContext != nullptr)
                    {
                        RmlUi::SetEditorDesignRows(PreviewContext, ModelName, FStringView(*List.Name, List.Name.Len()), List.Rows);
                    }
                }
            }
            ImGuiX::TextTooltip("How many items the preview repeats the data-for element for.");
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_ARROW_LEFT_BOTTOM))
            {
                InsertSnippet(RmlDataPanel::DesignListSnippet(List).c_str());
            }
            ImGuiX::TextTooltip("Insert a data-for element showing every member at the code cursor");

            std::string Members;
            for (const FString& Member : List.Members)
            {
                Members += (Members.empty() ? "" : ", ") + std::string(*Member);
            }
            if (Members.empty())
            {
                ImGui::TextColored(RmlDataPanel::kDesignDimColor, "    each item is the value itself");
            }
            else
            {
                ImGui::TextColored(RmlDataPanel::kDesignDimColor, "    item.%s", Members.c_str());
            }
            ImGui::PopID();
        }

        for (int32 Index = 0; Index < Model.Commands.Num(); ++Index)
        {
            const FUIDesignCommand& Command = Model.Commands[Index];
            ImGui::PushID(2000 + (int)Index);
            const bool bJustRan = !FiredCommands.IsEmpty() && FiredCommands.Last().FindLast(Command.Name + "(", 0) == 0;
            std::string Params;
            for (int32 Param = 0; Param < Command.Params.Num(); ++Param)
            {
                Params += (Param > 0 ? ", " : "") + std::string(*Command.Params[Param]);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(bJustRan ? ImVec4(0.55f, 0.90f, 0.60f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                LE_ICON_FUNCTION " %s(%s)", *Command.Name, Params.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_ARROW_LEFT_BOTTOM))
            {
                InsertSnippet(RmlDataPanel::DesignCommandSnippet(Command).c_str());
            }
            ImGuiX::TextTooltip("Insert data-event-click=\"{}(...)\" at the code cursor", *Command.Name);
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
        if (Attributes.IsEmpty())
        {
            return;
        }

        ImGui::Spacing();
        ImGui::TextDisabled(LE_ICON_LINK_VARIANT " Bindings");
        const FUIDesignModel* Model = DesignModels.IsEmpty() ? nullptr : &DesignModels[0];
        for (const RmlBinding::FAttribute& Attribute : Attributes)
        {
            ImGui::TextColored(RmlDataPanel::kDesignDimColor, "%s", *Attribute.Name);
            ImGui::SameLine();
            ImGui::TextUnformatted(*Attribute.Value);

            // A bare member name shows what the preview currently feeds it.
            if (Model != nullptr && PreviewContext != nullptr && Model->FindScalar(FStringView(*Attribute.Value, Attribute.Value.Len())) != nullptr)
            {
                const FStringView Member(*Attribute.Value, Attribute.Value.Len());
                const FString Current = (!DesignObjectModels.IsEmpty() && DesignObjectModels[0] != nullptr)
                    ? RmlUi::GetObjectModelValue(DesignObjectModels[0], Member)
                    : RmlUi::GetEditorDesignValue(PreviewContext, FStringView(*Model->Name, Model->Name.Len()), Member);
                ImGui::SameLine();
                ImGui::TextColored(RmlDataPanel::kDesignInfoColor, "= %s", *Current);
            }
        }
    }
}
