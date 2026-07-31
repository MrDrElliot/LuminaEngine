#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class FSkeletalMeshEditorTool : public FAssetEditorTool
    {
    public:
        
        FStringView MeshPropertiesName = "MeshProperties";

        LUMINA_EDITOR_TOOL(FSkeletalMeshEditorTool)
        
        FSkeletalMeshEditorTool(IEditorToolContext* Context, CObject* InAsset);


        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_HUMAN; }
        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void OnAssetLoadFinished() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

        bool bShowBones = false;
        bool bShowAABB = false;
        bool bShowWireframe = false;
        bool bShowNormals = false;
        bool bShowTangents = false;
        
        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        entt::entity DirectionalLightEntity = entt::null;
        entt::entity MeshEntity = entt::null;
    };
}
