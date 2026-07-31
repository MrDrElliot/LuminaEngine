#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "Particles/ParticleModule.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CParticleEmitterStack;
    class CParticleModule;

    class FParticleSystemEditorTool : public FAssetEditorTool
    {
    public:

        struct FCompilationResultInfo
        {
            FString CompilationLog;
            bool bIsError = false;
        };

        LUMINA_EDITOR_TOOL(FParticleSystemEditorTool)

        FParticleSystemEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_SHIMMER; }

        void OnInitialize() override;

        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void SetupWorldForTool() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        void Compile();
        void OnSave() override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

    private:

        void DrawStack();
        void DrawStackSection(EParticleModuleStage Stage, const char* Label);
        void DrawAddModulePopup(EParticleModuleStage Stage);
        void DrawSystemProperties();

        void SelectModule(CParticleModule* Module);

    private:

        entt::entity            ParticleEntity;
        entt::entity            DirectionalLightEntity;

        CParticleModule*        SelectedModule = nullptr;
        FCompilationResultInfo  CompilationResult;

        // Set when the stack changes structurally (add / remove / reorder / toggle); consumed at the
        // end of DrawStack to recompile once, after the UI loop rather than mid-iteration.
        bool                    bStackDirty = false;

        // Stage whose "+ Add Module" popup is open this frame.
        EParticleModuleStage    PendingAddStage = EParticleModuleStage::Spawn;

        TObjectPtr<CParticleEmitterStack> EmitterStack;
    };
}
