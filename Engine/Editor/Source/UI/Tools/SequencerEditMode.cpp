#include "SequencerEditMode.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "imgui.h"

namespace Lumina
{
    static constexpr float SequencerTrackHeight = 26.0f;
    static constexpr float SequencerLabelWidth  = 190.0f;
    static constexpr float SequencerRulerHeight = 24.0f;

    static const ImU32 SeqPanelBg     = IM_COL32(24, 26, 31, 255);
    static const ImU32 SeqRowBg       = IM_COL32(33, 36, 43, 255);
    static const ImU32 SeqRowAltBg    = IM_COL32(29, 32, 38, 255);
    static const ImU32 SeqRowSelBg    = IM_COL32(48, 70, 100, 255);
    static const ImU32 SeqRowHoverBg  = IM_COL32(41, 46, 56, 255);
    static const ImU32 SeqGridMinor   = IM_COL32(255, 255, 255, 12);
    static const ImU32 SeqGridMajor   = IM_COL32(255, 255, 255, 32);
    static const ImU32 SeqText        = IM_COL32(226, 230, 238, 255);
    static const ImU32 SeqTextDim     = IM_COL32(150, 157, 170, 255);
    static const ImU32 SeqKey         = IM_COL32(255, 205, 90, 255);
    static const ImU32 SeqKeyOutline  = IM_COL32(20, 18, 12, 220);
    static const ImU32 SeqPlayhead    = IM_COL32(120, 255, 150, 235);

    // Steps of 1, 2 and 5 per decade, so labels never land on awkward fractions.
    static float ChooseRulerStep(float VisibleSeconds, float TrackWidth)
    {
        const float TargetPixels = 90.0f;
        const float Rough = (VisibleSeconds / Math::Max(TrackWidth, 1.0f)) * TargetPixels;

        float Magnitude = 0.001f;
        while (Magnitude * 10.0f < Rough)
        {
            Magnitude *= 10.0f;
        }

        const float Normalized = Rough / Magnitude;
        const float Step = Normalized <= 1.0f ? 1.0f : (Normalized <= 2.0f ? 2.0f : (Normalized <= 5.0f ? 5.0f : 10.0f));
        return Step * Magnitude;
    }

    void FSequencerEditMode::OnEnter(CWorld* World)
    {
        PlayTime = 0.0f;
        bPlaying = false;

        if (Sequence.IsValid())
        {
            BindToWorld(World);
        }
    }

    void FSequencerEditMode::OnExit(CWorld* World)
    {
        bPlaying = false;
        ReleaseBindings(World);
    }

    void FSequencerEditMode::BindToWorld(CWorld* World)
    {
        ReleaseBindings(World);

        if (World == nullptr || !Sequence.IsValid())
        {
            return;
        }

        BoundEntities.assign(Sequence->Bindings.size(), entt::null);

        for (int32 i = 0; i < (int32)Sequence->Bindings.size(); ++i)
        {
            const SSequenceBinding& Binding = Sequence->Bindings[i];

            if (Binding.Kind == ESequenceBindingKind::Spawn)
            {
                if (!Binding.SpawnPrefab.IsValid())
                {
                    continue;
                }

                const entt::entity Spawned = Binding.SpawnPrefab->Instantiate(World);
                if (Spawned != entt::null)
                {
                    BoundEntities[i] = Spawned;
                    SpawnedEntities.push_back(Spawned);
                }
                continue;
            }

            // Possess matches on the entity's name, which is what the binding stores.
            auto View = World->View<SNameComponent>();
            for (entt::entity Entity : View)
            {
                if (View.get<SNameComponent>(Entity).Name == Binding.Name)
                {
                    BoundEntities[i] = Entity;
                    break;
                }
            }
        }

        CaptureRestoreState(World);
        RefreshAutoKeyWatch(World);
        bBound = true;
    }

    void FSequencerEditMode::ReleaseBindings(CWorld* World)
    {
        if (bBound)
        {
            ApplyRestoreState(World);
        }

        if (World != nullptr)
        {
            for (entt::entity Spawned : SpawnedEntities)
            {
                if (World->IsValidEntity(Spawned))
                {
                    World->DestroyEntity(Spawned);
                }
            }
        }

        SpawnedEntities.clear();
        BoundEntities.clear();
        RestoreState.clear();
        bBound = false;
    }

    void FSequencerEditMode::CaptureRestoreState(CWorld* World)
    {
        RestoreState.clear();

        if (World == nullptr)
        {
            return;
        }

        for (entt::entity Entity : BoundEntities)
        {
            // Spawned entities are destroyed on exit, so there is nothing to put back for them.
            if (Entity == entt::null || !World->IsValidEntity(Entity))
            {
                continue;
            }

            bool bSpawned = false;
            for (entt::entity Other : SpawnedEntities)
            {
                if (Other == Entity)
                {
                    bSpawned = true;
                    break;
                }
            }

            if (bSpawned)
            {
                continue;
            }

            if (STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entity))
            {
                FRestoreEntry Entry;
                Entry.Entity = Entity;
                Entry.Location = Transform->GetLocation();
                Entry.Rotation = Math::Degrees(Math::EulerAngles(Transform->GetRotation()));
                Entry.Scale = Transform->GetScale();
                RestoreState.push_back(Entry);
            }
        }
    }

    void FSequencerEditMode::ApplyRestoreState(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }

        for (const FRestoreEntry& Entry : RestoreState)
        {
            if (!World->IsValidEntity(Entry.Entity))
            {
                continue;
            }

            if (STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entry.Entity))
            {
                Transform->SetLocation(Entry.Location);
                Transform->SetRotationFromEuler(Entry.Rotation);
                Transform->SetScale(Entry.Scale);
            }
        }
    }

    void FSequencerEditMode::EvaluateAt(CWorld* World, float NewTime, bool bJumped)
    {
        if (World == nullptr || !Sequence.IsValid() || !bBound)
        {
            return;
        }

        FSequenceEvalContext EvalContext;
        EvalContext.World = World;
        EvalContext.Sequence = Sequence.Get();
        EvalContext.PreviousTime = PlayTime;
        EvalContext.Time = NewTime;
        EvalContext.BoundEntities = &BoundEntities;
        EvalContext.bJumped = bJumped;

        Sequence->Evaluate(EvalContext);

        PlayTime = NewTime;

        // Rebasing stops auto-key treating the sequence's own output as an edit on every scrubbed frame.
        RefreshAutoKeyWatch(World);
    }

    void FSequencerEditMode::RefreshAutoKeyWatch(CWorld* World)
    {
        AutoKeyWatch.assign(BoundEntities.size(), FRestoreEntry());

        if (World == nullptr)
        {
            return;
        }

        for (int32 i = 0; i < (int32)BoundEntities.size(); ++i)
        {
            const entt::entity Entity = BoundEntities[i];
            if (Entity == entt::null || !World->IsValidEntity(Entity))
            {
                continue;
            }

            if (const STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entity))
            {
                FRestoreEntry& Entry = AutoKeyWatch[i];
                Entry.Entity = Entity;
                Entry.Location = Transform->GetLocation();
                Entry.Rotation = Math::Degrees(Math::EulerAngles(Transform->GetRotation()));
                Entry.Scale = Transform->GetScale();
            }
        }
    }

    void FSequencerEditMode::ProcessAutoKey(CWorld* World)
    {
        if (World == nullptr || !bBound || AutoKeyWatch.size() != BoundEntities.size())
        {
            return;
        }

        // Loose enough that the gizmo's matrix round-trip does not key, tight enough that a nudge does.
        constexpr float PositionEpsilonSq = 1e-6f;
        constexpr float AngleEpsilonSq    = 1e-4f;

        for (int32 i = 0; i < (int32)BoundEntities.size(); ++i)
        {
            const entt::entity Entity = BoundEntities[i];
            if (Entity == entt::null || !World->IsValidEntity(Entity) || AutoKeyWatch[i].Entity != Entity)
            {
                continue;
            }

            const STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entity);
            if (Transform == nullptr)
            {
                continue;
            }

            const FRestoreEntry& Watched = AutoKeyWatch[i];
            const FVector3 Location = Transform->GetLocation();
            const FVector3 Rotation = Math::Degrees(Math::EulerAngles(Transform->GetRotation()));
            const FVector3 Scale = Transform->GetScale();

            const bool bMoved = Math::LengthSquared(Location - Watched.Location) > PositionEpsilonSq
                             || Math::LengthSquared(Rotation - Watched.Rotation) > AngleEpsilonSq
                             || Math::LengthSquared(Scale - Watched.Scale) > PositionEpsilonSq;

            if (bMoved)
            {
                KeyTransform(World, i);
            }
        }

        RefreshAutoKeyWatch(World);
    }

    entt::entity FSequencerEditMode::FindSelectedEntity(CWorld* World) const
    {
        if (World == nullptr)
        {
            return entt::null;
        }

        auto View = World->View<FSelectedInEditorComponent>();
        for (entt::entity Entity : View)
        {
            return Entity;
        }

        return entt::null;
    }

    int32 FSequencerEditMode::AddBindingFromSelection(CWorld* World)
    {
        const entt::entity Selected = FindSelectedEntity(World);
        if (Selected == entt::null || !Sequence.IsValid())
        {
            ImGuiX::Notifications::NotifyWarning("Select an entity in the world to bind it.");
            return INDEX_NONE;
        }

        FName EntityName = "Entity";
        if (const SNameComponent* NameComponent = World->TryGetComponent<SNameComponent>(Selected))
        {
            EntityName = NameComponent->Name;
        }

        for (int32 i = 0; i < (int32)Sequence->Bindings.size(); ++i)
        {
            if (Sequence->Bindings[i].Name == EntityName)
            {
                return i;
            }
        }

        SSequenceBinding& Binding = Sequence->Bindings.emplace_back();
        Binding.Name = EntityName;
        Binding.Kind = ESequenceBindingKind::Possess;

        Sequence->GetPackage()->MarkDirty();
        BindToWorld(World);

        return (int32)Sequence->Bindings.size() - 1;
    }

    CSequenceTrack_Transform* FSequencerEditMode::FindOrCreateTransformTrack(int32 BindingIndex)
    {
        for (const TObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
        {
            if (!Track.IsValid() || Track->BindingIndex != BindingIndex)
            {
                continue;
            }

            if (CSequenceTrack_Transform* Transform = Cast<CSequenceTrack_Transform>(Track.Get()))
            {
                return Transform;
            }
        }

        // Outered to the sequence's package so the track is an export and survives a save.
        CSequenceTrack_Transform* Created = NewObject<CSequenceTrack_Transform>(Sequence->GetPackage(), "TransformTrack");
        Created->BindingIndex = BindingIndex;

        // Keying is the only way tracks get made, and a track that drives nothing is a trap.
        Created->Location.bEnabled = true;
        Created->Rotation.bEnabled = true;
        Created->Scale.bEnabled = true;

        Sequence->Tracks.push_back(Created);
        return Created;
    }

    void FSequencerEditMode::KeyTransform(CWorld* World, int32 BindingIndex)
    {
        if (World == nullptr || !Sequence.IsValid()
            || BindingIndex < 0 || BindingIndex >= (int32)BoundEntities.size())
        {
            return;
        }

        const entt::entity Entity = BoundEntities[BindingIndex];
        if (Entity == entt::null || !World->IsValidEntity(Entity))
        {
            ImGuiX::Notifications::NotifyWarning("That binding did not resolve to an entity in this world.");
            return;
        }

        STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        CSequenceTrack_Transform* Track = FindOrCreateTransformTrack(BindingIndex);

        const float Time = Sequence->SnapToFrame(PlayTime);
        const FVector3 Location = Transform->GetLocation();
        const FVector3 Rotation = Math::Degrees(Math::EulerAngles(Transform->GetRotation()));
        const FVector3 Scale = Transform->GetScale();

        // A duplicate key at an identical time makes a zero-width segment, which reads as a hard step.
        const auto KeyChannel = [Time](SSequenceVectorCurve& Channel, const FVector3& Value)
        {
            Channel.X.Curve.UpdateOrAddKey(Time, Value.x);
            Channel.Y.Curve.UpdateOrAddKey(Time, Value.y);
            Channel.Z.Curve.UpdateOrAddKey(Time, Value.z);
        };

        KeyChannel(Track->Location, Location);
        KeyChannel(Track->Rotation, Rotation);
        KeyChannel(Track->Scale, Scale);

        Sequence->GetPackage()->MarkDirty();
    }

    CSequenceTrack_Transform* FSequencerEditMode::FindTransformTrack(int32 BindingIndex) const
    {
        if (!Sequence.IsValid())
        {
            return nullptr;
        }

        for (const TObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
        {
            if (!Track.IsValid() || Track->BindingIndex != BindingIndex)
            {
                continue;
            }

            if (CSequenceTrack_Transform* Transform = Cast<CSequenceTrack_Transform>(Track.Get()))
            {
                return Transform;
            }
        }

        return nullptr;
    }

    void FSequencerEditMode::MoveTransformKeys(CSequenceTrack_Transform* Track, float FromTime, float ToTime)
    {
        if (Track == nullptr)
        {
            return;
        }

        // Half a frame at 240fps, tight enough to keep adjacent keys distinct and loose enough for float error.
        constexpr float MatchEpsilon = 0.002f;

        SCurve* Channels[9] =
        {
            &Track->Location.X, &Track->Location.Y, &Track->Location.Z,
            &Track->Rotation.X, &Track->Rotation.Y, &Track->Rotation.Z,
            &Track->Scale.X,    &Track->Scale.Y,    &Track->Scale.Z,
        };

        for (SCurve* Channel : Channels)
        {
            // An asset-backed curve is shared, so retiming it here would edit every other user of it.
            if (Channel->bUseAsset)
            {
                continue;
            }

            for (SCurveKey& Key : Channel->Curve.Keys)
            {
                if (Math::Abs(Key.Time - FromTime) <= MatchEpsilon)
                {
                    Key.Time = ToTime;
                    break;
                }
            }

            Channel->Curve.SortKeys();
        }
    }

    void FSequencerEditMode::SetTransformKeyInterp(CSequenceTrack_Transform* Track, float AtTime, ECurveInterpMode Mode)
    {
        if (Track == nullptr)
        {
            return;
        }

        constexpr float MatchEpsilon = 0.002f;

        SCurve* Channels[9] =
        {
            &Track->Location.X, &Track->Location.Y, &Track->Location.Z,
            &Track->Rotation.X, &Track->Rotation.Y, &Track->Rotation.Z,
            &Track->Scale.X,    &Track->Scale.Y,    &Track->Scale.Z,
        };

        for (SCurve* Channel : Channels)
        {
            if (Channel->bUseAsset)
            {
                continue;
            }

            for (SCurveKey& Key : Channel->Curve.Keys)
            {
                if (Math::Abs(Key.Time - AtTime) <= MatchEpsilon)
                {
                    Key.InterpMode = Mode;
                    break;
                }
            }

            // Tangents are zero on a linear key, and a zero-slope Hermite is not the arc the mode is chosen for.
            Channel->Curve.ComputeAutoTangents();
        }
    }

    ECurveInterpMode FSequencerEditMode::GetTransformKeyInterp(const CSequenceTrack_Transform* Track, float AtTime)
    {
        if (Track == nullptr)
        {
            return ECurveInterpMode::Linear;
        }

        constexpr float MatchEpsilon = 0.002f;

        for (const SCurveKey& Key : Track->Location.X.Resolve().Keys)
        {
            if (Math::Abs(Key.Time - AtTime) <= MatchEpsilon)
            {
                return Key.InterpMode;
            }
        }

        return ECurveInterpMode::Linear;
    }

    void FSequencerEditMode::DeleteTransformKeys(CSequenceTrack_Transform* Track, float AtTime)
    {
        if (Track == nullptr)
        {
            return;
        }

        constexpr float MatchEpsilon = 0.002f;

        SCurve* Channels[9] =
        {
            &Track->Location.X, &Track->Location.Y, &Track->Location.Z,
            &Track->Rotation.X, &Track->Rotation.Y, &Track->Rotation.Z,
            &Track->Scale.X,    &Track->Scale.Y,    &Track->Scale.Z,
        };

        for (SCurve* Channel : Channels)
        {
            if (Channel->bUseAsset)
            {
                continue;
            }

            for (int32 i = (int32)Channel->Curve.Keys.size() - 1; i >= 0; --i)
            {
                if (Math::Abs(Channel->Curve.Keys[i].Time - AtTime) <= MatchEpsilon)
                {
                    Channel->Curve.RemoveKey(i);
                }
            }
        }
    }

    CSequenceTrack_CameraCut* FSequencerEditMode::FindCameraCutTrack() const
    {
        if (!Sequence.IsValid())
        {
            return nullptr;
        }

        for (const TObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
        {
            if (Track.IsValid())
            {
                if (CSequenceTrack_CameraCut* Cut = Cast<CSequenceTrack_CameraCut>(Track.Get()))
                {
                    return Cut;
                }
            }
        }

        return nullptr;
    }

    CSequenceTrack_CameraCut* FSequencerEditMode::FindOrCreateCameraCutTrack()
    {
        if (CSequenceTrack_CameraCut* Existing = FindCameraCutTrack())
        {
            return Existing;
        }

        // Cuts are one exclusive choice over time, so a second track would just fight for the same slot.
        CSequenceTrack_CameraCut* Created = NewObject<CSequenceTrack_CameraCut>(Sequence->GetPackage(), "CameraCutTrack");
        Sequence->Tracks.push_back(Created);
        return Created;
    }

    void FSequencerEditMode::AddCameraCutAtPlayhead(CWorld* World)
    {
        if (!Sequence.IsValid() || SelectedBinding == INDEX_NONE)
        {
            ImGuiX::Notifications::NotifyWarning("Select the binding holding the camera you want to cut to.");
            return;
        }

        // A binding with no camera would produce a cut that silently does nothing at runtime.
        if (SelectedBinding < (int32)BoundEntities.size())
        {
            const entt::entity Entity = BoundEntities[SelectedBinding];
            if (Entity != entt::null && World->IsValidEntity(Entity)
                && World->TryGetComponent<SCameraComponent>(Entity) == nullptr)
            {
                ImGuiX::Notifications::NotifyWarning("'{0}' has no camera component.",
                                                     Sequence->Bindings[SelectedBinding].Name);
                return;
            }
        }

        CSequenceTrack_CameraCut* Track = FindOrCreateCameraCutTrack();

        SSequenceCameraCut& Cut = Track->Cuts.emplace_back();
        Cut.BindingIndex = SelectedBinding;
        Cut.StartTime = Sequence->SnapToFrame(PlayTime);
        Cut.EndTime = Math::Min(Cut.StartTime + 1.0f, Sequence->Duration);

        // A zero-length cut can never contain the playhead, so it would be invisible and inert.
        if (Cut.EndTime <= Cut.StartTime)
        {
            Cut.EndTime = Cut.StartTime + Sequence->FrameToTime(1);
        }

        SelectedCut = (int32)Track->Cuts.size() - 1;
        Sequence->GetPackage()->MarkDirty();
    }

    void FSequencerEditMode::DrawTimeRuler(ImDrawList* DrawList, const ImVec2& Origin, float TrackLeft, float TrackWidth)
    {
        const float Duration = Math::Max(Sequence->Duration, 0.01f);
        const float ViewSeconds = Math::Max(Duration * TimelineZoom, 0.01f);

        const ImVec2 RulerMin(Origin.x, Origin.y);
        const ImVec2 RulerMax(Origin.x + SequencerLabelWidth + TrackWidth, Origin.y + SequencerRulerHeight);

        DrawList->AddRectFilled(RulerMin, RulerMax, IM_COL32(19, 21, 25, 255));
        DrawList->AddLine(ImVec2(RulerMin.x, RulerMax.y), ImVec2(RulerMax.x, RulerMax.y), SeqGridMajor);

        DrawList->AddText(ImVec2(Origin.x + 10.0f, Origin.y + 4.0f), SeqTextDim, "Tracks");

        DrawList->PushClipRect(ImVec2(TrackLeft, RulerMin.y), RulerMax, true);

        const float Step = ChooseRulerStep(ViewSeconds, TrackWidth);
        const float SubStep = Step * 0.25f;

        // Subdivisions first so labeled ticks draw over them.
        for (float T = Math::Floor(TimelineScroll / SubStep) * SubStep; T <= TimelineScroll + ViewSeconds; T += SubStep)
        {
            const float X = VisibleTimeToX(T);
            DrawList->AddLine(ImVec2(X, RulerMax.y - 5.0f), ImVec2(X, RulerMax.y), SeqGridMinor);
        }

        char Label[32];
        for (float T = Math::Floor(TimelineScroll / Step) * Step; T <= TimelineScroll + ViewSeconds; T += Step)
        {
            const float X = VisibleTimeToX(T);
            DrawList->AddLine(ImVec2(X, RulerMax.y - 10.0f), ImVec2(X, RulerMax.y), SeqGridMajor);

            // Reading a sub-second time in seconds is useless, and frames are useless across ten-second steps.
            if (Step < 1.0f)
            {
                snprintf(Label, sizeof(Label), "f%d", Sequence->TimeToFrame(T));
            }
            else
            {
                snprintf(Label, sizeof(Label), "%.2gs", T);
            }

            DrawList->AddText(ImVec2(X + 3.0f, RulerMin.y + 4.0f), SeqTextDim, Label);
        }

        DrawList->PopClipRect();
    }

    float FSequencerEditMode::DrawCameraCutRow(CWorld* World, ImDrawList* DrawList, const ImVec2& Origin,
                                               float TrackLeft, float TrackWidth, float Duration)
    {
        CSequenceTrack_CameraCut* Track = FindCameraCutTrack();

        const float RowY = Origin.y;
        const ImVec2 RowMin(Origin.x, RowY);
        const ImVec2 RowMax(TrackLeft + TrackWidth, RowY + SequencerTrackHeight);

        DrawList->AddRectFilled(RowMin, RowMax, IM_COL32(38, 33, 26, 255));
        DrawList->AddLine(ImVec2(TrackLeft, RowY), ImVec2(TrackLeft, RowMax.y), SeqGridMajor);
        DrawList->AddText(ImVec2(Origin.x + 10.0f, RowY + 5.0f), IM_COL32(255, 205, 130, 235), LE_ICON_MOVIE_OPEN "  Camera Cuts");

        DrawList->PushClipRect(ImVec2(TrackLeft, RowY), RowMax, true);
        struct FCutClipScope { ImDrawList* DL; ~FCutClipScope() { DL->PopClipRect(); } } ClipScope{ DrawList };

        if (Track == nullptr)
        {
            return SequencerTrackHeight;
        }

        // Shared with the key rows so a cut and a keyframe at the same time line up under any zoom.
        const auto TimeToX = [&](float Time) { return VisibleTimeToX(Time); };
        const auto XToTime = [&](float X)    { return Math::Clamp(VisibleXToTime(X), 0.0f, Duration); };

        const ImVec2 MousePos = ImGui::GetMousePos();
        constexpr float EdgeGrab = 5.0f;

        int32 PendingRemoval = INDEX_NONE;

        for (int32 i = 0; i < (int32)Track->Cuts.size(); ++i)
        {
            SSequenceCameraCut& Cut = Track->Cuts[i];

            const float StartX = TimeToX(Cut.StartTime);
            const float EndX = Math::Max(TimeToX(Cut.EndTime), StartX + 3.0f);

            const ImVec2 ClipMin(StartX, RowY + 2.0f);
            const ImVec2 ClipMax(EndX, RowY + SequencerTrackHeight - 4.0f);

            const bool bSelected = (i == SelectedCut);
            DrawList->AddRectFilled(ClipMin, ClipMax,
                bSelected ? IM_COL32(255, 190, 80, 235) : IM_COL32(190, 135, 55, 220), 2.0f);
            DrawList->AddRect(ClipMin, ClipMax, IM_COL32(20, 16, 10, 220), 2.0f);

            const bool bValidBinding = Cut.BindingIndex >= 0 && Cut.BindingIndex < (int32)Sequence->Bindings.size();
            const char* Label = bValidBinding ? Sequence->Bindings[Cut.BindingIndex].Name.c_str() : "<unbound>";
            DrawList->PushClipRect(ClipMin, ClipMax, true);
            DrawList->AddText(ImVec2(ClipMin.x + 4.0f, ClipMin.y + 1.0f), IM_COL32(25, 20, 12, 255), Label);
            DrawList->PopClipRect();

            const bool bHovered = ImGui::IsMouseHoveringRect(ClipMin, ClipMax);

            if (DraggingCut == INDEX_NONE && bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedCut = i;
                DraggingCut = i;

                // The grab offset keeps a clip from jumping so its start snaps under the cursor on the first frame.
                if (MousePos.x - StartX <= EdgeGrab)      { DragEdge = -1; }
                else if (EndX - MousePos.x <= EdgeGrab)   { DragEdge = 1; }
                else                                      { DragEdge = 0; DragGrabOffset = XToTime(MousePos.x) - Cut.StartTime; }
            }

            if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedCut = i;
                PendingRemoval = i;
            }
        }

        if (DraggingCut != INDEX_NONE)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || DraggingCut >= (int32)Track->Cuts.size())
            {
                DraggingCut = INDEX_NONE;
            }
            else
            {
                SSequenceCameraCut& Cut = Track->Cuts[DraggingCut];
                const float MinLength = Sequence->FrameToTime(1);
                const float Time = Sequence->SnapToFrame(XToTime(MousePos.x));

                if (DragEdge < 0)
                {
                    Cut.StartTime = Math::Min(Time, Cut.EndTime - MinLength);
                }
                else if (DragEdge > 0)
                {
                    Cut.EndTime = Math::Max(Time, Cut.StartTime + MinLength);
                }
                else
                {
                    const float Length = Cut.EndTime - Cut.StartTime;
                    Cut.StartTime = Math::Clamp(Time - DragGrabOffset, 0.0f, Math::Max(Duration - Length, 0.0f));
                    Cut.StartTime = Sequence->SnapToFrame(Cut.StartTime);
                    Cut.EndTime = Cut.StartTime + Length;
                }

                Sequence->GetPackage()->MarkDirty();

                // Re-evaluate live so the viewport shows the shot the drag is producing.
                EvaluateAt(World, PlayTime, true);
            }
        }

        // Deferred, since erasing mid-iteration invalidates the loop above.
        if (PendingRemoval != INDEX_NONE)
        {
            Track->Cuts.erase(Track->Cuts.begin() + PendingRemoval);
            SelectedCut = INDEX_NONE;
            DraggingCut = INDEX_NONE;
            Sequence->GetPackage()->MarkDirty();
        }

        return SequencerTrackHeight;
    }

    void FSequencerEditMode::Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered,
                                  ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize)
    {
        (void)Camera; (void)bViewportHovered; (void)ViewportScreenOrigin; (void)ViewportSize;

        if (!Sequence.IsValid())
        {
            return;
        }

        if (!bPlaying)
        {
            // During playback every driven transform is the sequence's own output, so there is nothing to key.
            if (bAutoKey)
            {
                ProcessAutoKey(World);
            }
            return;
        }

        float NewTime = PlayTime + ImGui::GetIO().DeltaTime * PlayRate;
        bool bJumped = false;

        if (NewTime >= Sequence->Duration)
        {
            if (bLoop)
            {
                NewTime = 0.0f;
                bJumped = true;
            }
            else
            {
                NewTime = Sequence->Duration;
                bPlaying = false;
            }
        }

        EvaluateAt(World, NewTime, bJumped);
    }

    void FSequencerEditMode::DrawToolbar(CWorld* World, float ButtonSize)
    {
        (void)ButtonSize;

        FGuid SequenceGUID = Sequence.IsValid() && Sequence->GetPackage() != nullptr
            ? Sequence->GetGUID() : FGuid();

        ImGui::SetNextItemWidth(240.0f);
        if (ImGuiX::AssetReferenceCombo("##Sequence", CSequence::StaticClass(), SequenceGUID, LE_ICON_FILMSTRIP))
        {
            ReleaseBindings(World);
            Sequence = Cast<CSequence>(LoadObject<CObject>(SequenceGUID));
            PlayTime = 0.0f;

            if (Sequence.IsValid())
            {
                BindToWorld(World);
            }
        }

        if (!Sequence.IsValid())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Pick or create a Sequence to begin.");
            return;
        }

        ImGui::SameLine();
        if (ImGui::Button(bPlaying ? LE_ICON_PAUSE "##Play" : LE_ICON_PLAY "##Play"))
        {
            bPlaying = !bPlaying;
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_STOP "##Stop"))
        {
            bPlaying = false;
            EvaluateAt(World, 0.0f, true);
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH " Rebind"))
        {
            BindToWorld(World);
        }
        ImGuiX::TextTooltip("Re-resolve every binding against the world. Use after renaming or adding an entity.");

        DrawSequencerWindow(World);
    }

    void FSequencerEditMode::DrawSequencerWindow(CWorld* World)
    {
        // A timeline needs the full width of the screen, and the user docks it wherever suits.
        if (!ImGui::Begin(LE_ICON_FILMSTRIP " Sequencer"))
        {
            ImGui::End();
            return;
        }

        // Suppressed while a field has the caret, so Space stays a space while typing a frame number.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
            {
                bPlaying = !bPlaying;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && bPlaying)
            {
                bPlaying = false;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
            {
                StepFrames(World, -1);
            }

            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
            {
                StepFrames(World, 1);
            }
        }

        DrawTransportBar(World);
        ImGui::Separator();
        DrawBindingList(World);
        ImGui::Separator();
        DrawTimeline(World);

        // The request comes from a popup, and the row loop above iterates the arrays this resizes.
        if (PendingRemoveBinding != INDEX_NONE)
        {
            RemoveBinding(World, PendingRemoveBinding);
            PendingRemoveBinding = INDEX_NONE;
        }

        ImGui::End();
    }

    void FSequencerEditMode::DrawTransportBar(CWorld* World)
    {
        const int32 FrameCount = Sequence->GetFrameCount();
        const int32 CurrentFrame = Sequence->TimeToFrame(PlayTime);

        if (ImGui::Button(LE_ICON_SKIP_PREVIOUS "##Start"))
        {
            bPlaying = false;
            EvaluateAt(World, 0.0f, true);
        }
        ImGuiX::TextTooltip("Jump to the start.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CHEVRON_DOUBLE_LEFT "##PrevKey"))
        {
            StepToAdjacentKey(World, -1);
        }
        ImGuiX::TextTooltip("Jump to the previous key.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CHEVRON_LEFT "##PrevFrame"))
        {
            StepFrames(World, -1);
        }
        ImGuiX::TextTooltip("Step back one frame.");

        ImGui::SameLine();
        if (ImGui::Button(bPlaying ? LE_ICON_PAUSE "##PlayT" : LE_ICON_PLAY "##PlayT"))
        {
            bPlaying = !bPlaying;
        }
        ImGuiX::TextTooltip("Play or pause. Space toggles too, and Escape stops.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CHEVRON_RIGHT "##NextFrame"))
        {
            StepFrames(World, 1);
        }
        ImGuiX::TextTooltip("Step forward one frame.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CHEVRON_DOUBLE_RIGHT "##NextKey"))
        {
            StepToAdjacentKey(World, 1);
        }
        ImGuiX::TextTooltip("Jump to the next key.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_SKIP_NEXT "##End"))
        {
            bPlaying = false;
            EvaluateAt(World, Sequence->Duration, true);
        }
        ImGuiX::TextTooltip("Jump to the end.");

        ImGui::SameLine();
        bool bLoopValue = bLoop;
        if (ImGui::Checkbox(LE_ICON_REPEAT "##Loop", &bLoopValue))
        {
            bLoop = bLoopValue;
        }
        ImGuiX::TextTooltip("Loop playback instead of stopping at the end.");

        // A frame rather than seconds, since it is what a key snaps to and what is worth typing.
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        int32 FrameEntry = CurrentFrame;
        if (ImGui::DragInt("##Frame", &FrameEntry, 0.25f, 0, Math::Max(FrameCount, 0), "Frame %d"))
        {
            bPlaying = false;
            EvaluateAt(World, Math::Clamp(Sequence->FrameToTime(FrameEntry), 0.0f, Sequence->Duration), true);
        }
        ImGuiX::TextTooltip("Playhead position. Drag or double-click to type a frame.");

        ImGui::SameLine();
        ImGui::TextDisabled("/ %d  (%.2fs)", FrameCount, PlayTime);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat("##Rate", &PlayRate, 0.01f, 0.05f, 8.0f, "Rate %.2fx");
        ImGuiX::TextTooltip("Preview playback speed. Does not affect the asset.");

        // Length and rate are reached for while scrubbing, not hidden away in a details panel.
        ImGui::SameLine();
        ImGui::TextUnformatted("|");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        float DurationEntry = Sequence->Duration;
        if (ImGui::DragFloat("##Duration", &DurationEntry, 0.05f, 0.1f, 3600.0f, "Length %.2fs"))
        {
            Sequence->Duration = Math::Max(DurationEntry, 0.1f);
            Sequence->GetPackage()->MarkDirty();

            // Shortening can strand the playhead and the view past the new end.
            PlayTime = Math::Min(PlayTime, Sequence->Duration);
            TimelineScroll = Math::Clamp(TimelineScroll, 0.0f,
                Math::Max(Sequence->Duration - Sequence->Duration * TimelineZoom, 0.0f));
        }
        ImGuiX::TextTooltip("Length of the sequence. Keys past the end are kept but never played.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_ARROW_COLLAPSE_HORIZONTAL "##Fit"))
        {
            FitDurationToContent();
        }
        ImGuiX::TextTooltip("Set the length to end on the last key.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        int32 RateEntry = Sequence->FrameRate;
        if (ImGui::DragInt("##FrameRate", &RateEntry, 0.2f, 1, 240, "%d fps"))
        {
            Sequence->FrameRate = Math::Clamp(RateEntry, 1, 240);
            Sequence->GetPackage()->MarkDirty();
        }
        ImGuiX::TextTooltip("Display and snapping rate. Evaluation stays continuous, so this never quantizes playback.");
    }

    void FSequencerEditMode::CollectKeyTimes(TVector<float>& OutTimes) const
    {
        OutTimes.clear();

        if (!Sequence.IsValid())
        {
            return;
        }

        for (const TObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
        {
            if (!Track.IsValid())
            {
                continue;
            }

            if (const CSequenceTrack_Transform* Transform = Cast<CSequenceTrack_Transform>(Track.Get()))
            {
                for (const SCurveKey& Key : Transform->Location.X.Resolve().Keys)
                {
                    OutTimes.push_back(Key.Time);
                }
            }
            else if (const CSequenceTrack_CameraCut* Cuts = Cast<CSequenceTrack_CameraCut>(Track.Get()))
            {
                // Both edges, since a cut boundary is exactly where a director wants to land.
                for (const SSequenceCameraCut& Cut : Cuts->Cuts)
                {
                    OutTimes.push_back(Cut.StartTime);
                    OutTimes.push_back(Cut.EndTime);
                }
            }
        }

        Algo::Sort(OutTimes.begin(), OutTimes.end());
        OutTimes.erase(Algo::Unique(OutTimes.begin(), OutTimes.end()), OutTimes.end());
    }

    void FSequencerEditMode::StepToAdjacentKey(CWorld* World, int32 Direction)
    {
        TVector<float> Times;
        CollectKeyTimes(Times);

        if (Times.empty())
        {
            return;
        }

        // Half a frame of slack, so a playhead already parked on a key steps off it instead of re-landing.
        const float Slack = 0.5f / (float)Math::Max(Sequence->FrameRate, 1);

        float Target = PlayTime;
        bool bFound = false;

        if (Direction > 0)
        {
            for (float Time : Times)
            {
                if (Time > PlayTime + Slack)
                {
                    Target = Time;
                    bFound = true;
                    break;
                }
            }
        }
        else
        {
            for (int32 Index = (int32)Times.size() - 1; Index >= 0; --Index)
            {
                if (Times[Index] < PlayTime - Slack)
                {
                    Target = Times[Index];
                    bFound = true;
                    break;
                }
            }
        }

        if (bFound)
        {
            bPlaying = false;
            EvaluateAt(World, Math::Clamp(Target, 0.0f, Sequence->Duration), true);
        }
    }

    void FSequencerEditMode::StepFrames(CWorld* World, int32 FrameDelta)
    {
        bPlaying = false;

        const int32 Frame = Math::Clamp(Sequence->TimeToFrame(PlayTime) + FrameDelta,
                                        0, Math::Max(Sequence->GetFrameCount(), 0));

        EvaluateAt(World, Math::Clamp(Sequence->FrameToTime(Frame), 0.0f, Sequence->Duration), true);
    }

    void FSequencerEditMode::FitDurationToContent()
    {
        TVector<float> Times;
        CollectKeyTimes(Times);

        if (Times.empty())
        {
            return;
        }

        Sequence->Duration = Math::Max(Times.back(), 0.1f);
        Sequence->GetPackage()->MarkDirty();

        PlayTime = Math::Min(PlayTime, Sequence->Duration);
        TimelineScroll = 0.0f;
    }

    void FSequencerEditMode::RemoveBinding(CWorld* World, int32 BindingIndex)
    {
        if (!Sequence.IsValid() || BindingIndex < 0 || BindingIndex >= (int32)Sequence->Bindings.size())
        {
            return;
        }

        // Spawned entities belong to this binding, so normal teardown destroys them before it disappears.
        ReleaseBindings(World);

        TVector<TObjectPtr<CSequenceTrack>> Kept;
        Kept.reserve(Sequence->Tracks.size());

        for (const TObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
        {
            if (!Track.IsValid())
            {
                continue;
            }

            if (CSequenceTrack_Transform* Transform = Cast<CSequenceTrack_Transform>(Track.Get()))
            {
                if (Transform->BindingIndex == BindingIndex)
                {
                    continue;
                }

                // Indices above the hole all shift down by one.
                if (Transform->BindingIndex > BindingIndex)
                {
                    --Transform->BindingIndex;
                }
            }
            else if (CSequenceTrack_CameraCut* Cuts = Cast<CSequenceTrack_CameraCut>(Track.Get()))
            {
                // A cut row is shared by every binding, so cuts naming this one go and the rest renumber.
                TVector<SSequenceCameraCut> KeptCuts;
                KeptCuts.reserve(Cuts->Cuts.size());

                for (SSequenceCameraCut Cut : Cuts->Cuts)
                {
                    if (Cut.BindingIndex == BindingIndex)
                    {
                        continue;
                    }

                    if (Cut.BindingIndex > BindingIndex)
                    {
                        --Cut.BindingIndex;
                    }

                    KeptCuts.push_back(Cut);
                }

                Cuts->Cuts = std::move(KeptCuts);
            }

            Kept.push_back(Track);
        }

        Sequence->Tracks = std::move(Kept);
        Sequence->Bindings.erase(Sequence->Bindings.begin() + BindingIndex);
        Sequence->GetPackage()->MarkDirty();

        SelectedBinding = INDEX_NONE;
        SelectedKeyBinding = INDEX_NONE;
        SelectedCut = INDEX_NONE;

        BindToWorld(World);
    }

    void FSequencerEditMode::DrawBindingList(CWorld* World)
    {
        if (ImGui::Button(LE_ICON_PLUS " Bind Selected"))
        {
            SelectedBinding = AddBindingFromSelection(World);
        }
        ImGuiX::TextTooltip("Add the selected world entity as a possessed binding.");

        ImGui::SameLine();
        ImGui::BeginDisabled(SelectedBinding == INDEX_NONE);
        if (ImGui::Button(LE_ICON_KEY " Key Transform"))
        {
            KeyTransform(World, SelectedBinding);
        }
        ImGuiX::TextTooltip("Capture the bound entity's current transform as a key at the playhead.");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(SelectedBinding == INDEX_NONE);
        if (ImGui::Button(LE_ICON_MOVIE_OPEN " Add Camera Cut"))
        {
            AddCameraCutAtPlayhead(World);
        }
        ImGuiX::TextTooltip("Cut to the selected binding's camera at the playhead. Drag the clip's body to move it, its edges to retime it, right-click to remove it.");
        ImGui::EndDisabled();

        ImGui::SameLine();
        bool bAuto = bAutoKey;
        if (ImGui::Checkbox("Auto Key", &bAuto))
        {
            bAutoKey = bAuto;
        }
        ImGuiX::TextTooltip("Key a bound entity automatically whenever you move it. Scrub, pose with the gizmo, repeat.");

        ImGui::SameLine();
        ImGui::TextDisabled("%d bindings, %d tracks",
            (int)Sequence->Bindings.size(), (int)Sequence->Tracks.size());
    }

    void FSequencerEditMode::DrawTimeline(CWorld* World)
    {
        const float Duration = Math::Max(Sequence->Duration, 0.01f);

        ImGui::SetNextItemWidth(-1.0f);
        float ScrubTime = PlayTime;
        if (ImGui::SliderFloat("##Scrub", &ScrubTime, 0.0f, Duration, "%.2fs"))
        {
            bPlaying = false;
            EvaluateAt(World, Sequence->SnapToFrame(ScrubTime), true);
        }

        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float PanelWidth = Math::Max(ImGui::GetContentRegionAvail().x, 128.0f);
        const float TrackWidth = Math::Max(PanelWidth - SequencerLabelWidth, 64.0f);
        const float TrackLeft = Origin.x + SequencerLabelWidth;

        const int32 RowCount = (int32)Sequence->Bindings.size() + 1;
        const float BodyTop = Origin.y + SequencerRulerHeight;
        const float BodyHeight = (float)RowCount * SequencerTrackHeight;
        const float TotalHeight = SequencerRulerHeight + BodyHeight;

        ImGui::InvisibleButton("##Timeline", ImVec2(PanelWidth, TotalHeight));
        const bool bTimelineHovered = ImGui::IsItemHovered();

        ImDrawList* DrawList = ImGui::GetWindowDrawList();

        // Zooming about the cursor keeps the frame under it put, which feels like a camera not a slider.
        const float VisibleSeconds = Duration * TimelineZoom;
        if (bTimelineHovered && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
        {
            const float Anchor01 = Math::Clamp((ImGui::GetMousePos().x - TrackLeft) / TrackWidth, 0.0f, 1.0f);
            const float AnchorTime = TimelineScroll + Anchor01 * VisibleSeconds;

            TimelineZoom = Math::Clamp(TimelineZoom * (ImGui::GetIO().MouseWheel > 0.0f ? 0.85f : 1.18f), 0.02f, 1.0f);
            TimelineScroll = Math::Clamp(AnchorTime - Anchor01 * (Duration * TimelineZoom), 0.0f,
                                         Math::Max(Duration - Duration * TimelineZoom, 0.0f));
        }
        else if (bTimelineHovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            TimelineScroll = Math::Clamp(TimelineScroll - ImGui::GetIO().MouseWheel * VisibleSeconds * 0.1f,
                                         0.0f, Math::Max(Duration - VisibleSeconds, 0.0f));
        }

        const float ViewSeconds = Math::Max(Duration * TimelineZoom, 0.01f);
        const float ViewStart = TimelineScroll;

        VisibleTimeToX = [=](float Time) { return TrackLeft + ((Time - ViewStart) / ViewSeconds) * TrackWidth; };
        VisibleXToTime = [=](float X) { return ViewStart + ((X - TrackLeft) / Math::Max(TrackWidth, 1.0f)) * ViewSeconds; };

        DrawList->AddRectFilled(Origin, ImVec2(Origin.x + PanelWidth, Origin.y + TotalHeight), SeqPanelBg, 4.0f);

        DrawTimeRuler(DrawList, Origin, TrackLeft, TrackWidth);

        // Vertical grid behind the rows, on the same steps as the ruler labels.
        const float Step = ChooseRulerStep(ViewSeconds, TrackWidth);
        for (float T = Math::Floor(ViewStart / Step) * Step; T <= ViewStart + ViewSeconds; T += Step)
        {
            const float X = VisibleTimeToX(T);
            if (X >= TrackLeft)
            {
                DrawList->AddLine(ImVec2(X, BodyTop), ImVec2(X, BodyTop + BodyHeight), SeqGridMinor);
            }
        }

        const float CutRowHeight = DrawCameraCutRow(World, DrawList, ImVec2(Origin.x, BodyTop), TrackLeft, TrackWidth, Duration);

        for (int32 i = 0; i < (int32)Sequence->Bindings.size(); ++i)
        {
            const float RowY = BodyTop + CutRowHeight + (float)i * SequencerTrackHeight;

            const ImVec2 RowMin(Origin.x, RowY);
            const ImVec2 RowMax(Origin.x + PanelWidth, RowY + SequencerTrackHeight);

            const bool bSelected = (i == SelectedBinding);
            const bool bHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(RowMin, RowMax);

            const ImU32 RowColor = bSelected ? SeqRowSelBg
                                 : (bHovered ? SeqRowHoverBg : ((i & 1) ? SeqRowAltBg : SeqRowBg));
            DrawList->AddRectFilled(RowMin, RowMax, RowColor);

            // So a long name reads as clipped rather than as running into the keys.
            DrawList->AddLine(ImVec2(TrackLeft, RowY), ImVec2(TrackLeft, RowMax.y), SeqGridMajor);

            const FName& Name = Sequence->Bindings[i].Name;
            DrawList->PushClipRect(RowMin, ImVec2(TrackLeft - 4.0f, RowMax.y), true);
            DrawList->AddText(ImVec2(Origin.x + 10.0f, RowY + 5.0f), bSelected ? SeqText : SeqTextDim, Name.c_str());
            DrawList->PopClipRect();

            // The track area to the right belongs to keys and cuts, which have their own context menus.
            const bool bOnLabel = bHovered && ImGui::GetMousePos().x < TrackLeft;

            if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedBinding = i;
            }

            if (bOnLabel && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedBinding = i;
                ImGui::OpenPopup("##BindingContext");
            }

            DrawList->PushClipRect(ImVec2(TrackLeft, RowY), RowMax, true);

            for (const TObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
            {
                const CSequenceTrack_Transform* Transform = Track.IsValid()
                    ? Cast<CSequenceTrack_Transform>(Track.Get()) : nullptr;

                if (Transform == nullptr || Transform->BindingIndex != i)
                {
                    continue;
                }

                // The three channels are keyed together, so overlapping diamond rows would be noise.
                const float RowMidY = RowY + SequencerTrackHeight * 0.5f;

                // Snapshotted, since a drag retimes and re-sorts the very array being walked.
                struct FKeyView { float Time; ECurveInterpMode Interp; };
                TVector<FKeyView> KeyViews;
                for (const SCurveKey& Key : Transform->Location.X.Resolve().Keys)
                {
                    KeyViews.push_back(FKeyView{ Key.Time, Key.InterpMode });
                }

                for (const FKeyView& KeyView : KeyViews)
                {
                    const float KeyTime = KeyView.Time;
                    const float X = VisibleTimeToX(KeyTime);

                    const bool bIsSelected = (SelectedKeyBinding == i)
                                          && Math::Abs(KeyTime - SelectedKeyTime) <= 0.002f;

                    const float R = bIsSelected ? 7.5f : 6.0f;
                    const ImU32 Fill = bIsSelected ? IM_COL32(255, 255, 255, 255) : SeqKey;

                    // Square steps, diamond ramps and circle eases, so the mode reads without opening a menu.
                    switch (KeyView.Interp)
                    {
                    case ECurveInterpMode::Constant:
                        DrawList->AddRectFilled(ImVec2(X - R * 0.8f, RowMidY - R * 0.8f),
                                                ImVec2(X + R * 0.8f, RowMidY + R * 0.8f), Fill, 1.0f);
                        DrawList->AddRect(ImVec2(X - R * 0.8f, RowMidY - R * 0.8f),
                                          ImVec2(X + R * 0.8f, RowMidY + R * 0.8f), SeqKeyOutline, 1.0f, 0, 1.5f);
                        break;

                    case ECurveInterpMode::Cubic:
                    case ECurveInterpMode::CubicUser:
                        DrawList->AddCircleFilled(ImVec2(X, RowMidY), R * 0.9f, Fill, 12);
                        DrawList->AddCircle(ImVec2(X, RowMidY), R * 0.9f, SeqKeyOutline, 12, 1.5f);
                        break;

                    default:
                        DrawList->AddQuadFilled(ImVec2(X, RowMidY - R), ImVec2(X + R, RowMidY),
                                                ImVec2(X, RowMidY + R), ImVec2(X - R, RowMidY), Fill);
                        DrawList->AddQuad(ImVec2(X, RowMidY - R), ImVec2(X + R, RowMidY),
                                          ImVec2(X, RowMidY + R), ImVec2(X - R, RowMidY), SeqKeyOutline, 1.5f);
                        break;
                    }

                    const ImVec2 HitMin(X - R - 2.0f, RowMidY - R - 2.0f);
                    const ImVec2 HitMax(X + R + 2.0f, RowMidY + R + 2.0f);
                    const bool bKeyHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(HitMin, HitMax);

                    if (bKeyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bDraggingKey)
                    {
                        SelectedBinding = i;
                        SelectedKeyBinding = i;
                        SelectedKeyTime = KeyTime;
                        bDraggingKey = true;
                    }

                    if (bKeyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    {
                        SelectedBinding = i;
                        SelectedKeyBinding = i;
                        SelectedKeyTime = KeyTime;
                        ImGui::OpenPopup("##KeyContext");
                    }
                }
            }

            DrawList->PopClipRect();
        }

        // Playhead last, clipped to the track area so it never draws over the name column.
        const float PlayheadX = VisibleTimeToX(Math::Clamp(PlayTime, 0.0f, Duration));
        if (PlayheadX >= TrackLeft && PlayheadX <= TrackLeft + TrackWidth)
        {
            DrawList->AddLine(ImVec2(PlayheadX, Origin.y), ImVec2(PlayheadX, Origin.y + TotalHeight), SeqPlayhead, 2.0f);
            DrawList->AddTriangleFilled(ImVec2(PlayheadX - 6.0f, Origin.y), ImVec2(PlayheadX + 6.0f, Origin.y),
                                        ImVec2(PlayheadX, Origin.y + 9.0f), SeqPlayhead);
        }

        // Runs after the rows so it sees this frame's selection, retiming every channel at once.
        if (bDraggingKey)
        {
            CSequenceTrack_Transform* Track = FindTransformTrack(SelectedKeyBinding);

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || Track == nullptr)
            {
                bDraggingKey = false;
            }
            else
            {
                const float NewTime = Sequence->SnapToFrame(
                    Math::Clamp(VisibleXToTime(ImGui::GetMousePos().x), 0.0f, Duration));

                if (Math::Abs(NewTime - SelectedKeyTime) > 1e-4f)
                {
                    MoveTransformKeys(Track, SelectedKeyTime, NewTime);
                    SelectedKeyTime = NewTime;
                    Sequence->GetPackage()->MarkDirty();

                    // Re-evaluate so the viewport shows the retimed shot as the key moves.
                    EvaluateAt(World, PlayTime, true);
                }
            }
        }

        if (ImGui::BeginPopup("##BindingContext"))
        {
            ImGui::BeginDisabled(SelectedBinding == INDEX_NONE);

            if (ImGui::MenuItem(LE_ICON_KEY " Key Transform"))
            {
                KeyTransform(World, SelectedBinding);
            }

            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_DELETE " Remove Binding"))
            {
                PendingRemoveBinding = SelectedBinding;
            }
            ImGuiX::TextTooltip("Removes the binding along with its tracks and any cuts pointing at it.");

            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("##KeyContext"))
        {
            CSequenceTrack_Transform* InterpTrack = FindTransformTrack(SelectedKeyBinding);
            const ECurveInterpMode CurrentInterp = GetTransformKeyInterp(InterpTrack, SelectedKeyTime);

            // The mode belongs to the key it leaves, so this shapes the segment from here to the next key.
            if (ImGui::BeginMenu(LE_ICON_VECTOR_CURVE " Interpolation"))
            {
                struct FInterpChoice { const char* Label; ECurveInterpMode Mode; };
                const FInterpChoice Choices[] =
                {
                    { LE_ICON_STAIRS " Constant",           ECurveInterpMode::Constant },
                    { LE_ICON_VECTOR_LINE " Linear",        ECurveInterpMode::Linear   },
                    { LE_ICON_CHART_BELL_CURVE " Cubic",    ECurveInterpMode::Cubic    },
                };

                for (const FInterpChoice& Choice : Choices)
                {
                    if (ImGui::MenuItem(Choice.Label, nullptr, CurrentInterp == Choice.Mode))
                    {
                        SetTransformKeyInterp(InterpTrack, SelectedKeyTime, Choice.Mode);
                        Sequence->GetPackage()->MarkDirty();
                        EvaluateAt(World, PlayTime, true);
                    }
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_DELETE " Delete Key"))
            {
                if (CSequenceTrack_Transform* Track = FindTransformTrack(SelectedKeyBinding))
                {
                    DeleteTransformKeys(Track, SelectedKeyTime);
                    Sequence->GetPackage()->MarkDirty();
                    EvaluateAt(World, PlayTime, true);
                }

                SelectedKeyBinding = INDEX_NONE;
            }
            ImGui::EndPopup();
        }

        // Delete removes the selected key, matching the context menu without needing the right-click.
        if (SelectedKeyBinding != INDEX_NONE && !bDraggingKey && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            if (CSequenceTrack_Transform* Track = FindTransformTrack(SelectedKeyBinding))
            {
                DeleteTransformKeys(Track, SelectedKeyTime);
                Sequence->GetPackage()->MarkDirty();
                EvaluateAt(World, PlayTime, true);
            }

            SelectedKeyBinding = INDEX_NONE;
        }

        // Click or drag anywhere on the ruler to scrub, which is where the hand goes for it.
        const ImVec2 RulerMin(TrackLeft, Origin.y);
        const ImVec2 RulerMax(TrackLeft + TrackWidth, Origin.y + SequencerRulerHeight);
        if (bTimelineHovered && ImGui::IsMouseHoveringRect(RulerMin, RulerMax)
            && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
        {
            bPlaying = false;
            EvaluateAt(World, Sequence->SnapToFrame(Math::Clamp(VisibleXToTime(ImGui::GetMousePos().x), 0.0f, Duration)), true);
        }
    }
}
