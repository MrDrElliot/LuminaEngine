#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "UI/Properties/PropertyEditContexts.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "SocketHierarchy.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    struct FSkeletonResource;
    class CSkeletalMesh;

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
        void OnPropertyEditFinished(const FPropertyChangedEvent& Event) override;
        void OnPostUndoRedo() override;
        void OnAssetDataChangedExternally() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        static void DrawBoneHierarchy(CWorld* DrawWorld, FSkeletonResource* SkeletonResource, const TVector<FMatrix4>& WorldTransforms, int32 BoneIndex);


        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        ECS::FEntity DirectionalLightEntity = ECS::NullEntity;
        ECS::FEntity MeshEntity = ECS::NullEntity;
        TWeakObjectPtr<CSkeletalMesh> PreviewMeshInUse;

        void RefreshPreviewMesh();
        
        FPropertyEditContext PropertyContext;
        FSkeletonEditContext SkeletonCtx;

        FSocketHierarchy SocketHierarchy;

        // Bind pose in world space, rebuilt each Update for the bone overlay and the socket hierarchy.
        TVector<FMatrix4> BoneWorldTransforms;
    };
}
