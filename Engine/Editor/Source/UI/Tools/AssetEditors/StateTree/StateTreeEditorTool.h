#pragma once

#include "StateTreeDebugger.h"
#include "StateTreeEditing.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "UI/Widgets/CategorizedPicker.h"
#include "imgui.h"

namespace Lumina
{
    // States as an outline of cards with their transitions drawn as arrows, the selection's details beside it and the compile result under it.
    class EDITOR_API FStateTreeEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FStateTreeEditorTool)

        FStateTreeEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_STATE_MACHINE; }

        void OnInitialize() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        void OnPostUndoRedo() override;

    private:

        friend struct FStateTreeEditorToolTestAccess;

        // A card's place this frame, for arrows and drops.
        struct FCardLayout
        {
            ImVec2         Min;
            ImVec2         Max;
            float          HeaderY = 0.0f;
            TVector<float> TransitionY;
            bool           bVisible = false;
        };

        CStateTree* GetTree() { return GetAsset<CStateTree>(); }

        // Every edit goes through here, so it is dirtied for undo and saving and the validation strip stays live.
        void Edit();
        void Select(const FStateTreeSelection& InSelection);
        void ActivateIssue(int32 IssueIndex);

        void AddState(FName Parent, int32 InsertAt);
        void AddNode(EStateTreeSlot Slot, CStruct* Type);
        void AddTransition(int32 State);
        void CopySelection();
        void PasteClipboard();
        void DeleteSelection();
        void DuplicateSelection();
        void ExtractToLinkedSubtree(int32 State);
        void BeginRename(int32 State);

        void DrawOutline();
        void DrawOutlineToolbar(CStateTree& Tree);
        void DrawStates(CStateTree& Tree, FName Parent, int32 Depth, TVector<bool>& Drawn, bool bShown);
        void DrawStateCard(CStateTree& Tree, int32 State, int32 Depth);
        void DrawStateContextMenu(CStateTree& Tree, int32 State);
        void AcceptStateDrop(CStateTree& Tree, int32 State);
        void DrawTransitionArrows(const CStateTree& Tree);
        void DrawValidation();
        void HandleOutlineShortcuts();

        void DrawDetails();
        void DrawBindings(CStateTree& Tree, const FStateTreeSelection& Reader);
        // A link button over the sources Target can read, returning the one picked.
        const SStateTreeBindingPath* DrawBindingPicker(const char* StrId, const FString& Label, const CStateTree& Tree, const FStateTreeSelection& Reader, const FProperty& Target, bool bAllowFunctions);
        CStruct* DrawNodePicker(FCategorizedPicker& Picker, const char* Label, EStateTreeSlot Slot);
        void DrawParameters();
        void DrawEvaluators();

        FStateTreeSelection         Selection;
        TVector<FStateTreeIssue>    Issues;
        TVector<FName>              CollapsedStates;
        TVector<FCardLayout>        Cards;

        // Outline edits wait for the cards to finish drawing, since they reorder the states being drawn.
        TFunction<void()>           DeferredEdit;

        int32                       RenamingState = Constants::kIndexNone;
        char                        RenameBuffer[128] = {};
        bool                        bFocusRename = false;
        bool                        bScrollToSelection = false;

        // The selected state's name before the details panel edits it, so a rename there carries its references along.
        FName                       DetailsStateName;

        TVector<SStateTreeState>    ClipboardStates;
        FInstancedStruct            ClipboardNode;

        FPropertyTable              DetailsTable;
        FPropertyTable              ParametersTable;
        FCategorizedPicker          TaskPicker;
        FCategorizedPicker          ConditionPicker;
        FCategorizedPicker          EvaluatorPicker;
        FCategorizedPicker          BindingPicker;
        TVector<FStateTreeBindingSource> BindingSources;
        FStateTreeDebugger          Debugger;
    };
}
