#include "EntityScriptComponentCustomization.h"
#include "UI/Properties/Customizations/AssetPickerFilter.h"

#include "imgui.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "UI/Properties/PropertyTable.h"
#include "Session/SessionOps.h"
#include "UI/Tools/EditorToolContext.h"
#include "UI/Widgets/EntityScriptPicker.h"

namespace Lumina
{
    namespace
    {
        constexpr const char* GScriptIcon = LE_ICON_LANGUAGE_CSHARP;

        /** "Game.ZombieFPS.ZombieBrain" -> "ZombieBrain" plus "Game.ZombieFPS", so the name can lead. */
        void SplitTypeName(const FString& Full, FStringView& OutShort, FStringView& OutNamespace)
        {
            const FStringView View(Full);
            const int32 Dot = View.FindLastOf('.');
            if (Dot == Constants::kIndexNone)
            {
                OutShort = View;
                OutNamespace = FStringView();
                return;
            }
            OutShort = View.Mid(Dot + 1);
            OutNamespace = View.Mid(0, Dot);
        }
    }

    namespace
    {
        // Names the component being dragged within, so a slot dropped on another entity's script list is ignored.
        struct FScriptSlotPayload
        {
            const void* Component = nullptr;
            int32       Index     = 0;
        };

        constexpr const char* kScriptSlotPayload = "LUMINA_SCRIPT_SLOT";
    }

    TSharedPtr<FEntityScriptComponentCustomization> FEntityScriptComponentCustomization::MakeInstance()
    {
        return MakeShared<FEntityScriptComponentCustomization>();
    }

    void FEntityScriptComponentCustomization::QueueMove(SEntityScriptComponent* Component, int32 From, int32 To)
    {
        PendingMutation = [this, Component, From, To]
        {
            const int32 Count = Component->Scripts.Num();
            if (From == To || From < 0 || To < 0 || From >= Count || To >= Count)
            {
                return;
            }

            TStrongObjectPtr<CEntityScript> Moved = Move(Component->Scripts[From]);
            Component->Scripts.RemoveAt(From);
            Component->Scripts.Insert(Move(Moved), To);

            // The views travel with their scripts, so an open table keeps its state across the move.
            if (SlotViews.Num() == Count)
            {
                FSlotView View = Move(SlotViews[From]);
                SlotViews.RemoveAt(From);
                SlotViews.Insert(Move(View), To);
            }
        };
    }

    void FEntityScriptComponentCustomization::BindView(FSlotView& View, CEntityScript* Script)
    {
        // The class is compared too, since a new script can land at a destroyed one's address.
        if (View.BoundScript == Script && View.BoundClass == Script->GetClass())
        {
            return;
        }

        View.ValueTable = MakeUnique<FPropertyTable>(static_cast<void*>(Script), static_cast<CStruct*>(Script->GetClass()));
        View.ValueTable->SetShowSearchBar(false);

        // Routed out as this customization's op, so a script edit shares the panel's transaction handling.
        View.ValueTable->SetStartEditCallback ([this](const FPropertyChangedEvent&) { NestedChangeOp = EPropertyChangeOp::Started; });
        View.ValueTable->SetPostEditCallback  ([this](const FPropertyChangedEvent&)
        {
            if (NestedChangeOp == EPropertyChangeOp::None) { NestedChangeOp = EPropertyChangeOp::Updated; }
        });
        View.ValueTable->SetFinishEditCallback([this](const FPropertyChangedEvent&) { NestedChangeOp = EPropertyChangeOp::Finished; });

        View.Buttons.Reset();
        for (const FFunction* Function : Script->GetClass()->GetFunctions())
        {
            if (Function->HasMetadata("Button") && Function->GetArguments().IsEmpty())
            {
                View.Buttons.Add(Function);
            }
        }
        View.SourceFile = Script->GetClass()->HasMeta("SourceFile")
            ? Script->GetClass()->GetMeta("SourceFile")
            : DotNet::FindScriptSourceFile(Script->GetClass()->GetName().ToString());

        View.BoundScript = Script;
        View.BoundClass = Script->GetClass();
    }

    bool FEntityScriptComponentCustomization::ApplySearch(const TSharedPtr<FPropertyHandle>& Property, FStringView Text)
    {
        const auto* Component = static_cast<SEntityScriptComponent*>(Property->ContainerPtr);
        SlotViews.SetNum(Component->Scripts.Num());

        bool bAnyPasses = false;
        for (int32 Index = 0; Index < Component->Scripts.Num(); ++Index)
        {
            CEntityScript* Script = Component->Scripts[Index].Get();
            FSlotView& View = SlotViews[Index];
            if (Text.IsEmpty() || Script == nullptr || Script->GetClass() == nullptr)
            {
                View.bPassesSearch = Text.IsEmpty();
                if (View.ValueTable)
                {
                    View.ValueTable->SetSearchText(FStringView());
                }
            }
            else
            {
                BindView(View, Script);
                View.bPassesSearch = View.ValueTable->MatchesSearch(Text, *Script->GetClass()->GetName().ToString());
            }
            bAnyPasses |= View.bPassesSearch;
        }
        return Text.IsEmpty() || bAnyPasses;
    }

    EPropertyChangeOp FEntityScriptComponentCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bool bWasChanged = false;
        auto* Component = static_cast<SEntityScriptComponent*>(Property->ContainerPtr);

        ImGui::PushID(this);

        SlotViews.SetNum(Component->Scripts.Num());

        for (int32 Index = 0; Index < Component->Scripts.Num(); ++Index)
        {
            CEntityScript* Script = Component->Scripts[Index].Get();
            FSlotView& View = SlotViews[Index];
            if (!View.bPassesSearch)
            {
                continue;
            }
            ImGui::PushID(Index);

            const bool bValid = Script != nullptr && Script->GetClass() != nullptr;
            const FString TypeName = bValid ? Script->GetClass()->GetName().ToString() : FString();

            FStringView Short;
            FStringView Namespace;
            SplitTypeName(TypeName, Short, Namespace);

            const FString HeaderLabel = bValid
                ? Lumina::Format("{} {}", GScriptIcon, FString(Short.GetData(), Short.Len()))
                : FString(LE_ICON_ALERT " (missing script)");

            const float Spacing     = ImGui::GetStyle().ItemSpacing.x;
            const float HeaderRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            const float ButtonsLeft = HeaderRight - AssetPickerFilter::kButtonSize.x * 5.0f - Spacing * 4.0f;

            // Breathing room around the title, which is what stops the stack of scripts reading as one block.
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f));
            ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_Header, EditorColors::ComponentHeader());
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorColors::Lighten(EditorColors::ComponentHeader(), 0.05f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorColors::Lighten(EditorColors::ComponentHeader(), 0.08f));
            // AllowOverlap so the buttons get their own clicks instead of the header toggling collapse.
            const bool bOpen = ImGui::CollapsingHeader(*(HeaderLabel + "##scripthdr"), ImGuiTreeNodeFlags_AllowOverlap);
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();

            // Dragging one header onto another moves that script into its slot.
            if (ImGui::BeginDragDropSource())
            {
                const FScriptSlotPayload Payload{ Component, Index };
                ImGui::SetDragDropPayload(kScriptSlotPayload, &Payload, sizeof(Payload));
                ImGui::TextUnformatted(*HeaderLabel);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* Dropped = ImGui::AcceptDragDropPayload(kScriptSlotPayload))
                {
                    const FScriptSlotPayload& Payload = *static_cast<const FScriptSlotPayload*>(Dropped->Data);
                    if (Payload.Component == Component && Payload.Index != Index)
                    {
                        QueueMove(Component, Payload.Index, Index);
                        bWasChanged = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // The namespace trails the name in the header's own row, dimmed, so it informs without competing.
            if (!Namespace.IsEmpty())
            {
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
                ImGui::TextDisabled("%.*s", Namespace.Len(), Namespace.GetData());
            }

            ImGui::SameLine(ButtonsLeft);
            ImGui::BeginDisabled(Index == 0);
            if (ImGui::Button(LE_ICON_ARROW_UP "##MoveEntityScriptUp", AssetPickerFilter::kButtonSize))
            {
                QueueMove(Component, Index, Index - 1);
                bWasChanged = true;
            }
            ImGui::EndDisabled();
            ImGuiX::TextTooltip("{}", "Move up. Scripts attach, ready and update top to bottom.");

            ImGui::SameLine();
            ImGui::BeginDisabled(Index + 1 >= Component->Scripts.Num());
            if (ImGui::Button(LE_ICON_ARROW_DOWN "##MoveEntityScriptDown", AssetPickerFilter::kButtonSize))
            {
                QueueMove(Component, Index, Index + 1);
                bWasChanged = true;
            }
            ImGui::EndDisabled();
            ImGuiX::TextTooltip("{}", "Move down. Scripts attach, ready and update top to bottom.");

            ImGui::SameLine();
            ImGui::BeginDisabled(View.SourceFile.IsEmpty());
            if (ImGui::Button(LE_ICON_FILE_CODE "##OpenEntityScriptSource", AssetPickerFilter::kButtonSize))
            {
                // The Content Browser's route, which honors the script editor chosen in the editor settings.
                if (IEditorToolContext* Tools = SessionOps::GetToolContext())
                {
                    Tools->OpenScriptEditor(FStringView(*View.SourceFile, View.SourceFile.Len()));
                }
            }
            ImGui::EndDisabled();
            ImGuiX::TextTooltip("{}", View.SourceFile.IsEmpty() ? FString("No source file found for this script.") : Lumina::Format("Open {}", View.SourceFile));

            ImGui::SameLine();
            ImGui::BeginDisabled(!bValid);
            if (ImGui::Button(LE_ICON_CONTENT_COPY "##CopyEntityScriptName", AssetPickerFilter::kButtonSize))
            {
                ImGui::SetClipboardText(*TypeName);
            }
            ImGui::EndDisabled();
            ImGuiX::TextTooltip("{}", "Copy the script's type name.");

            ImGui::SameLine();
            if (AssetPickerFilter::DrawClearButton("Remove this script."))
            {
                PendingMutation = [Component, Index]
                {
                    if (Index < Component->Scripts.Num())
                    {
                        if (CEntityScript* Removed = Component->Scripts[Index].Get(); Removed != nullptr && Removed->IsAttached())
                        {
                            Removed->OnDetach();
                        }
                        Component->Scripts.RemoveAt(Index);
                    }
                };
                bWasChanged = true;
            }

            if (bOpen && bValid)
            {
                BindView(View, Script);
                if (View.ValueTable)
                {
                    View.ValueTable->DrawTree(Args);
                }

                // The instance is created on demand, so a script that has never been dispatched to still has
                // something to invoke against.
                if (!View.Buttons.IsEmpty())
                {
                    ImGui::Spacing();

                    const float Right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                    for (int32 ButtonIndex = 0; ButtonIndex < View.Buttons.Num(); ++ButtonIndex)
                    {
                        const FFunction& Button = *View.Buttons[ButtonIndex];
                        const FCStringView Label = Button.GetMetadata("Button");
                        const FString Text = Lumina::Format("{} {}", LE_ICON_PLAY_CIRCLE,
                            Label.IsEmpty() ? Button.GetFunctionName().ToString() : FString(Label.GetData(), Label.Len()));
                        const float Width = ImGui::CalcTextSize(*Text).x + ImGui::GetStyle().FramePadding.x * 2.0f;

                        // Wrapped by hand, since a row of actions should use the width it has and then break.
                        if (ButtonIndex > 0 && ImGui::GetCursorPosX() + Width < Right)
                        {
                            ImGui::SameLine();
                        }

                        if (ImGui::Button(*Text))
                        {
                            FFunctionFrame Frame(Button);
                            Frame.Invoke(Script);
                        }
                        if (const FCStringView Tooltip = Button.GetMetadata("ToolTip"); !Tooltip.IsEmpty() && ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", *Tooltip);
                        }
                    }
                }

                ImGui::Spacing();
            }

            ImGui::PopID();
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Filed by source folder, because a project accumulates far more script classes than a flat list stays usable for.
        ScriptPicker.SearchHint = "Search scripts...";
        ScriptPicker.ItemNoun   = "scripts";
        ScriptPicker.TrailingCategory = EntityScriptPicker::NativeCategory;
        ImGui::BeginGroup();
        const FPickerItem* Picked = ScriptPicker.DrawButton("##AddEntityScript", LE_ICON_PLUS " Add Script", &EntityScriptPicker::GatherItems);
        ImGui::EndGroup();

        CClass* Class = Picked != nullptr ? FindObject<CClass>(FName(Picked->Key)) : nullptr;

        // A .cs file dragged from the content browser adds the script it declares.
        if (ImGui::BeginDragDropTarget())
        {
            FFixedString ScriptPath;
            if (DragDrop::AcceptFile("cs", ScriptPath))
            {
                Class = EntityScriptPicker::FindScriptClassForSource(FStringView(*ScriptPath, ScriptPath.Len()));
                if (Class == nullptr)
                {
                    ImGuiX::Notifications::NotifyWarning("\"{}\" declares no entity script to add.", *ScriptPath);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (Class != nullptr)
        {
            PendingMutation = [Component, Class]
            {
                // The driver adopts an owner-less script next tick, the same path a deserialized script takes.
                if (CObject* Created = NewObject(Class, nullptr, NAME_None, FGuid::New(), OF_Transient))
                {
                    Component->Scripts.Add(static_cast<CEntityScript*>(Created));
                }
            };
            bWasChanged = true;
        }

        ImGui::PopID();

        if (bWasChanged)
        {
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            bFinishPending = true;
            return EPropertyChangeOp::Started;
        }
        if (bFinishPending)
        {
            bFinishPending = false;
            return EPropertyChangeOp::Finished;
        }

        // Reported after the add and remove ops, so a structural change always wins the frame.
        if (NestedChangeOp != EPropertyChangeOp::None)
        {
            const EPropertyChangeOp Op = NestedChangeOp;
            NestedChangeOp = EPropertyChangeOp::None;
            return Op;
        }
        return EPropertyChangeOp::None;
    }

    void FEntityScriptComponentCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        // Runs after BeginTransaction, so the undo snapshot captured the pre-change state.
        if (PendingMutation)
        {
            PendingMutation();
            PendingMutation = {};
        }
    }

    void FEntityScriptComponentCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        // Must stay cheap and idempotent, since UpdateAndDraw calls this on EVERY draw.
    }
}
