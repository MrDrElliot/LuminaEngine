#include "ObjectBrowserEditorTool.h"


#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectFlags.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/Profile.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"

namespace Lumina
{
    namespace
    {
        // Shared by the header setup and the sort, so reordering columns cannot sort the wrong field.
        enum EObjectColumn : int32
        {
            Column_Name = 0,
            Column_Class,
            Column_Package,
            Column_StrongRefs,
            Column_WeakRefs,
            Column_Flags,
            Column_Count,
        };

        ImVec4 FlagsTint(EObjectFlags Flags)
        {
            if (EnumHasAnyFlags(Flags, OF_MarkedDestroy)) { return ImVec4(0.95f, 0.55f, 0.45f, 1.0f); }
            if (EnumHasAnyFlags(Flags, OF_Rooted))        { return ImVec4(0.55f, 0.85f, 1.00f, 1.0f); }
            if (EnumHasAnyFlags(Flags, OF_DefaultObject)) { return ImVec4(0.70f, 0.65f, 0.90f, 1.0f); }
            return EditorColors::TextMuted();
        }

        void DrawFilterHint(const char* Hint)
        {
            const ImVec2 Min = ImGui::GetItemRectMin();
            const ImVec2 Pad = ImGui::GetStyle().FramePadding;
            ImGui::GetWindowDrawList()->AddText(ImVec2(Min.x + Pad.x, Min.y + Pad.y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), Hint);
        }
    }

    void FObjectBrowserEditorTool::OnInitialize()
    {
        CreateToolWindow("Object Browser", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FObjectBrowserEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FObjectBrowserEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("What this is",
            "Every live CObject: loaded assets, class defaults, transient runtime objects. The tool to reach "
            "for when something is still alive that should not be, or when you want to know what a package "
            "actually holds.");
        DrawHelpTextRow("Snapshots",
            "The list is a snapshot, not a live view. Auto-refresh re-takes it on an interval; switch it off "
            "and use Refresh to hold a moment still while you read it. Nothing walks the object array in "
            "between, so the tool costs the same whether there are a hundred objects or a million.");
        DrawHelpTextRow("Refs",
            "Strong refs keep the object alive. Weak refs observe without keeping alive. An object sitting at "
            "0 strong that is still listed is either rooted or waiting on the next destroy pass.");
        DrawHelpTextRow("Filters",
            "Class defaults are hidden by default -- one per class, and rarely what you are looking for. "
            "'Pending Destroy' shows objects already marked for teardown, which is where a leak hunt starts.");
        DrawHelpTextRow("Details",
            "Selecting a row inspects that object's live properties, read-only. The selection is held weakly, "
            "so it reports the object as destroyed rather than showing stale values.");
    }

    void FObjectBrowserEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FEditorTool::Update(UpdateContext);

        if (!bAutoRefresh)
        {
            return;
        }

        TimeSinceRefresh += (float)UpdateContext.GetDeltaTime();
        if (TimeSinceRefresh >= RefreshInterval)
        {
            TimeSinceRefresh = 0.0f;
            bSnapshotDirty = true;
        }
    }

    void FObjectBrowserEditorTool::TakeSnapshot()
    {
        LUMINA_PROFILE_SCOPE();

        Rows.clear();
        TotalAlive = GObjectArray.GetNumAliveObjects();
        TotalSlots = GObjectArray.GetMaxObjects();
        Rows.reserve((size_t)Math::Max(TotalAlive, 0));

        // The ONE walk, so the draw path never touches the object array or dereferences a CObject.
        GObjectArray.ForEachObject([this](CObjectBase* Base, int32 Index)
        {
            CObject* Object = static_cast<CObject*>(Base);
            if (Object == nullptr)
            {
                return;
            }

            FObjectBrowserRow& Row = Rows.emplace_back();
            Row.Object     = Object;
            Row.Name       = Object->GetName();
            Row.Flags      = Object->GetFlags();
            Row.FlagsText  = ObjectFlagsToString(Row.Flags).c_str();
            Row.StrongRefs = Object->GetStrongRefCount();
            Row.WeakRefs   = Object->GetWeakRefCount();
            Row.bIsAsset   = Object->IsAsset();

            if (CClass* Class = Object->GetClass())
            {
                Row.ClassName = Class->GetName();
            }
            if (CPackage* Package = Object->GetPackage())
            {
                Row.PackageName = Package->GetName();
            }
        });

        bSnapshotDirty = false;
        bVisibleRowsDirty = true;
    }

    void FObjectBrowserEditorTool::RebuildVisibleRows()
    {
        LUMINA_PROFILE_SCOPE();

        VisibleRows.clear();
        VisibleRows.reserve(Rows.size());

        for (int32 i = 0; i < (int32)Rows.size(); ++i)
        {
            const FObjectBrowserRow& Row = Rows[i];

            if (!Filter.bShowDefaults       && EnumHasAnyFlags(Row.Flags, OF_DefaultObject)) { continue; }
            if (!Filter.bShowTransient      && EnumHasAnyFlags(Row.Flags, OF_Transient))     { continue; }
            if (!Filter.bShowPendingDestroy && EnumHasAnyFlags(Row.Flags, OF_MarkedDestroy)) { continue; }
            if (Filter.bRootedOnly          && !EnumHasAnyFlags(Row.Flags, OF_Rooted))       { continue; }
            if (Filter.bAssetsOnly          && !Row.bIsAsset)                                { continue; }

            if (NameFilter.IsActive() && !ImGuiX::PassSearchFilter(NameFilter, Row.Name.c_str()))
            {
                continue;
            }
            if (ClassFilter.IsActive() && !ImGuiX::PassSearchFilter(ClassFilter, Row.ClassName.c_str()))
            {
                continue;
            }

            VisibleRows.push_back(i);
        }

        ApplySort();
        bVisibleRowsDirty = false;
    }

    void FObjectBrowserEditorTool::ApplySort()
    {
        const int32 Column     = SortColumn;
        const bool  bAscending = bSortAscending;
        const TVector<FObjectBrowserRow>& Source = Rows;

        Algo::StableSort(VisibleRows, [&Source, Column, bAscending](int32 A, int32 B)
        {
            const FObjectBrowserRow& RowA = Source[A];
            const FObjectBrowserRow& RowB = Source[B];

            int32 Comparison = 0;
            switch (Column)
            {
            // The old comparator called ToString on both sides, two heap allocations per compare per frame.
            case Column_Name:       Comparison = strcmp(RowA.Name.c_str(), RowB.Name.c_str()); break;
            case Column_Class:      Comparison = strcmp(RowA.ClassName.c_str(), RowB.ClassName.c_str()); break;
            case Column_Package:    Comparison = strcmp(RowA.PackageName.c_str(), RowB.PackageName.c_str()); break;
            case Column_StrongRefs: Comparison = RowA.StrongRefs - RowB.StrongRefs; break;
            case Column_WeakRefs:   Comparison = RowA.WeakRefs - RowB.WeakRefs; break;
            case Column_Flags:      Comparison = (int32)RowA.Flags - (int32)RowB.Flags; break;
            default: break;
            }

            return bAscending ? Comparison < 0 : Comparison > 0;
        });
    }

    CObject* FObjectBrowserEditorTool::ResolveSelection() const
    {
        return SelectedObject.Get();
    }

    void FObjectBrowserEditorTool::DrawWindow(bool bIsFocused)
    {
        if (bSnapshotDirty)
        {
            TakeSnapshot();
        }

        DrawToolbar();
        ImGui::Separator();

        const float DetailsWidth = Math::Max(ImGui::GetContentRegionAvail().x * 0.32f, 240.0f);
        const float TableWidth   = Math::Max(ImGui::GetContentRegionAvail().x - DetailsWidth - ImGui::GetStyle().ItemSpacing.x, 200.0f);

        if (ImGui::BeginChild("##ObjectTable", ImVec2(TableWidth, 0.0f), ImGuiChildFlags_ResizeX))
        {
            DrawTable();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        if (ImGui::BeginChild("##ObjectDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
        {
            DrawDetailsPanel();
        }
        ImGui::EndChild();
    }

    void FObjectBrowserEditorTool::DrawToolbar()
    {
        if (ImGui::Button(LE_ICON_REFRESH " Refresh"))
        {
            bSnapshotDirty = true;
        }

        ImGui::SameLine();
        if (ImGui::Checkbox("Auto", &bAutoRefresh))
        {
            TimeSinceRefresh = 0.0f;
        }
        ImGuiX::TextTooltip("Re-take the snapshot on an interval. Switch it off to hold the list still.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##Interval", &RefreshInterval, 0.25f, 5.0f, "%.2fs");

        ImGui::SameLine();
        ImGui::TextDisabled("|");

        // Counts come from the snapshot, so they describe what is actually on screen.
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d shown", (int32)VisibleRows.size());
        ImGui::SameLine();
        ImGui::TextDisabled("of %d live / %d slots", TotalAlive, TotalSlots);

        // A filter change invalidates the visible SET, never the snapshot, so no re-walk per keystroke.
        bool bFilterChanged = false;

        ImGui::SetNextItemWidth(200.0f);
        bFilterChanged |= NameFilter.Draw("##NameFilter");
        if (!NameFilter.IsActive()) { DrawFilterHint(LE_ICON_MAGNIFY " Name..."); }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        bFilterChanged |= ClassFilter.Draw("##ClassFilter");
        if (!ClassFilter.IsActive()) { DrawFilterHint(LE_ICON_MAGNIFY " Class..."); }

        ImGui::SameLine();
        bFilterChanged |= ImGui::Checkbox("Defaults", &Filter.bShowDefaults);
        ImGuiX::TextTooltip("Show class default objects. One per class, so they dominate the list.");

        ImGui::SameLine();
        bFilterChanged |= ImGui::Checkbox("Transient", &Filter.bShowTransient);

        ImGui::SameLine();
        bFilterChanged |= ImGui::Checkbox("Pending Destroy", &Filter.bShowPendingDestroy);
        ImGuiX::TextTooltip("Objects already marked for teardown -- where a leak hunt usually starts.");

        ImGui::SameLine();
        bFilterChanged |= ImGui::Checkbox("Assets", &Filter.bAssetsOnly);

        ImGui::SameLine();
        bFilterChanged |= ImGui::Checkbox("Rooted", &Filter.bRootedOnly);
        ImGuiX::TextTooltip("GC roots only -- what is holding the rest of the object graph alive.");

        if (bFilterChanged)
        {
            bVisibleRowsDirty = true;
        }
    }

    void FObjectBrowserEditorTool::DrawTable()
    {
        constexpr ImGuiTableFlags TableFlags =
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable |
            ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        if (!ImGui::BeginTable("##Objects", Column_Count, TableFlags))
        {
            return;
        }

        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_DefaultSort, 0.28f, Column_Name);
        ImGui::TableSetupColumn("Class",   ImGuiTableColumnFlags_None,        0.22f, Column_Class);
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_None,        0.26f, Column_Package);
        ImGui::TableSetupColumn("Strong",  ImGuiTableColumnFlags_WidthFixed,  56.0f, Column_StrongRefs);
        ImGui::TableSetupColumn("Weak",    ImGuiTableColumnFlags_WidthFixed,  50.0f, Column_WeakRefs);
        ImGui::TableSetupColumn("Flags",   ImGuiTableColumnFlags_None,        0.24f, Column_Flags);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs())
        {
            if (Specs->SpecsDirty && Specs->SpecsCount > 0)
            {
                SortColumn        = (int32)Specs->Specs[0].ColumnUserID;
                bSortAscending    = Specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                bVisibleRowsDirty = true;
                Specs->SpecsDirty = false;
            }
        }

        // After the sort spec is known, or the clipper walks a list ordered for the previous spec.
        if (bVisibleRowsDirty)
        {
            RebuildVisibleRows();
        }

        CObject* Selected = ResolveSelection();

        ImGuiListClipper Clipper;
        Clipper.Begin((int32)VisibleRows.size());

        while (Clipper.Step())
        {
            for (int32 i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
            {
                const FObjectBrowserRow& Row = Rows[VisibleRows[i]];

                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(Column_Name);

                // Compared by resolved pointer, since the index changes on every refilter and sort.
                const bool bSelected = (Selected != nullptr) && (Selected == Row.Object.Get());
                if (ImGui::Selectable(Row.Name.c_str(), bSelected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    SelectedObject = Row.Object;
                }

                if (ImGui::BeginPopupContextItem("##RowMenu"))
                {
                    if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy Name"))
                    {
                        ImGui::SetClipboardText(Row.Name.c_str());
                    }
                    if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy Class"))
                    {
                        ImGui::SetClipboardText(Row.ClassName.c_str());
                    }
                    // Resolved on demand, since the GUID is not worth a string per row per snapshot.
                    if (CObject* Live = Row.Object.Get())
                    {
                        if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy GUID"))
                        {
                            ImGui::SetClipboardText(Live->GetGUID().ToString().c_str());
                        }
                    }
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(Column_Class);
                ImGui::TextUnformatted(Row.ClassName.c_str());

                ImGui::TableSetColumnIndex(Column_Package);
                if (Row.PackageName.IsNone())
                {
                    ImGui::TextDisabled("-");
                }
                else
                {
                    ImGui::TextUnformatted(Row.PackageName.c_str());
                }

                ImGui::TableSetColumnIndex(Column_StrongRefs);
                ImGui::Text("%d", Row.StrongRefs);

                ImGui::TableSetColumnIndex(Column_WeakRefs);
                ImGui::TextDisabled("%d", Row.WeakRefs);

                ImGui::TableSetColumnIndex(Column_Flags);
                ImGui::TextColored(FlagsTint(Row.Flags), "%s", Row.FlagsText.c_str());

                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

    void FObjectBrowserEditorTool::DrawDetailsPanel()
    {
        CObject* Object = ResolveSelection();

        if (Object == nullptr)
        {
            // Nothing picked and what you picked has died are different answers, which is the point here.
            if (SelectedObject.IsStale())
            {
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "The selected object has been destroyed.");
            }
            else
            {
                ImGui::TextDisabled("Select an object to inspect it.");
            }

            DetailsTable.reset();
            DetailsBoundObject = nullptr;
            return;
        }

        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::LargeBold);
        ImGui::TextUnformatted(Object->GetName().c_str());
        ImGuiX::Font::PopFont();

        // The class chain is the quickest way to see what a thing is and what it inherits.
        FString Chain;
        for (CClass* Class = Object->GetClass(); Class != nullptr; Class = Cast<CClass>(Class->GetSuperStruct()))
        {
            if (!Chain.empty())
            {
                Chain += " < ";
            }
            Chain += Class->GetName().c_str();
        }
        ImGui::TextColored(EditorColors::TextDim(), "%s", Chain.c_str());

        if (CPackage* Package = Object->GetPackage())
        {
            ImGui::TextColored(EditorColors::TextMuted(), "Package: %s", Package->GetName().c_str());
        }

        ImGui::TextColored(EditorColors::TextMuted(), "GUID: %s", Object->GetGUID().ToString().c_str());
        ImGui::TextColored(FlagsTint(Object->GetFlags()), "%s", ObjectFlagsToString(Object->GetFlags()).c_str());
        ImGui::TextColored(EditorColors::TextMuted(), "Strong: %d   Weak: %d",
            Object->GetStrongRefCount(), Object->GetWeakRefCount());

        ImGui::Separator();
        ImGui::Spacing();

        // Rebound only on a real selection change, since the table carries expansion and scroll state.
        if (DetailsBoundObject != Object)
        {
            DetailsBoundObject = Object;
            DetailsTable = MakeUnique<FPropertyTable>(Object);
        }

        if (DetailsTable)
        {
            // Read-only on purpose, since letting a debug list write into class defaults is a bigger promise.
            DetailsTable->DrawTree(/*bReadOnly*/ true);
        }
    }
}
