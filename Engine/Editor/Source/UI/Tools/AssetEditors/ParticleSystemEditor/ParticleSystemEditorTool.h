#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "Particles/ParticleModule.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CParticleEmitter;
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
        void Update(const FUpdateContext& UpdateContext) override;
        void SetupWorldForTool() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

        /** Full rebuild of EVERY emitter: regenerates the HLSL, invokes Slang, and swaps the shader entry.
         *  Needed only for structural stack changes (add/remove/reorder/toggle) and for inputs that alter
         *  generated code. Emitters compile independently -- one failing does not stop the others, so a
         *  broken emitter cannot take the whole effect offline. */
        void Compile();

        /** Value-only update: re-collects the module parameter slots and writes them to the asset without
         *  touching the shader, so the running preview keeps its particles. Returns false when the layout
         *  no longer matches the compiled shader, meaning the caller must fall back to Compile(). */
        bool RefreshModuleParams();
        void OnSave() override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }

    private:

        void DrawStack();
        void DrawEmitterSection(int32 EmitterIndex);
        void DrawStackSection(int32 EmitterIndex, EParticleModuleStage Stage, const char* Label);
        void DrawAddModulePopup(int32 EmitterIndex, EParticleModuleStage Stage);
        void DrawSystemProperties();

        /** Points the Details panel at an object: the system, an emitter, or a module. */
        void SelectObject(CObject* Object);
        void SelectModule(CParticleModule* Module) { SelectObject((CObject*)Module); }

        /** Brings EmitterStacks into line with the asset's emitter list, creating any stack that does not
         *  exist yet. Cheap and idempotent; called before anything walks the stacks. */
        void SyncEmitterStacks();

        /** Rescans reflection for every concrete CParticleModule subclass. Called when the add-module
         *  popup opens rather than cached once for the process: a plugin loaded (or unloaded) mid-session
         *  changes the answer, and a one-shot static would show a stale palette or reference a class whose
         *  module has since been unloaded. One scan per click is not worth caching around. */
        void RefreshModulePalette();

        /** Authoring stack for an emitter, or null when the index is out of range. */
        CParticleEmitterStack* GetStackFor(int32 EmitterIndex) const;

        /** Compiles one emitter's stacks into its own shader. Returns false on a compile error, having
         *  appended the reason to CompilationResult. */
        bool CompileEmitter(int32 EmitterIndex);

    private:

        entt::entity            ParticleEntity;
        entt::entity            DirectionalLightEntity;

        // The object the Details panel edits. Raw, not a TObjectPtr: modules and emitters are owned by the
        // stack and the asset, and a strong ref here would keep a deleted one alive behind the panel.
        CObject*                SelectedObject = nullptr;
        // Non-null only when SelectedObject is a module; the module-specific paths key off this.
        CParticleModule*        SelectedModule = nullptr;
        // Emitter whose column owns the selection, and the target of the add-module popup.
        int32                   SelectedEmitter = 0;
        FCompilationResultInfo  CompilationResult;

        // Emitter whose stack changed structurally (add / remove / reorder / toggle) this frame, or
        // INDEX_NONE. Consumed at the end of DrawStack so the recompile happens after the UI loop rather
        // than mid-iteration. Tracked per emitter rather than as a flag because a rebuild swaps the
        // FShaderEntry the dispatch binds, which restarts that emitter's particles -- recompiling all of
        // them would make editing one column visibly reset every other column.
        int32                   DirtyEmitter = INDEX_NONE;

        // Emitter the delete button asked to remove; applied after the column loop. Removing mid-loop
        // would shift every later column's index out from under the emitter it is drawing.
        CParticleEmitter*       PendingRemoveEmitter = nullptr;

        // Concrete module classes discovered by reflection, rebuilt each time the palette opens.
        TVector<CClass*>        ModulePalette;

        // Stage and emitter whose "+ Add Module" popup is open this frame.
        EParticleModuleStage    PendingAddStage   = EParticleModuleStage::Spawn;
        int32                   PendingAddEmitter = 0;

        // Parallel to the asset's Emitters, one authoring stack each. Rebuilt by SyncEmitterStacks rather
        // than maintained at every mutation site, so an emitter added anywhere still gets a stack.
        TVector<TObjectPtr<CParticleEmitterStack>> EmitterStacks;
    };
}
