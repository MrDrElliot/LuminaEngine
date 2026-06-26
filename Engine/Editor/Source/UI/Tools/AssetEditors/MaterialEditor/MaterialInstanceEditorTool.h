#pragma once
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include <entt/entt.hpp>

namespace Lumina
{
    class FMaterialInstanceEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FMaterialInstanceEditorTool)

        FMaterialInstanceEditorTool(IEditorToolContext* Context, CObject* InAsset);

        enum class EDebugMesh : uint8
        {
            Sphere,
            Cube,
            Plane,
            Cylinder,
            Cone,
        };

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FORMAT_LIST_BULLETED_TYPE; }
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;

        void OnAssetLoadFinished() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        void DrawParameterEditor(bool bFocused);
        void DrawTextureParameterColumn(class CMaterialInstance* Instance, const struct FMaterialParameter& Param, bool bEnabled);
        void SetDebugMesh(EDebugMesh Mesh);

        entt::entity MeshEntity;
        entt::entity DirectionalLightEntity;
        EDebugMesh   DebugMesh = EDebugMesh::Sphere;

        ImGuiTextFilter TexturePickerFilter;
    };
}
