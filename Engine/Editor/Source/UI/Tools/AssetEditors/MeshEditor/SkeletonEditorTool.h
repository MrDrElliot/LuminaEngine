#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    struct FSkeletonResource;

    class FSkeletonEditorTool : public FAssetEditorTool
    {
    public:
        
        LUMINA_EDITOR_TOOL(FSkeletonEditorTool)
        
        FSkeletonEditorTool(IEditorToolContext* Context, CObject* InAsset);


        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_BONE; }
        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void OnAssetLoadFinished() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        static void DrawBoneHierarchy(CWorld* DrawWorld, FSkeletonResource* SkeletonResource, const TVector<FMatrix4>& WorldTransforms, int32 BoneIndex);

        
        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        entt::entity DirectionalLightEntity = entt::null;
        entt::entity MeshEntity = entt::null;
        
        FTreeListViewContext BoneListContext;
        FTreeListView BoneListView;
        
        FName SelectedBone;
        
    };
}
