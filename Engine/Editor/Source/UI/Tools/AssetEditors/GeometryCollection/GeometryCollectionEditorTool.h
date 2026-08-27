#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CStaticMesh;

    // Editor for CGeometryCollection: pick mesh + fracture settings, Generate bakes convex
    // Voronoi chunks, preview the break with an explode slider. Runs in its own preview world.
    class FGeometryCollectionEditorTool : public FAssetEditorTool
    {
    public:

        FStringView FractureWindowName = "Fracture";

        LUMINA_EDITOR_TOOL(FGeometryCollectionEditorTool)

        FGeometryCollectionEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CUBE_OUTLINE; }
        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

    private:

        void DrawFractureWindow();
        void RebuildPreview();          // rebuild piece meshes + (re)spawn preview entities
        void ClearPreviewEntities();
        void ApplyExplode();            // offset each piece outward by ExplodeAmount

        TVector<TObjectPtr<CStaticMesh>> PieceMeshes;     // aligned with piece index (null = build failed)
        TVector<ECS::FEntity>            PieceEntities;    // aligned with piece index (null = no entity)
        ECS::FEntity                     LightEntity = ECS::NullEntity;
        float                            ExplodeAmount = 0.0f;
        bool                             bPreviewDirty = true;
    };
}
