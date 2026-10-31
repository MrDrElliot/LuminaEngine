#include "PropertyMatrixTool.h"

#include "EditorToolContext.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/Factories/TextureFactory/TextureFactory.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Transactions/ObjectSnapshotCommand.h"

namespace Lumina
{
    static constexpr const char* GridWindowName     = "Assets";
    static constexpr const char* DetailsWindowName  = "Selection";
    static constexpr int32       kMaxDefaultColumns = 8;
    static constexpr int32       kFallbackColumns   = 6;

    namespace
    {
        // Settings such as a texture's compression only reach its pixels through a cook.
        void RecookAfterEdit(TSpan<CObject* const> Objects, FProperty* Property)
        {
            if (Property == nullptr || !Property->HasEditorFlag(EPropertyEditorFlags::RequiresRecook))
            {
                return;
            }

            int32 Recooked = 0;
            int32 Failed = 0;
            for (CObject* Object : Objects)
            {
                if (CTexture* Texture = Cast<CTexture>(Object))
                {
                    ++(CTextureFactory::Recook(Texture) ? Recooked : Failed);
                }
            }

            if (Failed > 0)
            {
                ImGuiX::Notifications::NotifyError("Recooked {0} texture(s), {1} failed, see the log.", Recooked, Failed);
            }
            else if (Recooked > 0)
            {
                ImGuiX::Notifications::NotifySuccess("Recooked {0} texture(s).", Recooked);
            }
        }

        void CaptureValue(FProperty* Property, CObject* Object, TVector<uint8>& Out)
        {
            Out.clear();
            FMemoryWriter Writer(Out);
            FObjectProxyArchiver Ar(Writer, false);
            Property->Serialize(Ar, Property->GetValuePtr<void>(Object));
        }

        CClass* FindCommonClass(const TVector<CObject*>& Objects)
        {
            CClass* Common = Objects.empty() ? nullptr : Objects.front()->GetClass();
            for (CObject* Object : Objects)
            {
                while (Common != nullptr && !Object->GetClass()->IsChildOf(Common))
                {
                    Common = Common->GetSuperClass();
                }
            }
            return Common;
        }
    }

    // Every target's snapshot as one step, recooking after a restore so the pixels follow the restored settings.
    class FMatrixEditCommand final : public IUndoableCommand
    {
    public:

        FMatrixEditCommand(FName InName, const TVector<CObject*>& InTargets)
            : Name(InName)
            , Targets(InTargets)
        {
            for (CObject* Target : Targets)
            {
                Snapshots.push_back(MakeUnique<FObjectSnapshotCommand>(Target, InName));
            }
        }

        void SetProperty(FProperty* InProperty) { Property = InProperty; }

        void Undo() override
        {
            for (TUniquePtr<FObjectSnapshotCommand>& Snapshot : Snapshots)
            {
                Snapshot->Undo();
            }
            RecookAfterEdit(Targets, Property);
        }

        void Redo() override
        {
            for (TUniquePtr<FObjectSnapshotCommand>& Snapshot : Snapshots)
            {
                Snapshot->Redo();
            }
            RecookAfterEdit(Targets, Property);
        }

        void Finalize() override
        {
            for (TUniquePtr<FObjectSnapshotCommand>& Snapshot : Snapshots)
            {
                Snapshot->Finalize();
            }
        }

        bool IsNoOp() const override
        {
            return Algo::AllOf(Snapshots, [](const TUniquePtr<FObjectSnapshotCommand>& Snapshot) { return Snapshot->IsNoOp(); });
        }

        FName GetName() const override { return Name; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override
        {
            for (TUniquePtr<FObjectSnapshotCommand>& Snapshot : Snapshots)
            {
                Snapshot->VisitObjectReferences(Func);
            }
            for (CObject*& Target : Targets)
            {
                Target = Func(Target);
            }
        }

    private:

        FName                                       Name;
        TVector<CObject*>                           Targets;
        TVector<TUniquePtr<FObjectSnapshotCommand>> Snapshots;
        FProperty*                                  Property = nullptr;
    };

    FPropertyMatrixTool::FPropertyMatrixTool(IEditorToolContext* Context, TVector<FGuid> InAssets)
        : FEditorTool(Context, "Property Matrix", nullptr)
    {
        TVector<CObject*> Objects;
        for (const FGuid& Asset : InAssets)
        {
            if (CObject* Object = CPackage::LoadAssetGraph(Asset))
            {
                Objects.push_back(Object);
                Rows.push_back(FRow{ Object });
            }
        }

        CommonClass = FindCommonClass(Objects);
        if (CommonClass != nullptr)
        {
            for (FProperty* Property : CommonClass->GetProperties())
            {
                if (Property->IsVisible() && MakeInlinePropertyCustomization(Property) != nullptr)
                {
                    ColumnCandidates.push_back(Property);
                }
            }
            ToolName = FName(Format("Property Matrix ({0} {1})", Rows.size(), CommonClass->MakeDisplayName().c_str()).c_str());
        }

        Algo::Sort(Rows, [](const FRow& A, const FRow& B) { return A.Object->GetName().ToString() < B.Object->GetName().ToString(); });
        PinDefaultColumns();
        RefreshSelection();
    }

    void FPropertyMatrixTool::OnInitialize()
    {
        CreateToolWindow(GridWindowName, [this](bool) { DrawGrid(); });
        CreateToolWindow(DetailsWindowName, [this](bool) { DrawDetails(); });

        Details.SetEditMultipleValues(true);
        Details.SetStartEditCallback([this](const FPropertyChangedEvent& Event)
        {
            if (!Selection.empty())
            {
                BeginEdit(Event.PropertyName, Selection, Selection.front(), nullptr);
            }
        });
        Details.SetPostEditCallback([this](const FPropertyChangedEvent&) { SpreadEdit(); });
        Details.SetFinishEditCallback([this](const FPropertyChangedEvent&) { FinishEdit(); });

        Details.SetMultiValueFn([this](FProperty* Property)
        {
            return Algo::AnyOf(Selection, [this, Property](CObject* Object)
            {
                return !Property->Identical_InContainer(Selection.front(), Object);
            });
        });

        Details.SetTrailingControl([this](FProperty* Property)
        {
            if (!Algo::Contains(ColumnCandidates, Property))
            {
                return;
            }

            const bool bPinned = Algo::Contains(Columns, Property);
            ImGui::PushStyleColor(ImGuiCol_Text, bPinned ? EditorColors::Accent() : EditorColors::TextDim());
            if (ImGui::SmallButton(bPinned ? LE_ICON_PIN "##Pin" : LE_ICON_PIN_OUTLINE "##Pin"))
            {
                TogglePinned(Property);
            }
            ImGui::PopStyleColor();
            if (bPinned)
            {
                ImGuiX::TextTooltip("Remove this column from the grid");
            }
            else
            {
                ImGuiX::TextTooltip("Show this property as a column in the grid");
            }
        }, ImGui::GetFrameHeight());
    }

    void FPropertyMatrixTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FPropertyMatrixTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID GridDockID = 0;
        ImGuiID DetailsDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.32f, &DetailsDockID, &GridDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(GridWindowName).c_str(), GridDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(DetailsWindowName).c_str(), DetailsDockID);
    }

    void FPropertyMatrixTool::VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
    {
        FEditorTool::VisitObjectReferences(Func);
        for (FRow& Row : Rows)
        {
            Row.Object = Func(Row.Object.Get());
        }
        DropDeletedRows();
        RebuildCells();
        RefreshSelection();
    }

    void FPropertyMatrixTool::OnSave()
    {
        TVector<CPackage*> Packages;
        for (const FRow& Row : Rows)
        {
            Packages.push_back(Row.Object->GetPackage());
        }
        ToolContext->SavePackages(Move(Packages));
    }

    bool FPropertyMatrixTool::IsUnsavedDocument()
    {
        return Algo::AnyOf(Rows, [](const FRow& Row)
        {
            const CPackage* Package = Row.Object->GetPackage();
            return Package != nullptr && Package->IsDirty();
        });
    }

    void FPropertyMatrixTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::BeginMenu(LE_ICON_TABLE_COLUMN " Columns"))
        {
            DrawColumnsMenu();
            ImGui::EndMenu();
        }
    }

    void FPropertyMatrixTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Rows", "Every asset the matrix was opened on. Click to select, Ctrl+Click to toggle, Shift+Click for a range, double-click to open its editor.");
        DrawHelpTextRow("Columns", "Pin a property from the Selection panel or the Columns menu to show it as a column. Cells edit in place.");
        DrawHelpTextRow("Bulk edits", "The Selection panel edits every selected asset at once, and so does a cell in a selected row. Dimmed rows differ between the selected assets.");
        DrawHelpTextRow("Saving", "Edits mark the assets dirty. Save writes every one of them, and each edit is one undo step for all of them.");
    }

    void FPropertyMatrixTool::OnPostUndoRedo()
    {
        // A restore can resize containers the rows were built from, so the details rebuild rather than trust them.
        Details.MarkDirty();
    }

    void FPropertyMatrixTool::DrawColumnsMenu()
    {
        for (FProperty* Property : ColumnCandidates)
        {
            const bool bPinned = Algo::Contains(Columns, Property);
            if (ImGui::MenuItem(Property->GetPropertyDisplayName().c_str(), nullptr, bPinned))
            {
                TogglePinned(Property);
            }
        }
    }

    void FPropertyMatrixTool::DrawGrid()
    {
        DropDeletedRows();

        ImGui::SetNextItemWidth(260.0f * ImGuiX::GetUIScale());
        NameFilter.Draw(LE_ICON_MAGNIFY "##Filter");
        ImGui::SameLine();
        if (ImGui::Button("All"))
        {
            for (FRow& Row : Rows)
            {
                Row.bSelected = NameFilter.PassFilter(Row.Object->GetName().c_str());
            }
            RefreshSelection();
        }
        ImGuiX::TextTooltip("Select every row the filter shows");
        ImGui::SameLine();
        if (ImGui::Button("None"))
        {
            for (FRow& Row : Rows)
            {
                Row.bSelected = false;
            }
            RefreshSelection();
        }
        ImGui::SameLine();
        ImGui::TextColored(EditorColors::TextDim(), "%d of %d selected", (int32)Selection.size(), (int32)Rows.size());

        TVector<int32> Visible;
        Visible.reserve(Rows.size());
        for (int32 Index = 0; Index < (int32)Rows.size(); ++Index)
        {
            if (NameFilter.PassFilter(Rows[Index].Object->GetName().c_str()))
            {
                Visible.push_back(Index);
            }
        }

        constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY
            | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;

        if (!ImGui::BeginTable("##Matrix", 1 + (int32)Columns.size(), TableFlags))
        {
            return;
        }

        const float Scale = ImGuiX::GetUIScale();
        ImGui::TableSetupScrollFreeze(1, 1);
        // No initial width, so the fixed-fit table sizes the column to the longest name.
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_DefaultSort);
        for (FProperty* Property : Columns)
        {
            ImGui::TableSetupColumn(Property->GetPropertyDisplayName().c_str(), ImGuiTableColumnFlags_NoSort, 170.0f * Scale);
        }
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs(); Specs != nullptr && Specs->SpecsDirty && Specs->SpecsCount > 0)
        {
            const bool bAscending = Specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            Algo::Sort(Rows, [bAscending](const FRow& A, const FRow& B)
            {
                const FString NameA = A.Object->GetName().ToString();
                const FString NameB = B.Object->GetName().ToString();
                return bAscending ? NameA < NameB : NameB < NameA;
            });
            Specs->SpecsDirty = false;
            RefreshSelection();
            ImGui::EndTable();
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        ImGuiListClipper Clipper;
        Clipper.Begin((int32)Visible.size());
        while (Clipper.Step())
        {
            for (int32 Position = Clipper.DisplayStart; Position < Clipper.DisplayEnd; ++Position)
            {
                FRow& Row = Rows[Visible[Position]];
                ImGui::TableNextRow();
                ImGui::PushID(Visible[Position]);

                ImGui::TableSetColumnIndex(0);
                constexpr ImGuiSelectableFlags RowFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick;
                if (ImGui::Selectable(Row.Object->GetName().c_str(), Row.bSelected, RowFlags, ImVec2(0.0f, ImGui::GetFrameHeight())))
                {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        ToolContext->OpenAssetEditor(Row.Object->GetGUID());
                    }
                    else
                    {
                        const ImGuiIO& IO = ImGui::GetIO();
                        SelectRow(Visible, Position, IO.KeyCtrl, IO.KeyShift);
                    }
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                {
                    ImGuiX::TextTooltip("{0}", Row.Object->GetPackage() != nullptr ? Row.Object->GetPackage()->GetName().c_str() : "");
                }

                for (int32 Column = 0; Column < (int32)Columns.size(); ++Column)
                {
                    ImGui::TableSetColumnIndex(1 + Column);
                    DrawCell(Row, Column);
                }

                ImGui::PopID();
            }
        }
        ImGui::PopStyleVar();

        ImGui::EndTable();
    }

    void FPropertyMatrixTool::DrawCell(FRow& Row, int32 Column)
    {
        FCell& Cell = Row.Cells[Column];
        FProperty* Property = Columns[Column];

        ImGui::PushID(Column);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const FPropertyDrawArgs Args{ Property->IsReadOnly() };
        const EPropertyChangeOp Op = Cell.Customization->UpdateAndDraw(Cell.Handle, Args);
        ImGui::PopID();

        if (Op == EPropertyChangeOp::None)
        {
            return;
        }

        if (!Edit.has_value())
        {
            TVector<CObject*> Targets = Row.bSelected ? Selection : TVector<CObject*>{ Row.Object.Get() };
            BeginEdit(Property->Name, Move(Targets), Row.Object.Get(), Property);
        }

        Cell.Customization->UpdatePropertyValue(Cell.Handle);
        SpreadEdit();

        if (Op == EPropertyChangeOp::Finished)
        {
            FinishEdit();
        }
    }

    void FPropertyMatrixTool::DrawDetails()
    {
        if (Selection.empty())
        {
            ImGui::TextColored(EditorColors::TextDim(), "Select rows to edit them together.");
            return;
        }

        if (Details.GetObject() != Selection.front())
        {
            Details.SetObject(Selection.front(), CommonClass, CommonClass->GetDefaultObject());
        }

        if (Selection.size() == 1)
        {
            ImGui::TextColored(EditorColors::TextDim(), "Editing %s", Selection.front()->GetName().c_str());
        }
        else
        {
            ImGui::TextColored(EditorColors::TextDim(), "Editing %d assets", (int32)Selection.size());
        }
        Details.DrawTree();
    }

    void FPropertyMatrixTool::RebuildCells()
    {
        CObject* Defaults = CommonClass != nullptr ? CommonClass->GetDefaultObject() : nullptr;
        for (FRow& Row : Rows)
        {
            Row.Cells.clear();
            for (FProperty* Property : Columns)
            {
                Row.Cells.push_back(FCell{ MakeShared<FPropertyHandle>(Row.Object.Get(), Defaults, Property), MakeInlinePropertyCustomization(Property) });
            }
        }
    }

    void FPropertyMatrixTool::RefreshSelection()
    {
        Selection.clear();
        for (const FRow& Row : Rows)
        {
            if (Row.bSelected)
            {
                Selection.push_back(Row.Object.Get());
            }
        }
    }

    void FPropertyMatrixTool::PinDefaultColumns()
    {
        // The properties the assets disagree on are the ones worth comparing first.
        for (FProperty* Property : ColumnCandidates)
        {
            const bool bDiffers = Algo::AnyOf(Rows, [&](const FRow& Row)
            {
                return !Property->Identical_InContainer(Rows.front().Object.Get(), Row.Object.Get());
            });
            if (bDiffers && (int32)Columns.size() < kMaxDefaultColumns)
            {
                Columns.push_back(Property);
            }
        }

        for (int32 Index = 0; Columns.empty() && Index < (int32)ColumnCandidates.size() && Index < kFallbackColumns; ++Index)
        {
            Columns.push_back(ColumnCandidates[Index]);
        }
        RebuildCells();
    }

    void FPropertyMatrixTool::TogglePinned(FProperty* Property)
    {
        if (Algo::Contains(Columns, Property))
        {
            Columns.erase(Algo::Remove(Columns, Property), Columns.end());
        }
        else
        {
            Columns.push_back(Property);
        }
        RebuildCells();
    }

    void FPropertyMatrixTool::DropDeletedRows()
    {
        const auto IsGone = [](const FRow& Row)
        {
            return Row.Object == nullptr || Row.Object->GetPackage() == nullptr || Row.Object->GetPackage()->HasAnyFlag(OF_MarkedDestroy);
        };
        if (Algo::AnyOf(Rows, IsGone))
        {
            Rows.erase(Algo::RemoveIf(Rows, IsGone), Rows.end());
            RefreshSelection();
            Details.SetObject(nullptr, nullptr);
        }
    }

    void FPropertyMatrixTool::SelectRow(const TVector<int32>& Visible, int32 Position, bool bToggle, bool bRange)
    {
        FRow& Clicked = Rows[Visible[Position]];
        const int32 Anchor = Algo::IndexOf(Visible, LastClickedAsset, [this](int32 Index) { return Rows[Index].Object->GetGUID(); });

        if (bRange && Anchor != Constants::kIndexNone)
        {
            if (!bToggle)
            {
                for (FRow& Row : Rows)
                {
                    Row.bSelected = false;
                }
            }
            for (int32 Index = Math::Min(Anchor, Position); Index <= Math::Max(Anchor, Position); ++Index)
            {
                Rows[Visible[Index]].bSelected = true;
            }
        }
        else if (bToggle)
        {
            Clicked.bSelected = !Clicked.bSelected;
            LastClickedAsset = Clicked.Object->GetGUID();
        }
        else
        {
            for (FRow& Row : Rows)
            {
                Row.bSelected = &Row == &Clicked;
            }
            LastClickedAsset = Clicked.Object->GetGUID();
        }

        RefreshSelection();
    }

    void FPropertyMatrixTool::BeginEdit(FName Label, TVector<CObject*> Targets, CObject* Source, FProperty* Property)
    {
        FBulkEdit& Open = Edit.emplace();
        Open.Targets = Move(Targets);
        Open.Source = Source;
        Open.Property = Property;

        if (Property == nullptr)
        {
            for (FProperty* Candidate : CommonClass->GetProperties())
            {
                CaptureValue(Candidate, Source, Open.SourceBaseline.emplace_back());
            }
        }

        TUniquePtr<FMatrixEditCommand> Command = MakeUnique<FMatrixEditCommand>(Label, Open.Targets);
        Command->SetProperty(Property);
        Open.Command = Command.get();

        TransactionManager.BeginTransaction(Label);
        TransactionManager.Record(Move(Command));
    }

    void FPropertyMatrixTool::SpreadEdit()
    {
        if (!Edit.has_value())
        {
            return;
        }

        if (Edit->Property == nullptr)
        {
            Edit->Property = FindChangedProperty();
            Edit->Command->SetProperty(Edit->Property);
        }

        if (Edit->Property == nullptr)
        {
            return;
        }

        for (CObject* Target : Edit->Targets)
        {
            if (Target != Edit->Source)
            {
                Edit->Property->CopyCompleteValue_InContainer(Target, Edit->Source);
            }
        }
    }

    void FPropertyMatrixTool::FinishEdit()
    {
        if (!Edit.has_value())
        {
            return;
        }

        const TVector<CObject*> Targets = Move(Edit->Targets);
        FProperty* Property = Edit->Property;
        Edit.reset();

        TransactionManager.CommitTransaction();
        if (Property == nullptr)
        {
            return;
        }

        for (CObject* Target : Targets)
        {
            Target->PostPropertyChange(Property);
            if (CPackage* Package = Target->GetPackage())
            {
                Package->MarkDirty();
            }
        }

        RecookAfterEdit(Targets, Property);

        // A container another row received has new elements, and the details rows were built over the old ones.
        Details.MarkDirty();
    }

    FProperty* FPropertyMatrixTool::FindChangedProperty() const
    {
        TVector<uint8> Current;
        const TSpan<FProperty* const> Properties = CommonClass->GetProperties();
        for (int32 Index = 0; Index < (int32)Properties.size(); ++Index)
        {
            CaptureValue(Properties[Index], Edit->Source, Current);
            if (Current != Edit->SourceBaseline[Index])
            {
                return Properties[Index];
            }
        }
        return nullptr;
    }
}
