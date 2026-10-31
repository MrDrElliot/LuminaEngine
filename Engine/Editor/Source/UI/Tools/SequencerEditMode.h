#pragma once

#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "UI/Tools/WorldEditorMode.h"

namespace Lumina
{
    class FPropertyTable;

    // Authors a cutscene against the editor world, keyed by posing entities with the ordinary gizmo, so it leaves viewport input alone.
    class FSequencerEditMode final : public IWorldEditorMode
    {
    public:

        FSequencerEditMode();
        ~FSequencerEditMode() override;

        const char* GetDisplayName() const override { return "Sequencer"; }
        const char* GetIcon() const override { return LE_ICON_FILMSTRIP; }
        const char* GetTooltip() const override { return "Author cutscenes: bind entities, key their transforms, and cut between cameras."; }
        bool ConsumesViewportInput() const override { return false; }

        void OnEnter(CWorld* World) override;
        void OnExit(CWorld* World) override;
        void Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize) override;
        void DrawToolbar(CWorld* World, float ButtonSize) override;

    private:

        struct FAutoKeyPose
        {
            ECS::FEntity    Entity = ECS::NullEntity;
            FVector3        Location;
            FVector3        Rotation;
            FVector3        Scale = FVector3(1.0f);
        };

        void BindToWorld(CWorld* World);
        void ReleaseBindings(CWorld* World);
        void EvaluateAt(CWorld* World, float NewTime, bool bJumped);
        void MarkSequenceDirty();
        void CommitEdit(CWorld* World);
        void ResetSelection();

        void RefreshAutoKeyWatch(CWorld* World);
        void ProcessAutoKey(CWorld* World);

        void DrawSequencerWindow(CWorld* World);
        void DrawTransportBar(CWorld* World);
        void DrawBindingList(CWorld* World);
        void DrawAddTrackMenu(CWorld* World);
        void DrawTimeline(CWorld* World);
        void DrawTimeRuler(ImDrawList* DrawList, const ImVec2& Origin);
        void DrawCameraCutRow(CWorld* World, ImDrawList* DrawList, float RowY, bool bTimelineHovered);
        void DrawBindingRow(ImDrawList* DrawList, int32 BindingIndex, int32 RowIndex, float RowY, bool bTimelineHovered);
        void DrawTrackRow(ImDrawList* DrawList, int32 TrackIndex, int32 RowIndex, float RowY, bool bTimelineHovered);
        void UpdateKeyDrags(CWorld* World, float Duration);
        void DrawBindingPopups(CWorld* World);
        void DrawTrackPopups(CWorld* World);
        void DrawDetails(CWorld* World);

        NODISCARD float TimeToX(float Time) const;
        NODISCARD float XToTime(float X) const;

        void CollectKeyTimes(TVector<float>& OutTimes) const;
        void StepToAdjacentKey(CWorld* World, int32 Direction);
        void StepFrames(CWorld* World, int32 FrameDelta);
        void FitDurationToContent();

        int32 AddBindingFromSelection(CWorld* World);
        int32 AddBinding(CWorld* World, ECS::FEntity Entity);
        void RemoveBinding(CWorld* World, int32 BindingIndex);

        NODISCARD ECS::FEntity FindSelectedEntity(CWorld* World) const;
        NODISCARD bool GetEditorView(CWorld* World, FVector3& OutLocation, FVector3& OutRotation) const;

        NODISCARD CSequenceTrack_Transform* FindTransformTrack(int32 BindingIndex) const;
        CSequenceTrack_Transform* FindOrCreateTransformTrack(int32 BindingIndex);
        void KeyTransform(CWorld* World, int32 BindingIndex);
        void KeyFromView(CWorld* World, int32 BindingIndex);
        void AddCameraFromView(CWorld* World);
        void DeleteSelectedKey(CWorld* World);

        NODISCARD CSequenceTrack_CameraCut* FindCameraCutTrack() const;
        CSequenceTrack_CameraCut* FindOrCreateCameraCutTrack();
        void AddCameraCutAtPlayhead(CWorld* World);

        CSequenceTrack* AddTrackOfClass(CWorld* World, CClass* Class, int32 BindingIndex);
        void KeyTrackAtPlayhead(CWorld* World, CSequenceTrack* Track);
        void RemoveTrack(CWorld* World, int32 TrackIndex);

        void SetPreviewCameras(CWorld* World, bool bPreview);

        TStrongObjectPtr<CSequence>     Sequence;
        FSequenceInstance               Instance;
        TVector<FAutoKeyPose>           AutoKeyWatch;

        TUniquePtr<FPropertyTable>      Details;
        CObject*                        DetailsTarget = nullptr;

        FString                         LastFiredEvent;
        double                          LastFiredEventTime = 0.0;

        float                           PlayTime = 0.0f;
        float                           PlayRate = 1.0f;

        // The visible span as a fraction of the sequence's length, so 1.0 fits the whole thing.
        float                           TimelineZoom = 1.0f;
        float                           TimelineScroll = 0.0f;

        float                           ViewLeft = 0.0f;
        float                           ViewWidth = 1.0f;
        float                           ViewStart = 0.0f;
        float                           ViewSeconds = 1.0f;

        int32                           SelectedBinding = Constants::kIndexNone;

        // A row's key is the time all nine channels share, so it is selected by time, which also survives a re-sort.
        int32                           SelectedKeyBinding = Constants::kIndexNone;
        float                           SelectedKeyTime = 0.0f;

        int32                           SelectedTrack = Constants::kIndexNone;
        int32                           SelectedTrackKey = Constants::kIndexNone;

        // DragEdge is -1 for the start handle, +1 for the end, and 0 for the whole clip.
        int32                           SelectedCut = Constants::kIndexNone;
        int32                           DraggingCut = Constants::kIndexNone;
        int32                           DragEdge = 0;
        float                           DragGrabOffset = 0.0f;

        int32                           PendingRemoveBinding = Constants::kIndexNone;
        int32                           PendingRemoveTrack = Constants::kIndexNone;

        uint8                           bBound:1 = false;
        uint8                           bPlaying:1 = false;
        uint8                           bLoop:1 = true;
        uint8                           bAutoKey:1 = false;
        uint8                           bPreviewCameras:1 = true;
        uint8                           bDraggingKey:1 = false;
        uint8                           bDraggingTrackKey:1 = false;
        uint8                           bScrubbing:1 = false;
        uint8                           bDetailsEdited:1 = false;
    };
}
