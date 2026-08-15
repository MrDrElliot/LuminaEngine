#pragma once

#define USE_IMGUI_API
#include <imgui.h>

#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CAnimation;
    class CAnimationMontage;
    struct SSimpleAnimationComponent;

    class FAnimationMontageEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FAnimationMontageEditorTool)

        FAnimationMontageEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_ANIMATION_PLAY; }

        void OnInitialize() override;
        void SetupWorldForTool() override;
        void Update(const FUpdateContext& UpdateContext) override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void OnAssetLoadFinished() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        // What a click or drag on the timeline refers to.
        enum class ESelectionKind : uint8 { None, Segment, Section, Notify, NotifyState };

        enum class EDragMode : uint8 { None, Playhead, MoveItem, ResizeStart, ResizeEnd };

        void DrawTimeline();
        void DrawTransport(float Duration);
        void DrawSlotTracks(CAnimationMontage* Montage, float Duration);
        /** Keeps Sections in timeline order, which every section lookup assumes. */
        void SortSections(CAnimationMontage* Montage);
        void DrawInspector(CAnimationMontage* Montage, float Duration);

        void SyncPreviewToPlayhead(CAnimationMontage* Montage);
        SSimpleAnimationComponent* GetPreviewComponent() const;

        void MarkMontageDirty();
        float SnapTime(float Time, float Duration) const;
        void ClearSelection();
        void DeleteSelected(CAnimationMontage* Montage);

        /** Appends a segment to a slot track, starting at the track's current end. */
        void AppendSegment(CAnimationMontage* Montage, int32 TrackIndex, CAnimation* Animation, float StartTime);

        /** Packs a slot track's segments end to end in start order, so playback has no gaps. */
        void ReflowTrack(CAnimationMontage* Montage, int32 TrackIndex);

        // Transport
        bool  bIsPlaying = false;
        bool  bLooping   = true;
        float PlayRate   = 1.0f;
        float Playhead   = 0.0f;

        // Timeline view
        float TimelineZoom = 1.0f;
        float TimelinePanSeconds = 0.0f;
        bool  bSnapToFrame = true;
        int   FrameRate = 30;

        /** Slot track whose segments drive the preview mesh; montages usually have exactly one. */
        int32 PreviewTrack = 0;

        ESelectionKind SelectedKind = ESelectionKind::None;
        int32 SelectedTrack = 0;
        int32 SelectedIndex = -1;

        EDragMode      DragMode = EDragMode::None;
        ESelectionKind DragKind = ESelectionKind::None;
        int32          DragTrack = -1;
        int32          DragIndex = -1;
        float          DragGrabOffset = 0.0f;

        float LaneAddTime = 0.0f;

        entt::entity DirectionalLightEntity = entt::null;
        entt::entity MeshEntity = entt::null;
    };
}
