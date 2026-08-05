#pragma once
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
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
        const char* GetTitlebarIcon() const override { return LE_ICON_PALETTE_SWATCH; }
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

        // Standard object picker, shared across every texture row. The parameter table only exposes each
        // slot through CMaterialInstance's override list, so the picker is driven by a handle synthesized
        // over Scratch, which is seeded from the row's resolved texture before the draw and read back after.
        // One instance is safe because UpdateAndDraw re-syncs from the handle each frame, and the caller
        // already pushes a per-parameter ImGui ID.
        TSharedPtr<IPropertyTypeCustomization> TexturePicker;
        TSharedPtr<FPropertyHandle>            TextureHandle;
        FMaterialParameterOverride             TextureScratch;
    };
}
