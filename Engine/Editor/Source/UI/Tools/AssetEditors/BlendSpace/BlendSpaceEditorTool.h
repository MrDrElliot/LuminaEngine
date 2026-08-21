#pragma once

#define USE_IMGUI_API
#include <imgui.h>

#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"
#include "Core/Math/Math.h"
#include "Memory/SmartPtr.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    class CSkeleton;
    struct FSkeletonResource;

    // Editor for CBlendSpace: the grid is the asset. Samples are dragged where they belong, the preview
    // cursor is dropped anywhere between them, and the mesh in the viewport plays exactly what the runtime
    // would produce at that position.
    class FBlendSpaceEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FBlendSpaceEditorTool)

        FBlendSpaceEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CHART_SCATTER_PLOT; }

        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void OnAssetDataChangedExternally() override;
        void OnPostUndoRedo() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        void DrawGridWindow();
        void DrawDetailsWindow();

        // The grid, in draw order: background, gridlines, triangulation, samples, cursor.
        void DrawGridCanvas();
        void HandleGridInput();

        CSkeleton* GetSkeleton();
        FSkeletonResource* GetSkeletonResource();

        void RefreshPreviewMesh();

        // Runs the same weight -> sample -> fold recipe the VM's SampleBlendSpace opcode does, so what
        // the viewport shows is what the runtime will produce rather than an editor-only approximation.
        void EvaluatePreviewPose(float DeltaTime);

        ImVec2 AxisToCanvas(const FVector2& AxisPosition) const;
        FVector2 CanvasToAxis(const ImVec2& CanvasPosition) const;

        // Ctrl inverts the toggle rather than forcing snapping on, so the one modifier both enables it
        // when the toggle is off and suspends it when the toggle is on.
        bool IsSnapActive() const;
        FVector2 SnapToGrid(const FVector2& AxisPosition) const;

        int32 FindSampleAtCanvasPos(const ImVec2& CanvasPosition) const;

        void AddSampleAt(const FVector2& AxisPosition);
        void RemoveSampleAt(int32 SampleIndex);

        void SyncSampleTable();

        // Forces the rebind SyncSampleTable's address compare would miss when the block is reused.
        void RebindSampleTable();

        void RefreshAfterStructuralEdit();

        void BeginAssetTransaction(FName Name);
        void EndAssetTransaction();

        TUniquePtr<FPropertyTable>  SampleTable;
        void*                       SampleTarget = nullptr;
        int32                       SampleTableSourceCount = 0;

        // Canvas rect in screen space, refreshed every draw; the axis<->canvas mapping reads it.
        ImVec2                      CanvasMin = ImVec2(0.0f, 0.0f);
        ImVec2                      CanvasSize = ImVec2(1.0f, 1.0f);

        entt::entity                MeshEntity = entt::null;
        entt::entity                LightEntity = entt::null;

        FVector2                    PreviewPosition = FVector2(0.0f, 0.0f);
        float                       PreviewPhase = 0.0f;
        float                       PlayRate = 1.0f;

        int32                       SelectedSample = INDEX_NONE;
        int32                       DraggedSample = INDEX_NONE;

        TObjectPtr<CSkeleton>       CachedSkeleton;

        // Snapping and the drawn gridlines share this, so samples land on the lines you can actually see.
        int32                       SnapDivisions = 10;

        uint8                       bSnapEnabled:1 = true;
        uint8                       bPlaying:1 = true;
        uint8                       bShowTriangulation:1 = true;
        uint8                       bShowWeights:1 = true;
        uint8                       bDragTransactionOpen:1 = false;
    };
}
