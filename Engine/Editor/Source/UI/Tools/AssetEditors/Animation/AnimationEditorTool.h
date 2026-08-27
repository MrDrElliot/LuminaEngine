#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>

#include "ImGuizmo.h"
#include "UI/CurveEditor/CurveEditorWidget.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CAnimation;
    struct FAnimationResource;
    struct SSimpleAnimationComponent;

    class FAnimationEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FAnimationEditorTool)

        FAnimationEditorTool(IEditorToolContext* Context, CObject* InAsset);


        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_ANIMATION; }
        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void OnAssetLoadFinished() override;
        void OnPropertyEditFinished(const FPropertyChangedEvent& Event) override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        // What kind of notify a selection/drag refers to.
        enum class ENotifyKind : uint8 { None, Point, State };

        // Active timeline drag interaction.
        enum class EDragMode : uint8 { None, Playhead, MoveItem, ResizeStart, ResizeEnd };

        // Panels
        void DrawSequencer();
        void DrawTransport(SSimpleAnimationComponent* AnimComp, float Duration);
        void DrawNotifyTimeline(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration);
        void DrawNotifyInspector(CAnimation* Animation);
        void DrawCurvesTab(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration);
        void DrawFloatCurves(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration);
        void DrawBoneChannels(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration);

        // Curve data helpers
        void AddCurve(FAnimationResource* Resource);
        void DeleteCurve(FAnimationResource* Resource, int32 Index);

        // Notify data helpers
        void  EnsureNotifyTracks(FAnimationResource* Resource);
        void  MarkAnimationDirty();
        float SnapTime(float Time, float Duration) const;
        void  ClearSelection();
        void  AddNotifyAt(FAnimationResource* Resource, FName Track, float Time, bool bState);
        void  DeleteSelected(FAnimationResource* Resource);

        SSimpleAnimationComponent* GetPreviewComponent() const;

        // Transport state
        bool  bIsPlaying = false;
        bool  bLooping   = true;
        float Playrate   = 1.0f;
        // Playhead time as of last frame; editor advances the clip itself (a paused
        // world reports dt=0) and uses this to flash notifies the playhead crosses.
        float LastPlayheadTime = 0.0f;

        // Timeline view state
        float TimelineZoom = 1.0f;       // 1 == fit clip to visible width
        float TimelinePanSeconds = 0.0f; // time at the left edge of the lane area
        bool  bSnapToFrame = true;
        int   FrameRate = 30;

        // Selection / drag
        ENotifyKind SelectedKind = ENotifyKind::None;
        int         SelectedIndex = -1;
        int         SelectedTrack = -1;

        EDragMode   DragMode = EDragMode::None;
        ENotifyKind DragKind = ENotifyKind::None;
        int         DragIndex = -1;

        // Clip time captured when the "add notify" lane popup opens.
        float       LaneAddTime = 0.0f;

        // Pulses briefly when the playhead crosses a notify during in-editor playback,
        // so authored events visibly fire while previewing. Maps notify index -> flash age.
        float       NotifyFlash[256] = {};

        // Curve view
        // Encodes bone * NumTrackKinds + kind, since a compressed bone carries its three tracks inline.
        static constexpr int32 NumTrackKinds = 3;
        int  SelectedChannel = -1;
        int  SelectedCurve   = -1;

        // Edits the selected authored curve. Rebound every frame: adding or removing a curve can move
        // the array the bound pointer points into.
        FCurveEditorWidget CurveWidget;

        // Inline editor for the selected notify's instanced type.
        FPropertyTable NotifyDetails;

        ImGuizmo::OPERATION GuizmoOp = ImGuizmo::TRANSLATE;
        ECS::FEntity DirectionalLightEntity = ECS::NullEntity;
        ECS::FEntity MeshEntity = ECS::NullEntity;
    };
}
