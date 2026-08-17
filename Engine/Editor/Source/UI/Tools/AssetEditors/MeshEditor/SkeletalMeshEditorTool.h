#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"
#include "MeshUVViewer.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CSkeleton;
    struct FSkeletonResource;

    /** What one bone actually drives, summed over the LOD 0 meshlet vertices. */
    struct FBoneInfluenceStats
    {
        uint32 VertexCount = 0;
        float  MaxWeight   = 0.0f;
        float  TotalWeight = 0.0f;
    };

    /** Skinning health for the open mesh, rebuilt only when the mesh resource or its skeleton changes. */
    struct FSkinningAnalysis
    {
        const CMesh*     AnalyzedMesh     = nullptr;
        const CSkeleton* AnalyzedSkeleton = nullptr;
        bool             bValid           = false;

        uint32 LOD0Vertices = 0;

        /** Highest weighted joint index + 1 over EVERY LOD, because the GPU bone fetch is unbounded. */
        uint32 RequiredBones = 0;

        // Indexed by how many non-zero weights a vertex has, 0 through 4.
        uint32 InfluenceHistogram[5] = {};

        // Weighted to a joint the skeleton has no bone for; these skin to garbage.
        uint32 OutOfRangeVertices = 0;

        // Fully rigid to bone 0, the shape a vertex collapses to when its weights arrived empty.
        uint32 RigidToRootVertices = 0;

        // Weights that do not sum to 255; a shortfall drags the vertex toward the origin.
        uint32 UnnormalizedVertices = 0;

        TVector<FBoneInfluenceStats> PerBone;
        uint32 UnusedBones = 0;
    };

    class FSkeletalMeshEditorTool : public FAssetEditorTool
    {
    public:

        FStringView MeshPropertiesName = "MeshProperties";
        FStringView UVViewerName = "UVs";
        FStringView SkinningName = "Skinning";

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

        // Mesh-specific rows inside the shared Visualize menu (was the separate "Mesh Debug" menu).
        void DrawViewModeExtraItems() override;
        void DrawHelpMenu() override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void OnAssetDataChangedExternally() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

        bool bShowBones = false;
        bool bShowAABB = false;
        bool bShowSockets = true;

        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        entt::entity DirectionalLightEntity = entt::null;
        entt::entity MeshEntity = entt::null;

        // Unwrapped-UV inspector; owns its own view state (zoom/pan/LOD) across frames.
        FMeshUVViewer UVViewer;

    private:

        void  RebuildSkinningAnalysis();
        void  RebuildSelectedBonePoints();
        void  DrawSkinningPanel();
        float GetBoneDrawRadius() const;

        /** Bind-pose bone transforms in world space, sized to the skeleton; empty when there is none. */
        void  BuildBonePoseTransforms(TVector<FMatrix4>& OutTransforms) const;

        FSkinningAnalysis Skinning;

        // -1 = none. Drives the viewport bone highlight and the influenced-vertex overlay.
        int32 SelectedBoneIndex = -1;

        /** Mesh-local LOD 0 positions the selected bone drives, capped and cached against re-walking. */
        TVector<FVector3> SelectedBonePoints;
        int32  PointsCachedForBone = -1;
        uint32 SelectedBoneTotalPoints = 0;

        static constexpr uint32 kMaxInfluencePoints = 6000;

        // Selected surface in the Geometry Surfaces UI (-1 = none); drawn as a colored AABB overlay.
        int32 SelectedSurfaceIndex = -1;

        // -1 = automatic (distance-driven), 0..MAX_MESH_LODS-1 = forced.
        int32 PreviewLODIndex = -1;
    };
}
