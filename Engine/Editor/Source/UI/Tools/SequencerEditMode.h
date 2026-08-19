#pragma once

#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/WorldEditorMode.h"

namespace Lumina
{
    // Authoring a cutscene against the live world: bind entities, scrub, and key what you see.
    //
    // Unlike terrain and foliage this mode does NOT consume viewport input. Keying works by posing an
    // entity with the ordinary gizmo and capturing it, so select and transform have to keep working.
    class FSequencerEditMode final : public IWorldEditorMode
    {
    public:

        const char* GetDisplayName() const override { return "Sequencer"; }
        const char* GetIcon() const override { return LE_ICON_FILMSTRIP; }
        const char* GetTooltip() const override
        {
            return "Author cutscenes: bind entities, key their transforms, and cut between cameras.";
        }

        void OnEnter(CWorld* World) override;
        void OnExit(CWorld* World) override;

        void Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered,
                  ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize) override;

        void DrawToolbar(CWorld* World, float ButtonSize) override;

        bool ConsumesViewportInput() const override { return false; }

    private:

        void DrawSequencerWindow(CWorld* World);
        void DrawTransportBar(CWorld* World);
        void DrawTimeline(CWorld* World);
        void DrawBindingList(CWorld* World);

        /** Every time anything is keyed or cut, ascending and deduplicated. Drives jump-to-key. */
        void CollectKeyTimes(TVector<float>& OutTimes) const;

        /** Moves the playhead to the nearest key strictly before (-1) or after (+1) it. */
        void StepToAdjacentKey(CWorld* World, int32 Direction);

        void StepFrames(CWorld* World, int32 FrameDelta);

        /** Drops a binding, the tracks that drive it, and the cuts that point at it. Bindings are addressed
         *  by index, so everything above the hole has to be renumbered or tracks silently retarget. */
        void RemoveBinding(CWorld* World, int32 BindingIndex);

        /** Grows or shrinks Duration to end on the last authored key. */
        void FitDurationToContent();

        // Resolves every binding against the world, spawning prefab-backed ones.
        void BindToWorld(CWorld* World);
        void ReleaseBindings(CWorld* World);

        void EvaluateAt(CWorld* World, float NewTime, bool bJumped);

        // Scrubbing writes straight into world entities, so the pre-scrub transforms are snapshotted on
        // bind and put back on exit. Without this, authoring a cutscene permanently moves the level.
        void CaptureRestoreState(CWorld* World);
        void ApplyRestoreState(CWorld* World);

        int32 AddBindingFromSelection(CWorld* World);
        entt::entity FindSelectedEntity(CWorld* World) const;

        CSequenceTrack_Transform* FindOrCreateTransformTrack(int32 BindingIndex);
        void KeyTransform(CWorld* World, int32 BindingIndex);

        // Retimes or removes every channel's key at a given time, so a row's key behaves as the single
        // thing it looks like rather than nine that can drift apart.
        static void MoveTransformKeys(CSequenceTrack_Transform* Track, float FromTime, float ToTime);
        static void DeleteTransformKeys(CSequenceTrack_Transform* Track, float AtTime);
        static void SetTransformKeyInterp(CSequenceTrack_Transform* Track, float AtTime, ECurveInterpMode Mode);

        /** Interpolation of the row's key at a time, taken from Location.X since the nine channels are
         *  keyed and retimed as a unit. Linear when there is no key there. */
        static ECurveInterpMode GetTransformKeyInterp(const CSequenceTrack_Transform* Track, float AtTime);

        CSequenceTrack_Transform* FindTransformTrack(int32 BindingIndex) const;

        CSequenceTrack_CameraCut* FindCameraCutTrack() const;
        CSequenceTrack_CameraCut* FindOrCreateCameraCutTrack();
        void AddCameraCutAtPlayhead(CWorld* World);

        // Cuts are ranges, so the row is drawn and edited as clips rather than the diamonds a keyed track
        // gets. Returns the height it consumed.
        float DrawCameraCutRow(CWorld* World, ImDrawList* DrawList, const ImVec2& Origin,
                               float TrackLeft, float TrackWidth, float Duration);

        TObjectPtr<CSequence>    Sequence;

        // Parallel to Sequence->Bindings.
        TVector<entt::entity>    BoundEntities;
        TVector<entt::entity>    SpawnedEntities;

        struct FRestoreEntry
        {
            entt::entity Entity = entt::null;
            FVector3     Location;
            FVector3     Rotation;
            FVector3     Scale = FVector3(1.0f);
        };
        TVector<FRestoreEntry>   RestoreState;

        // Last transform seen per binding. Auto-key diffs against this, and it is refreshed immediately
        // after every evaluation so the sequence's own writes never read as a user edit.
        TVector<FRestoreEntry>   AutoKeyWatch;

        void RefreshAutoKeyWatch(CWorld* World);
        void ProcessAutoKey(CWorld* World);

        float                    PlayTime = 0.0f;
        float                    PlayRate = 1.0f;
        int32                    SelectedBinding = INDEX_NONE;

        // Deferred: the request comes from a popup opened mid-draw, and removing a binding resizes the very
        // arrays the row loop is walking.
        int32                    PendingRemoveBinding = INDEX_NONE;

        // A key on a row is really a TIME at which all nine transform channels hold a key, since keying
        // writes them together. Selection is therefore by time rather than by index, which also survives
        // the re-sort a retime causes.
        int32                    SelectedKeyBinding = INDEX_NONE;
        float                    SelectedKeyTime = 0.0f;
        uint8                    bDraggingKey:1 = false;

        // Cut drag state. Edge is -1 for the start handle, +1 for the end, 0 for moving the whole clip.
        int32                    SelectedCut = INDEX_NONE;
        int32                    DraggingCut = INDEX_NONE;
        int32                    DragEdge = 0;
        float                    DragGrabOffset = 0.0f;
        // Seconds of sequence visible per screen. 1.0 fits the whole thing; smaller zooms in.
        float                    TimelineZoom = 1.0f;
        float                    TimelineScroll = 0.0f;

        void DrawTimeRuler(ImDrawList* DrawList, const ImVec2& Origin, float TrackLeft, float TrackWidth);

        // Time<->pixel mapping for the current zoom and scroll, rebuilt each frame by DrawTimeline so the
        // cut row and the key rows cannot disagree about where a given time sits.
        TFunction<float(float)>  VisibleTimeToX;
        TFunction<float(float)>  VisibleXToTime;

        uint8                    bBound:1 = false;
        uint8                    bPlaying:1 = false;
        uint8                    bLoop:1 = true;
        uint8                    bAutoKey:1 = false;
    };
}
