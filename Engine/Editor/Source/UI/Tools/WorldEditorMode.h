#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "World/ECS/Entity.h"

namespace Lumina
{
    class CWorld;
    struct SCameraComponent;

    // What a mode may ask of its host, so it can wrap an interaction in undo without knowing the tool.
    class IEditorModeContext
    {
    public:

        virtual ~IEditorModeContext() = default;

        virtual void BeginModeTransaction() = 0;
        virtual void EndModeTransaction(const char* Name) = 0;

        // Cheaper, for an interaction that only adds entities; EndModeTransaction closes it.
        virtual void BeginModeCreationTransaction() = 0;

        virtual ECS::FEntity GetModeSelectionFocus() const = 0;
        virtual void SetModeSelection(ECS::FEntity Entity) = 0;
    };

    // One exclusive viewport input mode; the host routes tick, overlay and toolbar to the active one.
    class IWorldEditorMode
    {
    public:

        virtual ~IWorldEditorMode() = default;

        virtual const char* GetDisplayName() const = 0;
        virtual const char* GetIcon() const { return LE_ICON_CURSOR_DEFAULT; }
        virtual const char* GetTooltip() const { return nullptr; }
        virtual void OnEnter(CWorld* World) {}
        virtual void OnExit(CWorld* World) {}
        virtual void Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize) {}
        virtual void DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera) {}
        virtual void DrawToolbar(CWorld* World, float ButtonSize) {}

        // True when the mode owns viewport input outright, so the host drops its own selection, marquee and gizmo.
        virtual bool ConsumesViewportInput() const { return true; }

        void SetContext(IEditorModeContext* InContext) { Context = InContext; }

    protected:

        IEditorModeContext* Context = nullptr;
    };

    // The registry's stable first entry, whose picking, marquee and gizmo the world editor handles itself.
    class FSelectionEditorMode final : public IWorldEditorMode
    {
    public:

        const char* GetDisplayName() const override { return "Select"; }
        const char* GetIcon() const override { return LE_ICON_CURSOR_DEFAULT; }
        const char* GetTooltip() const override { return "Click to pick entities; drag to marquee-select; use the transform gizmo."; }
        bool ConsumesViewportInput() const override { return false; }
    };
}
