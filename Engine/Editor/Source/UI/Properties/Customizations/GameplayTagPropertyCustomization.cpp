#include "GameplayTagPropertyCustomization.h"

#include <EASTL/algorithm.h>

#include "imgui.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Config/Config.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/ObjectCore.h"
#include "GameplayTags/GameplayTagRegistry.h"
#include "GameplayTags/GameplayTagsSettings.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    EPropertyChangeOp FGameplayTagPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        bool bChanged = false;

        const ImGuiStyle& Style = ImGui::GetStyle();
        const float Scale      = ImGuiX::GetUIScale();
        const float LineHeight = ImGui::GetFrameHeight();
        const float Spacing    = Style.ItemInnerSpacing.x;

        // Closed preview mirrors the open list: tag icon + the dotted name (or "None").
        FFixedString Preview = LE_ICON_TAG "  ";
        Preview += Value.TagName.IsNone() ? "None" : Value.TagName.c_str();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - LineHeight - Spacing);

        // Keep the picker comfortably wide so deep dotted paths aren't cramped.
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * Scale, 0.0f), ImVec2(FLT_MAX, FLT_MAX));

        if (ImGui::BeginCombo("##gameplaytag", Preview.c_str(), ImGuiComboFlags_HeightLargest))
        {
            const float PopupWidth = ImGui::GetContentRegionAvail().x;

            if (ImGui::IsWindowAppearing())
            {
                TagFilter.Clear();
                TagTree.MarkTreeDirty();
                ImGui::SetKeyboardFocusHere();
            }

            // Search box. Any edit re-filters the tree on the next Draw.
            if (TagFilter.Draw("##filter", PopupWidth - ImGui::CalcTextSize("Clear").x - Style.FramePadding.x * 2.0f - Spacing))
            {
                TagTree.MarkTreeDirty();
            }
            if (!TagFilter.IsActive())
            {
                ImVec2 HintPos = ImGui::GetItemRectMin();
                HintPos.x += Style.FramePadding.x + 2.0f;
                HintPos.y += Style.FramePadding.y;
                ImGui::GetWindowDrawList()->AddText(HintPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), LE_ICON_MAGNIFY " Search...");
            }

            // Clear back to None.
            ImGui::SameLine(0.0f, Spacing);
            if (ImGui::Button("Clear"))
            {
                Value.TagName = FName();
                bChanged = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::Separator();

            FTreeListViewContext Context;
            Context.RebuildTreeFunction = [this](FTreeListView& Tree)
            {
                BuildTagTree(Tree);
            };
            Context.ItemSelectedFunction = [this, &bChanged](FTreeListView& Tree, FTreeNodeID Node, bool)
            {
                if (Tree.IsValid(Node))
                {
                    Value.TagName = FName(Tree.Get<FString>(Node).c_str());
                    bChanged = true;
                }
            };

            const float RowHeight = ImGui::GetFrameHeight();
            const float ListHeight = ImClamp(LastBuiltCount, 1, 14) * RowHeight + Style.FramePadding.y * 2.0f;

            ImGui::BeginChild("##tagtree", ImVec2(PopupWidth, ListHeight));
            TagTree.Draw(Context);
            ImGui::EndChild();

            if (LastBuiltCount == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextMuted());
                ImGui::TextUnformatted(TagFilter.IsActive() ? "No matching tags." : "No tags. Use + to add one.");
                ImGui::PopStyleColor();
            }

            // Selecting a tag closes the picker.
            if (bChanged)
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndCombo();
        }

        // "+" : author a new tag. Interns it, persists it to the project's tag settings, and selects it.
        ImGui::SameLine(0.0f, Spacing);
        if (ImGui::Button(LE_ICON_PLUS "##addtag", ImVec2(LineHeight, LineHeight)))
        {
            NewTagBuffer[0] = '\0';
            ImGui::OpenPopup("AddGameplayTag");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Add a new gameplay tag");
        }

        if (ImGui::BeginPopup("AddGameplayTag"))
        {
            ImGui::TextUnformatted("New tag (e.g. Ability.Fire.Fireball)");
            ImGui::SetNextItemWidth(280.0f);
            const bool bEnter = ImGui::InputText("##newtag", NewTagBuffer, sizeof(NewTagBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool bAdd = ImGui::Button("Add");

            if ((bEnter || bAdd) && NewTagBuffer[0] != '\0')
            {
                const FString NewTag(NewTagBuffer);
                FGameplayTagRegistry::Get().RequestTag(FStringView(NewTag.c_str(), NewTag.size()));

                CGameplayTagsSettings* Settings = GetMutableDefault<CGameplayTagsSettings>();
                if (Settings != nullptr && eastl::find(Settings->Tags.begin(), Settings->Tags.end(), NewTag) == Settings->Tags.end())
                {
                    Settings->Tags.push_back(NewTag);
                    if (GConfig != nullptr)
                    {
                        GConfig->SaveSettings(CGameplayTagsSettings::StaticClass());
                    }
                }

                Value.TagName = FName(NewTag.c_str());
                bChanged = true;
                TagTree.MarkTreeDirty();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        return bChanged ? EPropertyChangeOp::Updated : EPropertyChangeOp::None;
    }

    void FGameplayTagPropertyCustomization::BuildTagTree(FTreeListView& Tree)
    {
        LastBuiltCount = 0;

        TVector<FString> Tags;
        FGameplayTagRegistry::Get().GetAllTags(Tags);
        eastl::sort(Tags.begin(), Tags.end());

        // A path that is the ancestor of another tag is a "category" (folder); the rest are leaves.
        THashSet<FString> CategoryPaths;
        for (const FString& Tag : Tags)
        {
            size_t Dot = Tag.find('.');
            while (Dot != FString::npos)
            {
                CategoryPaths.emplace(Tag.substr(0, Dot));
                Dot = Tag.find('.', Dot + 1);
            }
        }

        const bool bFiltering = TagFilter.IsActive();
        const FString Current = Value.TagName.IsNone() ? FString() : FString(Value.TagName.c_str());

        THashMap<FString, FTreeNodeID> PathToNode;

        for (const FString& Tag : Tags)
        {
            if (bFiltering && !ImGuiX::PassSearchFilter(TagFilter, Tag.c_str()))
            {
                continue;
            }

            // Walk the dotted path, creating each segment node once (deduped by cumulative path).
            FTreeNodeID Parent = InvalidTreeNode;
            FString Accum;
            size_t Start = 0;
            while (true)
            {
                const size_t Dot = Tag.find('.', Start);
                const FString Segment = (Dot == FString::npos) ? Tag.substr(Start) : Tag.substr(Start, Dot - Start);

                if (!Accum.empty())
                {
                    Accum += ".";
                }
                Accum += Segment;

                FTreeNodeID Node;
                auto It = PathToNode.find(Accum);
                if (It == PathToNode.end())
                {
                    const bool bCategory = CategoryPaths.find(Accum) != CategoryPaths.end();
                    const char* Icon = bCategory ? LE_ICON_FOLDER : LE_ICON_TAG;

                    Node = Tree.CreateNode(Parent, FStringView(Segment.c_str(), Segment.size()), Hash::GetHash64(Accum));
                    Tree.EmplaceUserData<FString>(Node, Accum);

                    FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Node);
                    Display.DisplayName = FString(Icon) + "  " + Segment;
                    Display.IconText    = Icon;
                    Display.IconColor   = bCategory ? EditorColors::TextMuted() : EditorColors::Accent();
                    Display.DisplayColor = bCategory ? EditorColors::TextDim() : EditorColors::TextPrimary();
                    Display.TooltipText = Accum;

                    FTreeNodeState& State = Tree.Get<FTreeNodeState>(Node);
                    if (Accum == Current)
                    {
                        State.bSelected = true;
                    }

                    // Reveal: expand everything while searching, otherwise just the path to the current value.
                    const bool bAncestorOfCurrent = Current.size() > Accum.size()
                        && Current.compare(0, Accum.size(), Accum) == 0
                        && Current[Accum.size()] == '.';
                    if (bFiltering || bAncestorOfCurrent)
                    {
                        State.bExpanded = true;
                    }

                    PathToNode.emplace(Accum, Node);
                    ++LastBuiltCount;
                }
                else
                {
                    Node = It->second;
                }

                Parent = Node;
                if (Dot == FString::npos)
                {
                    break;
                }
                Start = Dot + 1;
            }
        }
    }

    void FGameplayTagPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (void* Ptr = Property->GetValuePtr())
        {
            *static_cast<FGameplayTag*>(Ptr) = Value;
        }
    }

    void FGameplayTagPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (const void* Ptr = Property->GetValuePtr())
        {
            Value = *static_cast<const FGameplayTag*>(Ptr);
        }
    }
}
