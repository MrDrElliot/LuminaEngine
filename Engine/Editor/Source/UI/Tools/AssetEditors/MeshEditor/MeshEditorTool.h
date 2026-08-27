#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>

#include "ImGuizmo.h"
#include "MeshUVViewer.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class FStaticMeshEditorTool : public FAssetEditorTool
    {
    public:

        FStringView MeshPropertiesName = "MeshProperties";
        FStringView UVViewerName = "UVs";
        
        LUMINA_EDITOR_TOOL(FStaticMeshEditorTool)
        
        FStaticMeshEditorTool(IEditorToolContext* Context, CObject* InAsset);


        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CUBE; }
        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void OnAssetLoadFinished() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;

        // Mesh-specific rows inside the shared Visualize menu (was the separate "Mesh Debug" menu).
        void DrawViewModeExtraItems() override;
        void DrawHelpMenu() override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void OnAssetDataChangedExternally() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

        bool bShowAABB = false;
        bool bShowWireframe = false;
        bool bShowNormals = false;
        bool bShowTangents = false;

        // Selected surface in the Geometry Surfaces UI (-1 = none); drawn as a colored
        // AABB overlay in Update() to show which part of the mesh it covers.
        int32 SelectedSurfaceIndex = -1;

        // -1 = automatic (distance-driven), 0..MAX_MESH_LODS-1 = forced.
        int32 PreviewLODIndex = -1;

        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        ECS::FEntity DirectionalLightEntity = ECS::NullEntity;
        ECS::FEntity MeshEntity = ECS::NullEntity;

        // Unwrapped-UV inspector; owns its own view state (zoom/pan/LOD) across frames.
        FMeshUVViewer UVViewer;
    };
}
