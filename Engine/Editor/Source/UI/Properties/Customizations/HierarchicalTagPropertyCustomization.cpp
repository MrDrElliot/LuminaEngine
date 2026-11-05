#include "HierarchicalTagPropertyCustomization.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "Config/Config.h"
#include "Core/Object/ObjectCore.h"
#include "HierarchicalTags/HierarchicalTagRegistry.h"
#include "HierarchicalTags/HierarchicalTagsSettings.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace HierarchicalTagEditorInternal
    {
        struct FTagNode
        {
            FString        Path;
            FString        Segment;
            int32          Depth = 0;
            TVector<int32> Children;
        };

        ImU32 Col(const ImVec4& Color, float Alpha = 1.0f)
        {
            return EditorColors::U32(EditorColors::WithAlpha(Color, Color.w * Alpha));
        }

        float RowHeight()
        {
            return ImGui::GetTextLineHeight() + 8.0f * ImGuiX::GetUIScale();
        }

        bool ValidateTagName(FStringView Name, FString& OutReason)
        {
            if (Name.IsEmpty())
            {
                OutReason = "Enter a name, such as Ability.Fire.Fireball.";
                return false;
            }
            int32 PartLength = 0;
            for (const char C : Name)
            {
                if (C == '.')
                {
                    if (PartLength == 0)
                    {
                        OutReason = "Every part between dots needs a name.";
                        return false;
                    }
                    PartLength = 0;
                    continue;
                }
                if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_'))
                {
                    OutReason = "Use only letters, digits, underscores and dots.";
                    return false;
                }
                ++PartLength;
            }
            if (PartLength == 0)
            {
                OutReason = "A tag can't end with a dot.";
                return false;
            }
            return true;
        }

        FStringView Trimmed(FStringView Text)
        {
            int32 First = 0;
            int32 Last = Text.Len();
            while (First < Last && Text[First] == ' ') { ++First; }
            while (Last > First && Text[Last - 1] == ' ') { --Last; }
            return Text.Mid(First, Last - First);
        }

        bool IsKnownTag(FName Tag)
        {
            return !Tag.IsNone() && FHierarchicalTagRegistry::Get().FindTag(Tag) != 0;
        }

        void AddProjectTag(const FString& Tag)
        {
            FHierarchicalTagRegistry::Get().RequestTag(FName(Tag));
            CHierarchicalTagsSettings* Settings = GetMutableDefault<CHierarchicalTagsSettings>();
            if (Settings != nullptr && !Algo::Contains(Settings->Tags, Tag))
            {
                Settings->Tags.Add(Tag);
                Algo::Sort(Settings->Tags, [](const FString& A, const FString& B) { return strcmp(*A, *B) < 0; });
                if (GConfig != nullptr)
                {
                    GConfig->SaveSettings(CHierarchicalTagsSettings::StaticClass());
                }
            }
        }

        // The parent path is dimmed so the leaf, the part that differs between neighbours, is what the eye lands on.
        void DrawTagText(ImDrawList* DrawList, ImVec2 Position, FStringView Tag, ImU32 LeafColor, ImU32 PathColor)
        {
            int32 Dot = Constants::kIndexNone;
            for (int32 Index = Tag.Len() - 1; Index >= 0; --Index)
            {
                if (Tag[Index] == '.')
                {
                    Dot = Index;
                    break;
                }
            }
            const char* Text = Tag.GetData();
            if (Dot != Constants::kIndexNone)
            {
                DrawList->AddText(Position, PathColor, Text, Text + Dot + 1);
                Position.x += ImGui::CalcTextSize(Text, Text + Dot + 1).x;
            }
            DrawList->AddText(Position, LeafColor, Text + Dot + 1, Text + Tag.Len());
        }

        enum class ECheck : uint8 { Off, Partial, On };

        void DrawCheck(ImDrawList* DrawList, ImVec2 Center, ECheck State)
        {
            const float Scale = ImGuiX::GetUIScale();
            const float Half = 6.5f * Scale;
            const ImVec2 Min(Center.x - Half, Center.y - Half);
            const ImVec2 Max(Center.x + Half, Center.y + Half);
            const float Rounding = 3.0f * Scale;
            if (State == ECheck::Off)
            {
                DrawList->AddRect(Min, Max, Col(EditorColors::TextMuted(), 0.55f), Rounding, 1.2f * Scale);
                return;
            }
            DrawList->AddRectFilled(Min, Max, Col(EditorColors::Accent(), State == ECheck::On ? 1.0f : 0.45f), Rounding);
            if (State == ECheck::On)
            {
                const ImVec2 Points[] = { ImVec2(Center.x - 3.5f * Scale, Center.y + 0.2f * Scale), ImVec2(Center.x - 1.0f * Scale, Center.y + 2.7f * Scale), ImVec2(Center.x + 3.8f * Scale, Center.y - 2.6f * Scale) };
                DrawList->AddPolyline(Points, 3, IM_COL32(255, 255, 255, 235), 1.7f * Scale);
            }
            else
            {
                DrawList->AddLine(ImVec2(Center.x - 3.0f * Scale, Center.y), ImVec2(Center.x + 3.0f * Scale, Center.y), IM_COL32(255, 255, 255, 220), 1.7f * Scale);
            }
        }

        constexpr float kInset = 8.0f;

        bool QuietButton(const char* Id, const char* Icon, const char* Label, bool bActive, const char* Detail = nullptr)
        {
            const float Scale = ImGuiX::GetUIScale();
            const float PadX = kInset * Scale;
            const float Height = ImGui::GetFrameHeight();
            const float IconWidth = Icon != nullptr ? ImGui::CalcTextSize(Icon).x + 6.0f * Scale : 0.0f;
            const float DetailWidth = Detail != nullptr ? ImGui::CalcTextSize(Detail).x + 6.0f * Scale : 0.0f;
            const float Width = PadX * 2.0f + IconWidth + ImGui::CalcTextSize(Label).x + DetailWidth;
            const ImVec2 Min = ImGui::GetCursorScreenPos();
            const bool bClicked = ImGui::InvisibleButton(Id, ImVec2(Width, Height));
            const bool bHovered = ImGui::IsItemHovered();
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            const ImVec2 Max(Min.x + Width, Min.y + Height);
            if (bHovered || bActive)
            {
                DrawList->AddRectFilled(Min, Max, Col(EditorColors::RowBgHovered()), ImGui::GetStyle().FrameRounding);
            }
            const float TextY = Min.y + (Height - ImGui::GetTextLineHeight()) * 0.5f;
            float X = Min.x + PadX;
            if (Icon != nullptr)
            {
                DrawList->AddText(ImVec2(X, TextY), Col(EditorColors::Accent()), Icon);
                X += IconWidth;
            }
            DrawList->AddText(ImVec2(X, TextY), Col(bHovered || bActive ? EditorColors::TextPrimary() : EditorColors::TextDim()), Label);
            if (Detail != nullptr)
            {
                DrawList->AddText(ImVec2(X + ImGui::CalcTextSize(Label).x + 6.0f * Scale, TextY), Col(EditorColors::TextMuted()), Detail);
            }
            return bClicked;
        }
    }

    using namespace HierarchicalTagEditorInternal;

    class FHierarchicalTagPicker
    {
    public:

        void OnOpened(const TVector<FName>& Selection);

        // Single mode replaces Selection; multi mode ticks tags in and out. True when Selection changed.
        bool Draw(bool bMulti, TVector<FName>& Selection);


    private:

        void Rebuild();
        bool SubtreePasses(int32 Node) const;
        void CollectRows(int32 Node, TVector<int32>& Out) const;
        void DrawRow(int32 Node, bool bMulti, TVector<FName>& Selection, bool& bChanged);
        bool DrawFooter(bool bMulti, TVector<FName>& Selection);

        TVector<FTagNode> Nodes;
        TVector<int32>    Roots;
        THashSet<FString> Expanded;
        THashSet<FString> PartialPaths;
        ImGuiTextFilter   Filter;
        char              NewTag[128] = {};
        FString           NewTagError;
        bool              bAdding      = false;
        bool              bFocusNewTag = false;
    };

    void FHierarchicalTagPicker::OnOpened(const TVector<FName>& Selection)
    {
        Filter.Clear();
        NewTag[0] = '\0';
        NewTagError.Reset();
        bAdding = false;
        Rebuild();

        Expanded.Reset();
        for (const FName& Tag : Selection)
        {
            const FString Path = Tag.ToString();
            for (int32 Dot = Path.Find('.'); Dot != Constants::kIndexNone; Dot = Path.Find('.', Dot + 1))
            {
                Expanded.Add(Path.Left(Dot));
            }
        }
    }

    void FHierarchicalTagPicker::Rebuild()
    {
        Nodes.Reset();
        Roots.Reset();

        TVector<FString> Tags;
        FHierarchicalTagRegistry::Get().GetAllTags(Tags);
        Algo::Sort(Tags, [](const FString& A, const FString& B) { return strcmp(*A, *B) < 0; });

        THashMap<FString, int32> ByPath;
        for (const FString& Tag : Tags)
        {
            int32 Parent = Constants::kIndexNone;
            int32 Start = 0;
            while (true)
            {
                const int32 Dot = Tag.Find('.', Start);
                const FString Path = Dot == Constants::kIndexNone ? Tag : Tag.Left(Dot);
                int32 Node;
                if (const int32* Existing = ByPath.Find(Path))
                {
                    Node = *Existing;
                }
                else
                {
                    Node = Nodes.Num();
                    FTagNode& Added = Nodes.EmplaceGetRef();
                    Added.Path = Path;
                    Added.Segment = Path.Mid(Start);
                    Added.Depth = Parent == Constants::kIndexNone ? 0 : Nodes[Parent].Depth + 1;
                    (Parent == Constants::kIndexNone ? Roots : Nodes[Parent].Children).Add(Node);
                    ByPath.Add(Path, Node);
                }
                Parent = Node;
                if (Dot == Constants::kIndexNone)
                {
                    break;
                }
                Start = Dot + 1;
            }
        }
    }

    bool FHierarchicalTagPicker::SubtreePasses(int32 Node) const
    {
        return ImGuiX::PassSearchFilter(Filter, Nodes[Node].Path)
            || Algo::AnyOf(Nodes[Node].Children, [this](int32 Child) { return SubtreePasses(Child); });
    }

    void FHierarchicalTagPicker::CollectRows(int32 Node, TVector<int32>& Out) const
    {
        const bool bFiltering = Filter.IsActive();
        if (bFiltering && !SubtreePasses(Node))
        {
            return;
        }
        Out.Add(Node);
        if (bFiltering || Expanded.Contains(Nodes[Node].Path))
        {
            for (const int32 Child : Nodes[Node].Children)
            {
                CollectRows(Child, Out);
            }
        }
    }

    void FHierarchicalTagPicker::DrawRow(int32 NodeIndex, bool bMulti, TVector<FName>& Selection, bool& bChanged)
    {
        const FTagNode& Node = Nodes[NodeIndex];
        const float Scale = ImGuiX::GetUIScale();
        const float Height = RowHeight();
        const float Width = ImGui::GetContentRegionAvail().x;
        const float Indent = 14.0f * Scale;
        const bool bFiltering = Filter.IsActive();
        const bool bHasChildren = !Node.Children.IsEmpty();
        const bool bExpanded = bFiltering || Expanded.Contains(Node.Path);
        const FName Tag(Node.Path);
        const bool bSelected = Algo::Contains(Selection, Tag);
        const bool bPartial = !bSelected && PartialPaths.Contains(Node.Path);
        const bool bMatch = !bFiltering || ImGuiX::PassSearchFilter(Filter, Node.Path);

        ImGui::PushID(NodeIndex);
        const ImVec2 Min = ImGui::GetCursorScreenPos();
        const ImVec2 Max(Min.x + Width, Min.y + Height);
        ImGui::InvisibleButton("##row", ImVec2(Width, Height));
        const bool bHovered = ImGui::IsItemHovered();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const float Rounding = 4.0f * Scale;

        if (!bMulti && bSelected)
        {
            DrawList->AddRectFilled(Min, Max, Col(EditorColors::Accent(), 0.2f), Rounding);
        }
        else if (bHovered)
        {
            DrawList->AddRectFilled(Min, Max, Col(EditorColors::RowBgHovered()), Rounding);
        }

        const float Left = Min.x + 4.0f * Scale;
        for (int32 Level = 1; Level <= Node.Depth; ++Level)
        {
            const float GuideX = Left + (Level - 1) * Indent + Indent * 0.5f;
            DrawList->AddLine(ImVec2(GuideX, Min.y), ImVec2(GuideX, Max.y), Col(EditorColors::Border(), 0.6f));
        }

        const float TextY = Min.y + (Height - ImGui::GetTextLineHeight()) * 0.5f;
        float X = Left + Node.Depth * Indent;
        const float ArrowWidth = Indent;
        if (bHasChildren)
        {
            const char* Arrow = bExpanded ? LE_ICON_CHEVRON_DOWN : LE_ICON_CHEVRON_RIGHT;
            const float ArrowTextWidth = ImGui::CalcTextSize(Arrow).x;
            DrawList->AddText(ImVec2(X + (ArrowWidth - ArrowTextWidth) * 0.5f, TextY), Col(EditorColors::TextMuted(), bFiltering ? 0.35f : 0.9f), Arrow);
        }
        const float ArrowRight = X + ArrowWidth;
        X = ArrowRight + 2.0f * Scale;

        if (bMulti)
        {
            const float Box = 13.0f * Scale;
            DrawCheck(DrawList, ImVec2(X + Box * 0.5f, Min.y + Height * 0.5f), bSelected ? ECheck::On : bPartial ? ECheck::Partial : ECheck::Off);
            X += Box + 7.0f * Scale;
        }

        const ImU32 TextColor = !bMatch ? Col(EditorColors::TextMuted(), 0.55f)
            : bSelected || bHovered ? Col(EditorColors::TextPrimary())
            : Col(EditorColors::TextPrimary(), bHasChildren ? 0.8f : 0.92f);
        DrawList->AddText(ImVec2(X, TextY), TextColor, *Node.Segment);

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (bHasChildren && !bFiltering && ImGui::GetIO().MousePos.x < ArrowRight)
            {
                bExpanded ? (void)Expanded.Remove(Node.Path) : (void)Expanded.Add(Node.Path);
            }
            else if (bMulti)
            {
                bSelected ? (void)Selection.RemoveSingle(Tag) : (void)Selection.Add(Tag);
                bChanged = true;
            }
            else
            {
                Selection.Reset();
                Selection.Add(Tag);
                bChanged = true;
            }
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("%s", *Node.Path);
        }
        if (ImGui::BeginPopupContextItem("##rowmenu"))
        {
            if (ImGui::MenuItem(LE_ICON_PLUS "  Add Sub Tag"))
            {
                const FString Prefix = Node.Path + ".";
                ImStrncpy(NewTag, *Prefix, sizeof(NewTag));
                NewTagError.Reset();
                bAdding = true;
                bFocusNewTag = true;
            }
            if (ImGui::MenuItem(LE_ICON_CONTENT_COPY "  Copy Name"))
            {
                ImGui::SetClipboardText(*Node.Path);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    bool FHierarchicalTagPicker::DrawFooter(bool bMulti, TVector<FName>& Selection)
    {
        const float Scale = ImGuiX::GetUIScale();
        if (!bAdding)
        {
            if (QuietButton("##newtag", LE_ICON_PLUS, "New Tag...", false))
            {
                bAdding = true;
                bFocusNewTag = true;
            }
            return false;
        }

        const ImGuiStyle& Style = ImGui::GetStyle();
        const float ButtonSize = ImGui::GetFrameHeight();
        if (bFocusNewTag)
        {
            ImGui::SetKeyboardFocusHere();
            bFocusNewTag = false;
        }
        ImGui::SetNextItemWidth(ImMax(ImGui::GetContentRegionAvail().x - ButtonSize - Style.ItemSpacing.x - Style.FramePadding.x, 48.0f));
        const bool bEnter = ImGui::InputTextWithHint("##newtagname", "Ability.Fire.Fireball", NewTag, sizeof(NewTag), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited())
        {
            NewTagError.Reset();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::WithAlpha(EditorColors::Accent(), 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorColors::Accent());
        const bool bAdd = ImGui::Button(LE_ICON_PLUS "##add", ImVec2(ButtonSize, 0.0f)) || bEnter;
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Add tag");
        }
        if (!NewTagError.IsEmpty())
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f * Scale));
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT_CIRCLE_OUTLINE " %s", *NewTagError);
        }

        if (!bAdd)
        {
            return false;
        }
        const FString Name(Trimmed(NewTag));
        if (!ValidateTagName(Name, NewTagError))
        {
            return false;
        }
        AddProjectTag(Name);
        Rebuild();
        for (int32 Dot = Name.Find('.'); Dot != Constants::kIndexNone; Dot = Name.Find('.', Dot + 1))
        {
            Expanded.Add(Name.Left(Dot));
        }
        if (!bMulti)
        {
            Selection.Reset();
        }
        if (!Algo::Contains(Selection, FName(Name)))
        {
            Selection.Add(FName(Name));
        }
        NewTag[0] = '\0';
        bAdding = false;
        return true;
    }

    bool FHierarchicalTagPicker::Draw(bool bMulti, TVector<FName>& Selection)
    {
        const float Scale = ImGuiX::GetUIScale();
        bool bChanged = false;

        PartialPaths.Reset();
        for (const FName& Tag : Selection)
        {
            const FString Path = Tag.ToString();
            for (int32 Dot = Path.Find('.'); Dot != Constants::kIndexNone; Dot = Path.Find('.', Dot + 1))
            {
                PartialPaths.Add(Path.Left(Dot));
            }
        }

        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGuiX::SearchBar("##tagsearch", Filter, LE_ICON_MAGNIFY "  Search tags");
        ImGui::Dummy(ImVec2(0.0f, 1.0f * Scale));

        TVector<int32> Rows;
        for (const int32 Root : Roots)
        {
            CollectRows(Root, Rows);
        }

        const int32 Shown = ImClamp(Rows.Num(), 1, 12);
        if (ImGui::BeginChild("##tagrows", ImVec2(0.0f, Shown * RowHeight()), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            for (const int32 Row : Rows)
            {
                DrawRow(Row, bMulti, Selection, bChanged);
            }
            ImGui::PopStyleVar();
            if (Rows.IsEmpty())
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(EditorColors::TextMuted(), Nodes.IsEmpty() ? "  No tags yet." : "  Nothing matches.");
            }
        }
        ImGui::EndChild();

        const ImVec2 LineMin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(LineMin, ImVec2(LineMin.x + ImGui::GetContentRegionAvail().x, LineMin.y), Col(EditorColors::Border(), 0.8f));
        ImGui::Dummy(ImVec2(0.0f, 4.0f * Scale));

        if (DrawFooter(bMulti, Selection))
        {
            bChanged = true;
        }
        return bChanged;
    }

    //~ Single tag

    FHierarchicalTagPropertyCustomization::FHierarchicalTagPropertyCustomization()
        : Picker(MakeUnique<FHierarchicalTagPicker>())
    {
    }

    FHierarchicalTagPropertyCustomization::~FHierarchicalTagPropertyCustomization() = default;

    EPropertyChangeOp FHierarchicalTagPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bool bChanged = false;
        const float Scale = ImGuiX::GetUIScale();
        const ImGuiStyle& Style = ImGui::GetStyle();
        const bool bSet = !Value.TagName.IsNone();
        const bool bKnown = !bSet || IsKnownTag(Value.TagName);
        const bool bOpen = ImGui::IsPopupOpen("##tagpicker");

        const float Width = ImGui::GetContentRegionAvail().x;
        const float Height = ImGui::GetFrameHeight();
        const ImVec2 Min = ImGui::GetCursorScreenPos();
        const ImVec2 Max(Min.x + Width, Min.y + Height);
        const ImRect Clear(ImVec2(Max.x - Height * 2.0f, Min.y), ImVec2(Max.x - Height, Max.y));

        ImGui::InvisibleButton("##field", ImVec2(Width, Height));
        const bool bHovered = ImGui::IsItemHovered();
        const bool bOverClear = bSet && bHovered && Clear.Contains(ImGui::GetIO().MousePos);

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(Min, Max, Col(bOpen || bHovered ? EditorColors::RowBgHovered() : EditorColors::FrameBg()), Style.FrameRounding);
        if (!bKnown)
        {
            DrawList->AddRect(Min, Max, Col(EditorColors::Warning(), 0.6f), Style.FrameRounding);
        }

        const float TextY = Min.y + (Height - ImGui::GetTextLineHeight()) * 0.5f;
        float X = Min.x + Style.FramePadding.x;
        const char* Icon = bKnown ? LE_ICON_TAG_OUTLINE : LE_ICON_ALERT_CIRCLE_OUTLINE;
        DrawList->AddText(ImVec2(X, TextY), Col(!bKnown ? EditorColors::Warning() : bSet ? EditorColors::Accent() : EditorColors::TextMuted()), Icon);
        X += ImGui::CalcTextSize(Icon).x + 6.0f * Scale;

        DrawList->PushClipRect(ImVec2(X, Min.y), ImVec2(Clear.Min.x, Max.y), true);
        if (bSet)
        {
            DrawTagText(DrawList, ImVec2(X, TextY), Value.TagName.ToString().View(), Col(bKnown ? EditorColors::TextPrimary() : EditorColors::Warning()), Col(EditorColors::TextMuted()));
        }
        else
        {
            DrawList->AddText(ImVec2(X, TextY), Col(EditorColors::TextMuted()), "None");
        }
        DrawList->PopClipRect();

        if (bSet && bHovered)
        {
            const ImVec2 Size = ImGui::CalcTextSize(LE_ICON_CLOSE);
            DrawList->AddText(ImVec2(Clear.GetCenter().x - Size.x * 0.5f, TextY), Col(bOverClear ? EditorColors::Danger() : EditorColors::TextMuted()), LE_ICON_CLOSE);
        }
        const ImVec2 ArrowSize = ImGui::CalcTextSize(LE_ICON_CHEVRON_DOWN);
        DrawList->AddText(ImVec2(Max.x - Height * 0.5f - ArrowSize.x * 0.5f, TextY), Col(EditorColors::TextMuted()), LE_ICON_CHEVRON_DOWN);

        if (bHovered && !bKnown)
        {
            ImGui::SetTooltip("%s is not a registered tag.", *Value.TagName.ToString());
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (bOverClear)
            {
                Value.TagName = FName();
                bChanged = true;
            }
            else
            {
                TVector<FName> Current;
                if (bSet)
                {
                    Current.Add(Value.TagName);
                }
                Picker->OnOpened(Current);
                ImGui::OpenPopup("##tagpicker");
            }
        }

        ImGui::SetNextWindowPos(ImVec2(Min.x, Max.y + 2.0f * Scale));
        ImGui::SetNextWindowSizeConstraints(ImVec2(ImMax(Width, 300.0f * Scale), 0.0f), ImVec2(ImMax(Width, 420.0f * Scale), FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * Scale, 8.0f * Scale));
        if (ImGui::BeginPopup("##tagpicker"))
        {
            TVector<FName> Selection;
            if (bSet)
            {
                Selection.Add(Value.TagName);
            }
            if (Picker->Draw(false, Selection))
            {
                Value.TagName = Selection.IsEmpty() ? FName() : Selection[0];
                bChanged = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        return EditSession.Advance(bChanged, false);
    }

    void FHierarchicalTagPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (void* Ptr = Property->GetValuePtr())
        {
            *static_cast<SHierarchicalTag*>(Ptr) = Value;
        }
    }

    void FHierarchicalTagPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (const void* Ptr = Property->GetValuePtr())
        {
            Value = *static_cast<const SHierarchicalTag*>(Ptr);
        }
    }

    //~ Tag container

    FHierarchicalTagContainerPropertyCustomization::FHierarchicalTagContainerPropertyCustomization()
        : Picker(MakeUnique<FHierarchicalTagPicker>())
    {
    }

    FHierarchicalTagContainerPropertyCustomization::~FHierarchicalTagContainerPropertyCustomization() = default;

    EPropertyChangeOp FHierarchicalTagContainerPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bool bChanged = false;
        const float Scale = ImGuiX::GetUIScale();

        const bool bOpen = ImGui::IsPopupOpen("##tagpicker");
        const ImVec2 ButtonMin = ImGui::GetCursorScreenPos();
        const FString Count = Format("{}", Value.Tags.Num());
        if (QuietButton("##edit", LE_ICON_TAG_MULTIPLE_OUTLINE, "Edit Tags", bOpen, Value.Tags.IsEmpty() ? nullptr : *Count))
        {
            TVector<FName> Current;
            for (const SHierarchicalTag& Tag : Value.Tags)
            {
                Current.Add(Tag.TagName);
            }
            Picker->OnOpened(Current);
            ImGui::OpenPopup("##tagpicker");
        }
        const float ButtonBottom = ImGui::GetItemRectMax().y;

        if (!Value.Tags.IsEmpty())
        {
            ImGui::SameLine();
            ImGuiX::AlignRight(ImGui::CalcTextSize("Clear").x + kInset * 2.0f * Scale);
            if (QuietButton("##clear", nullptr, "Clear", false))
            {
                Value.Tags.Reset();
                bChanged = true;
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 1.0f * Scale));
        const float Height = RowHeight();
        const float Width = ImGui::GetContentRegionAvail().x;
        const ImVec2 PanelMin = ImGui::GetCursorScreenPos();
        const int32 RowCount = ImMax(Value.Tags.Num(), 1);
        const float Pad = 4.0f * Scale;
        const ImVec2 PanelMax(PanelMin.x + Width, PanelMin.y + RowCount * Height + Pad * 2.0f);
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(PanelMin, PanelMax, Col(EditorColors::FrameBg(), 0.55f), ImGui::GetStyle().FrameRounding);

        int32 RemoveIndex = Constants::kIndexNone;
        ImGui::SetCursorScreenPos(ImVec2(PanelMin.x, PanelMin.y + Pad));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        if (Value.Tags.IsEmpty())
        {
            DrawList->AddText(ImVec2(PanelMin.x + kInset * Scale, PanelMin.y + Pad + (Height - ImGui::GetTextLineHeight()) * 0.5f), Col(EditorColors::TextMuted(), 0.8f), "No tags");
            ImGui::Dummy(ImVec2(Width, Height));
        }

        for (int32 Index = 0; Index < Value.Tags.Num(); ++Index)
        {
            const FString Tag = Value.Tags[Index].TagName.ToString();
            const bool bKnown = IsKnownTag(Value.Tags[Index].TagName);

            ImGui::PushID(Index);
            const ImVec2 Min = ImGui::GetCursorScreenPos();
            const ImVec2 Max(Min.x + Width, Min.y + Height);
            ImGui::InvisibleButton("##tagrow", ImVec2(Width, Height));
            const bool bHovered = ImGui::IsItemHovered();
            const ImRect Remove(ImVec2(Max.x - Height, Min.y), Max);
            const bool bOverRemove = bHovered && Remove.Contains(ImGui::GetIO().MousePos);

            if (bHovered)
            {
                DrawList->AddRectFilled(ImVec2(Min.x + Pad, Min.y), ImVec2(Max.x - Pad, Max.y), Col(EditorColors::RowBgHovered()), 4.0f * Scale);
            }
            const float TextY = Min.y + (Height - ImGui::GetTextLineHeight()) * 0.5f;
            float X = Min.x + kInset * Scale;
            const char* Icon = bKnown ? LE_ICON_TAG_OUTLINE : LE_ICON_ALERT_CIRCLE_OUTLINE;
            DrawList->AddText(ImVec2(X, TextY), Col(bKnown ? EditorColors::Accent() : EditorColors::Warning(), bKnown ? 0.85f : 1.0f), Icon);
            X += ImGui::CalcTextSize(Icon).x + 6.0f * Scale;
            DrawList->PushClipRect(ImVec2(X, Min.y), ImVec2(Remove.Min.x, Max.y), true);
            DrawTagText(DrawList, ImVec2(X, TextY), Tag.View(), Col(bKnown ? EditorColors::TextPrimary() : EditorColors::Warning()), Col(EditorColors::TextMuted()));
            DrawList->PopClipRect();

            if (bHovered)
            {
                const ImVec2 Size = ImGui::CalcTextSize(LE_ICON_CLOSE);
                DrawList->AddText(ImVec2(Remove.GetCenter().x - Size.x * 0.5f, TextY), Col(bOverRemove ? EditorColors::Danger() : EditorColors::TextMuted()), LE_ICON_CLOSE);
                if (!bKnown && !bOverRemove)
                {
                    ImGui::SetTooltip("%s is not a registered tag. Right-click to add it to the project.", *Tag);
                }
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && bOverRemove)
            {
                RemoveIndex = Index;
            }
            if (ImGui::BeginPopupContextItem("##tagmenu"))
            {
                if (!bKnown && ImGui::MenuItem(LE_ICON_PLUS "  Add To Project Tags"))
                {
                    AddProjectTag(Tag);
                }
                if (ImGui::MenuItem(LE_ICON_CONTENT_COPY "  Copy"))
                {
                    ImGui::SetClipboardText(*Tag);
                }
                if (ImGui::MenuItem(LE_ICON_CLOSE "  Remove"))
                {
                    RemoveIndex = Index;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(ImVec2(PanelMin.x, PanelMax.y));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));

        ImGui::SetNextWindowPos(ImVec2(ButtonMin.x, ButtonBottom + 2.0f * Scale), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(ImClamp(Width, 300.0f * Scale, 420.0f * Scale), 0.0f), ImVec2(420.0f * Scale, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * Scale, 8.0f * Scale));
        if (ImGui::BeginPopup("##tagpicker"))
        {
            TVector<FName> Selection;
            for (const SHierarchicalTag& Tag : Value.Tags)
            {
                Selection.Add(Tag.TagName);
            }
            if (Picker->Draw(true, Selection))
            {
                TVector<SHierarchicalTag> Tags;
                for (const FName& Name : Selection)
                {
                    Tags.EmplaceGetRef().TagName = Name;
                }
                Value.Tags = Move(Tags);
                bChanged = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        if (RemoveIndex != Constants::kIndexNone)
        {
            Value.Tags.RemoveAt(RemoveIndex);
            bChanged = true;
        }

        return EditSession.Advance(bChanged, false);
    }

    void FHierarchicalTagContainerPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (void* Ptr = Property->GetValuePtr())
        {
            *static_cast<SHierarchicalTagContainer*>(Ptr) = Value;
        }
    }

    void FHierarchicalTagContainerPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        if (const void* Ptr = Property->GetValuePtr())
        {
            Value = *static_cast<const SHierarchicalTagContainer*>(Ptr);
        }
    }
}
