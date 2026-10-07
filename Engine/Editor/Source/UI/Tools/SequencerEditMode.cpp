#include "SequencerEditMode.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Assets/AssetTypes/Audio/SoundBase.h"
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

    // Uniform access to what each kind of track keys, so rows, drags and jump-to-key treat them alike.
    namespace SequencerTracks
    {
        static SKeyedCurve* Curve(CSequenceTrack* Track)
        {
            if (CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))        { return &Property->Curve.Curve; }
            if (CSequenceTrack_Fade* Fade = Cast<CSequenceTrack_Fade>(Track))                    { return &Fade->Amount.Curve; }
            if (CSequenceTrack_TimeDilation* Dilation = Cast<CSequenceTrack_TimeDilation>(Track)) { return &Dilation->Scale.Curve; }
            if (CSequenceTrack_LookAt* LookAt = Cast<CSequenceTrack_LookAt>(Track))              { return &LookAt->Weight.Curve; }
            if (CSequenceTrack_CameraShake* Shake = Cast<CSequenceTrack_CameraShake>(Track))     { return &Shake->Intensity.Curve; }
            return nullptr;
        }

        // What an unkeyed curve means to its track, and so what a first key should hold.
        static float RestingValue(const CSequenceTrack* Track)
        {
            return Track->IsA<CSequenceTrack_Fade>() ? 0.0f : 1.0f;
        }

        static int32 KeyCount(CSequenceTrack* Track)
        {
            if (SKeyedCurve* Keys = Curve(Track))                                  { return Keys->NumKeys(); }
            if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track)) { return (int32)Events->Keys.size(); }
            if (CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))  { return (int32)Audio->Clips.size(); }
            return 0;
        }

        static float* KeyTime(CSequenceTrack* Track, int32 Index)
        {
            if (Index < 0 || Index >= KeyCount(Track))
            {
                return nullptr;
            }
            if (SKeyedCurve* Keys = Curve(Track))                                  { return &Keys->Keys[Index].Time; }
            if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track)) { return &Events->Keys[Index].Time; }
            if (CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))  { return &Audio->Clips[Index].StartTime; }
            return nullptr;
        }

        // Re-sorts after a retime and returns where the moved key landed.
        static int32 SortKeys(CSequenceTrack* Track, int32 Index)
        {
            float* Moved = KeyTime(Track, Index);
            if (Moved == nullptr)
            {
                return Constants::kIndexNone;
            }
            const float Time = *Moved;

            if (SKeyedCurve* Keys = Curve(Track))
            {
                const SCurveKey Key = Keys->Keys[Index];
                Keys->Keys.erase(Keys->Keys.begin() + Index);
                auto Where = std::upper_bound(Keys->Keys.begin(), Keys->Keys.end(), Time,
                    [](float Value, const SCurveKey& Other) { return Value < Other.Time; });
                const int32 Landed = (int32)(Where - Keys->Keys.begin());
                Keys->Keys.insert(Where, Key);
                Keys->ComputeAutoTangents();
                return Landed;
            }
            if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track))
            {
                const SSequenceEventKey Key = Events->Keys[Index];
                Events->Keys.erase(Events->Keys.begin() + Index);
                auto Where = std::upper_bound(Events->Keys.begin(), Events->Keys.end(), Time,
                    [](float Value, const SSequenceEventKey& Other) { return Value < Other.Time; });
                const int32 Landed = (int32)(Where - Events->Keys.begin());
                Events->Keys.insert(Where, Key);
                return Landed;
            }
            return Index;
        }

        static void RemoveKey(CSequenceTrack* Track, int32 Index)
        {
            if (Index < 0 || Index >= KeyCount(Track))
            {
                return;
            }
            if (SKeyedCurve* Keys = Curve(Track))
            {
                Keys->RemoveKey(Index);
                Keys->ComputeAutoTangents();
            }
            else if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track))
            {
                Events->Keys.erase(Events->Keys.begin() + Index);
            }
            else if (CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))
            {
                Audio->Clips.erase(Audio->Clips.begin() + Index);
            }
        }

        static FString RowLabel(const CSequenceTrack* Track)
        {
            FString Label(Track->GetTrackDisplayName().data(), Track->GetTrackDisplayName().size());
            if (const CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))
            {
                Label = Property->PropertyPath.empty() ? FString("Property (unset)") : Property->PropertyPath;
            }
            return Label;
        }
    }

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

        Instance.bDriveCamera = bPreviewCameras;
        Instance.Bind(Sequence.Get(), World);
        RefreshAutoKeyWatch(World);
        bBound = Instance.bBound;
    }

    void FSequencerEditMode::ReleaseBindings(CWorld* World)
    {
        Instance.Release(World, true);
        Instance.FiredEvents.clear();
        bBound = false;
    }

    void FSequencerEditMode::EvaluateAt(CWorld* World, float NewTime, bool bJumped)
    {
        if (World == nullptr || !Sequence.IsValid() || !bBound)
        {
            return;
        }

        // Stopped means scrubbing, which should not set the score off.
        Instance.Evaluate(Sequence.Get(), World, NewTime, PlayTime, bJumped, false, false, !bPlaying);
        PlayTime = NewTime;

        // Nothing consumes events in the editor, so the latest is shown and the rest dropped.
        if (!Instance.FiredEvents.empty())
        {
            LastFiredEvent = FString(Instance.FiredEvents.back().Name.ToString().c_str());
            LastFiredEventTime = ImGui::GetTime();
            Instance.FiredEvents.clear();
        }

        // Rebasing stops auto-key treating the sequence's own output as an edit on every scrubbed frame.
        RefreshAutoKeyWatch(World);
    }

    void FSequencerEditMode::RefreshAutoKeyWatch(CWorld* World)
    {
        AutoKeyWatch.assign(Instance.BoundEntities.size(), FRestoreEntry());

        if (World == nullptr)
        {
            return;
        }

        for (int32 i = 0; i < (int32)Instance.BoundEntities.size(); ++i)
        {
            const ECS::FEntity Entity = Instance.BoundEntities[i];
            if (Entity == ECS::NullEntity || !World->IsValidEntity(Entity))
            {
                continue;
            }

            if (const STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entity))
            {
                FRestoreEntry& Entry = AutoKeyWatch[i];
                Entry.Entity = Entity;
                Entry.Location = Transform->GetLocation();
                Entry.Rotation = Math::Degrees(Math::YawFirstEulerAngles(Transform->GetRotation()));
                Entry.Scale = Transform->GetScale();
            }
        }
    }

    void FSequencerEditMode::ProcessAutoKey(CWorld* World)
    {
        if (World == nullptr || !bBound || AutoKeyWatch.size() != Instance.BoundEntities.size())
        {
            return;
        }

        // Loose enough that the gizmo's matrix round-trip does not key, tight enough that a nudge does.
        constexpr float PositionEpsilonSq = 1e-6f;
        constexpr float AngleEpsilonSq    = 1e-4f;

        for (int32 i = 0; i < (int32)Instance.BoundEntities.size(); ++i)
        {
            const ECS::FEntity Entity = Instance.BoundEntities[i];
            if (Entity == ECS::NullEntity || !World->IsValidEntity(Entity) || AutoKeyWatch[i].Entity != Entity)
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
            const FVector3 Rotation = Math::Degrees(Math::YawFirstEulerAngles(Transform->GetRotation()));
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

    ECS::FEntity FSequencerEditMode::FindSelectedEntity(CWorld* World) const
    {
        if (World == nullptr)
        {
            return ECS::NullEntity;
        }

        auto View = World->View<FSelectedInEditorComponent>();
        for (ECS::FEntity Entity : View)
        {
            return Entity;
        }

        return ECS::NullEntity;
    }

    int32 FSequencerEditMode::AddBindingFromSelection(CWorld* World)
    {
        const ECS::FEntity Selected = FindSelectedEntity(World);
        if (Selected == ECS::NullEntity || !Sequence.IsValid())
        {
            ImGuiX::Notifications::NotifyWarning("Select an entity in the world to bind it.");
            return Constants::kIndexNone;
        }

        FName EntityName = "Entity";
        if (const SNameComponent* NameComponent = World->TryGetComponent<SNameComponent>(Selected))
        {
            EntityName = NameComponent->Name;
        }

        const int32 Existing = Algo::IndexOf(Sequence->Bindings, EntityName, &SSequenceBinding::Name);
        if (Existing != Constants::kIndexNone)
        {
            return Existing;
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
        for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
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
            || BindingIndex < 0 || BindingIndex >= (int32)Instance.BoundEntities.size())
        {
            return;
        }

        const ECS::FEntity Entity = Instance.BoundEntities[BindingIndex];
        if (Entity == ECS::NullEntity || !World->IsValidEntity(Entity))
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
        const FVector3 Rotation = Math::Degrees(Math::YawFirstEulerAngles(Transform->GetRotation()));
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
        Track->Rotation.UnwindAngles();

        Sequence->GetPackage()->MarkDirty();
    }

    CSequenceTrack_Transform* FSequencerEditMode::FindTransformTrack(int32 BindingIndex) const
    {
        if (!Sequence.IsValid())
        {
            return nullptr;
        }

        for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
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

            const auto KeyItr = Algo::FindIf(Channel->Curve.Keys,
                [&](const SCurveKey& Key) { return Math::Abs(Key.Time - FromTime) <= MatchEpsilon; });

            if (KeyItr != Channel->Curve.Keys.end())
            {
                KeyItr->Time = ToTime;
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

            const auto KeyItr = Algo::FindIf(Channel->Curve.Keys,
                [&](const SCurveKey& Key) { return Math::Abs(Key.Time - AtTime) <= MatchEpsilon; });

            if (KeyItr != Channel->Curve.Keys.end())
            {
                KeyItr->InterpMode = Mode;
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

        const TVector<SCurveKey>& Keys = Track->Location.X.Resolve().Keys;
        const auto KeyItr = Algo::FindIf(Keys,
            [&](const SCurveKey& Key) { return Math::Abs(Key.Time - AtTime) <= MatchEpsilon; });

        return KeyItr != Keys.end() ? KeyItr->InterpMode : ECurveInterpMode::Linear;
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

        for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
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
        if (!Sequence.IsValid() || SelectedBinding == Constants::kIndexNone)
        {
            ImGuiX::Notifications::NotifyWarning("Select the binding holding the camera you want to cut to.");
            return;
        }

        // A binding with no camera would produce a cut that silently does nothing at runtime.
        if (SelectedBinding < (int32)Instance.BoundEntities.size())
        {
            const ECS::FEntity Entity = Instance.BoundEntities[SelectedBinding];
            if (Entity != ECS::NullEntity && World->IsValidEntity(Entity)
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

        int32 PendingRemoval = Constants::kIndexNone;

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

            if (DraggingCut == Constants::kIndexNone && bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
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

        if (DraggingCut != Constants::kIndexNone)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || DraggingCut >= (int32)Track->Cuts.size())
            {
                DraggingCut = Constants::kIndexNone;
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
        if (PendingRemoval != Constants::kIndexNone)
        {
            Track->Cuts.erase(Track->Cuts.begin() + PendingRemoval);
            SelectedCut = Constants::kIndexNone;
            DraggingCut = Constants::kIndexNone;
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
        DrawDetails(World);

        // The request comes from a popup, and the row loop above iterates the arrays this resizes.
        if (PendingRemoveBinding != Constants::kIndexNone)
        {
            RemoveBinding(World, PendingRemoveBinding);
            PendingRemoveBinding = Constants::kIndexNone;
        }

        if (PendingRemoveTrack != Constants::kIndexNone)
        {
            RemoveTrack(World, PendingRemoveTrack);
            PendingRemoveTrack = Constants::kIndexNone;
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

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        float LetterboxEntry = Sequence->LetterboxAspect;
        const char* LetterboxFormat = LetterboxEntry > 0.0f ? "Bars %.2f" : "No bars";
        if (ImGui::DragFloat("##Letterbox", &LetterboxEntry, 0.01f, 0.0f, 4.0f, LetterboxFormat))
        {
            Sequence->LetterboxAspect = Math::Clamp(LetterboxEntry, 0.0f, 4.0f);
            Sequence->GetPackage()->MarkDirty();
            EvaluateAt(World, PlayTime, true);
        }
        ImGuiX::TextTooltip("Black bars at this width over height for the whole sequence, such as 2.39. Zero shows none.");

        ImGui::SameLine();
        bool bPreview = bPreviewCameras;
        if (ImGui::Checkbox(LE_ICON_MOVIE_OPEN " Preview Cuts", &bPreview))
        {
            SetPreviewCameras(World, bPreview);
        }
        ImGuiX::TextTooltip("Look through the camera each cut makes live. Off keeps your own camera so you can fly through a shot.");

        if (!LastFiredEvent.empty() && ImGui::GetTime() - LastFiredEventTime < 1.5)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), LE_ICON_FLAG " %s", LastFiredEvent.c_str());
        }
    }

    void FSequencerEditMode::CollectKeyTimes(TVector<float>& OutTimes) const
    {
        OutTimes.clear();

        if (!Sequence.IsValid())
        {
            return;
        }

        for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
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
            else
            {
                CSequenceTrack* Mutable = const_cast<CSequenceTrack*>(Track.Get());
                for (int32 Key = 0; Key < SequencerTracks::KeyCount(Mutable); ++Key)
                {
                    OutTimes.push_back(*SequencerTracks::KeyTime(Mutable, Key));
                }
            }
        }

        Algo::Sort(OutTimes);
        OutTimes.erase(Algo::Unique(OutTimes), OutTimes.end());
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

        TVector<TStrongObjectPtr<CSequenceTrack>> Kept;
        Kept.reserve(Sequence->Tracks.size());

        for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
        {
            if (!Track.IsValid())
            {
                continue;
            }

            if (Track->BindingIndex == BindingIndex)
            {
                continue;
            }

            // Indices above the hole all shift down by one.
            if (Track->BindingIndex > BindingIndex)
            {
                --Track->BindingIndex;
            }

            if (CSequenceTrack_LookAt* LookAt = Cast<CSequenceTrack_LookAt>(Track.Get()))
            {
                if (LookAt->TargetBindingIndex == BindingIndex)
                {
                    LookAt->TargetBindingIndex = Constants::kIndexNone;
                }
                else if (LookAt->TargetBindingIndex > BindingIndex)
                {
                    --LookAt->TargetBindingIndex;
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

        SelectedBinding = Constants::kIndexNone;
        SelectedKeyBinding = Constants::kIndexNone;
        SelectedCut = Constants::kIndexNone;
        SelectedTrack = Constants::kIndexNone;
        SelectedTrackKey = Constants::kIndexNone;

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
        ImGui::BeginDisabled(SelectedBinding == Constants::kIndexNone);
        if (ImGui::Button(LE_ICON_KEY " Key Transform"))
        {
            KeyTransform(World, SelectedBinding);
        }
        ImGuiX::TextTooltip("Capture the bound entity's current transform as a key at the playhead.");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(SelectedBinding == Constants::kIndexNone);
        if (ImGui::Button(LE_ICON_MOVIE_OPEN " Add Camera Cut"))
        {
            AddCameraCutAtPlayhead(World);
        }
        ImGuiX::TextTooltip("Cut to the selected binding's camera at the playhead. Drag the clip's body to move it, its edges to retime it, right-click to remove it.");
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CAMERA " New Camera"))
        {
            AddCameraFromView(World);
        }
        ImGuiX::TextTooltip("Add a cinematic camera where the editor camera is, keyed at the playhead and cut to from here.");

        ImGui::SameLine();
        ImGui::BeginDisabled(SelectedBinding == Constants::kIndexNone);
        if (ImGui::Button(LE_ICON_CAMERA_IRIS " Key From View"))
        {
            KeyFromView(World, SelectedBinding);
        }
        ImGuiX::TextTooltip("Key the selected binding to where the editor camera is. Turn off Preview Cuts, fly to the next pose, key again.");
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Track"))
        {
            ImGui::OpenPopup("##AddTrack");
        }
        ImGuiX::TextTooltip("Add a track. Tracks that drive something go on the selected binding, the rest on the whole sequence.");
        DrawAddTrackMenu(World);

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

        // Each binding's row is followed by its other tracks, then the tracks that act on the whole sequence.
        struct FTimelineRow { int32 Binding = Constants::kIndexNone; int32 Track = Constants::kIndexNone; };
        TVector<FTimelineRow> Rows;
        const auto IsRowTrack = [](const CSequenceTrack* Track)
        {
            return Track != nullptr && !Track->IsA<CSequenceTrack_Transform>() && !Track->IsA<CSequenceTrack_CameraCut>();
        };
        for (int32 BindingIndex = 0; BindingIndex < (int32)Sequence->Bindings.size(); ++BindingIndex)
        {
            Rows.push_back({ BindingIndex, Constants::kIndexNone });
            for (int32 TrackIndex = 0; TrackIndex < (int32)Sequence->Tracks.size(); ++TrackIndex)
            {
                const CSequenceTrack* Track = Sequence->Tracks[TrackIndex].Get();
                if (IsRowTrack(Track) && Track->BindingIndex == BindingIndex)
                {
                    Rows.push_back({ BindingIndex, TrackIndex });
                }
            }
        }
        for (int32 TrackIndex = 0; TrackIndex < (int32)Sequence->Tracks.size(); ++TrackIndex)
        {
            const CSequenceTrack* Track = Sequence->Tracks[TrackIndex].Get();
            if (IsRowTrack(Track) && (Track->BindingIndex < 0 || Track->BindingIndex >= (int32)Sequence->Bindings.size()))
            {
                Rows.push_back({ Constants::kIndexNone, TrackIndex });
            }
        }

        const int32 RowCount = (int32)Rows.size() + 1;
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

        for (int32 RowIndex = 0; RowIndex < (int32)Rows.size(); ++RowIndex)
        {
            const float RowY = BodyTop + CutRowHeight + (float)RowIndex * SequencerTrackHeight;
            if (Rows[RowIndex].Track != Constants::kIndexNone)
            {
                DrawTrackRow(World, DrawList, Rows[RowIndex].Track, RowIndex, RowY, Origin.x, PanelWidth, TrackLeft, TrackWidth, bTimelineHovered);
                continue;
            }
            const int32 i = Rows[RowIndex].Binding;

            const ImVec2 RowMin(Origin.x, RowY);
            const ImVec2 RowMax(Origin.x + PanelWidth, RowY + SequencerTrackHeight);

            const bool bSelected = (i == SelectedBinding);
            const bool bHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(RowMin, RowMax);

            const ImU32 RowColor = bSelected ? SeqRowSelBg
                                 : (bHovered ? SeqRowHoverBg : ((RowIndex & 1) ? SeqRowAltBg : SeqRowBg));
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
                SelectedTrack = Constants::kIndexNone;
                SelectedTrackKey = Constants::kIndexNone;
            }

            if (bOnLabel && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedBinding = i;
                ImGui::OpenPopup("##BindingContext");
            }

            DrawList->PushClipRect(ImVec2(TrackLeft, RowY), RowMax, true);

            for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
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
                                          ImVec2(X + R * 0.8f, RowMidY + R * 0.8f), SeqKeyOutline, 1.0f, 1.5f, 0);
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

        if (bDraggingTrackKey)
        {
            CSequenceTrack* Track = SelectedTrack >= 0 && SelectedTrack < (int32)Sequence->Tracks.size() ? Sequence->Tracks[SelectedTrack].Get() : nullptr;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || Track == nullptr || SelectedTrackKey == Constants::kIndexNone)
            {
                bDraggingTrackKey = false;
            }
            else
            {
                const float NewTime = Sequence->SnapToFrame(Math::Clamp(VisibleXToTime(ImGui::GetMousePos().x), 0.0f, Duration));
                float* KeyTime = SequencerTracks::KeyTime(Track, SelectedTrackKey);
                if (KeyTime != nullptr && Math::Abs(NewTime - *KeyTime) > 1e-4f)
                {
                    *KeyTime = NewTime;
                    SelectedTrackKey = SequencerTracks::SortKeys(Track, SelectedTrackKey);
                    Sequence->GetPackage()->MarkDirty();
                    EvaluateAt(World, PlayTime, true);
                }
            }
        }

        DrawTrackPopups(World);

        if (ImGui::BeginPopup("##BindingContext"))
        {
            ImGui::BeginDisabled(SelectedBinding == Constants::kIndexNone);

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

                SelectedKeyBinding = Constants::kIndexNone;
            }
            ImGui::EndPopup();
        }

        // Delete removes the selected key, matching the context menu without needing the right-click.
        if (SelectedKeyBinding != Constants::kIndexNone && !bDraggingKey && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            if (CSequenceTrack_Transform* Track = FindTransformTrack(SelectedKeyBinding))
            {
                DeleteTransformKeys(Track, SelectedKeyTime);
                Sequence->GetPackage()->MarkDirty();
                EvaluateAt(World, PlayTime, true);
            }

            SelectedKeyBinding = Constants::kIndexNone;
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

    void FSequencerEditMode::DrawTrackRow(CWorld* World, ImDrawList* DrawList, int32 TrackIndex, int32 RowIndex, float RowY,
                                          float Left, float PanelWidth, float TrackLeft, float TrackWidth, bool bTimelineHovered)
    {
        CSequenceTrack* Track = Sequence->Tracks[TrackIndex].Get();

        const ImVec2 RowMin(Left, RowY);
        const ImVec2 RowMax(Left + PanelWidth, RowY + SequencerTrackHeight);
        const bool bSelected = TrackIndex == SelectedTrack;
        const bool bHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(RowMin, RowMax);
        DrawList->AddRectFilled(RowMin, RowMax, bSelected ? SeqRowSelBg : (bHovered ? SeqRowHoverBg : ((RowIndex & 1) ? SeqRowAltBg : SeqRowBg)));
        DrawList->AddLine(ImVec2(TrackLeft, RowY), ImVec2(TrackLeft, RowMax.y), SeqGridMajor);

        const FVector4 Tint = Track->GetTrackColor();
        const float Alpha = Track->bEnabled ? 1.0f : 0.35f;
        const ImU32 TintColor = ImGui::ColorConvertFloat4ToU32(ImVec4(Tint.x, Tint.y, Tint.z, Alpha));
        const ImU32 TintFaint = ImGui::ColorConvertFloat4ToU32(ImVec4(Tint.x, Tint.y, Tint.z, 0.28f * Alpha));

        // Indented under its binding, so the row reads as belonging to the entity above it.
        const float Indent = Track->BindingIndex != Constants::kIndexNone ? 24.0f : 10.0f;
        DrawList->AddRectFilled(ImVec2(Left + Indent - 7.0f, RowY + 6.0f), ImVec2(Left + Indent - 3.0f, RowMax.y - 6.0f), TintColor, 1.0f);

        const FString Label = SequencerTracks::RowLabel(Track);
        DrawList->PushClipRect(RowMin, ImVec2(TrackLeft - 4.0f, RowMax.y), true);
        DrawList->AddText(ImVec2(Left + Indent, RowY + 5.0f), Track->bEnabled ? (bSelected ? SeqText : SeqTextDim) : IM_COL32(110, 114, 124, 255), Label.c_str());
        DrawList->PopClipRect();

        const bool bOnLabel = bHovered && ImGui::GetMousePos().x < TrackLeft;
        if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bDraggingTrackKey)
        {
            SelectedTrack = TrackIndex;
            SelectedTrackKey = Constants::kIndexNone;
            SelectedKeyBinding = Constants::kIndexNone;
        }
        if (bOnLabel && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            SelectedTrack = TrackIndex;
            ImGui::OpenPopup("##TrackContext");
        }

        DrawList->PushClipRect(ImVec2(TrackLeft, RowY), RowMax, true);
        const float MidY = RowY + SequencerTrackHeight * 0.5f;

        // The curve's shape behind its keys, so a fade or a speed ramp reads without opening an editor.
        if (SKeyedCurve* Keys = SequencerTracks::Curve(Track); Keys != nullptr && Keys->NumKeys() > 0)
        {
            float MinValue = 0.0f;
            float MaxValue = 1.0f;
            Keys->GetValueRange(MinValue, MaxValue);
            MinValue = Math::Min(MinValue, 0.0f);
            MaxValue = Math::Max(MaxValue, MinValue + 1e-3f);

            const float Top = RowY + 4.0f;
            const float Height = SequencerTrackHeight - 8.0f;
            ImVec2 Previous;
            for (float X = TrackLeft; X <= TrackLeft + TrackWidth; X += 3.0f)
            {
                const float Value = Keys->Evaluate(VisibleXToTime(X));
                const ImVec2 Point(X, Top + Height * (1.0f - (Value - MinValue) / (MaxValue - MinValue)));
                if (X > TrackLeft)
                {
                    DrawList->AddQuadFilled(ImVec2(Previous.x, RowMax.y - 4.0f), Previous, Point, ImVec2(Point.x, RowMax.y - 4.0f), TintFaint);
                    DrawList->AddLine(Previous, Point, TintColor, 1.5f);
                }
                Previous = Point;
            }
        }

        const int32 KeyCount = SequencerTracks::KeyCount(Track);
        for (int32 KeyIndex = 0; KeyIndex < KeyCount; ++KeyIndex)
        {
            const float Time = *SequencerTracks::KeyTime(Track, KeyIndex);
            const float X = VisibleTimeToX(Time);
            const bool bKeySelected = bSelected && KeyIndex == SelectedTrackKey;
            const ImU32 Fill = bKeySelected ? IM_COL32(255, 255, 255, 255) : TintColor;
            float HitRight = X + 6.0f;

            if (const CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track))
            {
                // A flag on a pole, named, since an event's name is the whole point of it.
                DrawList->AddLine(ImVec2(X, RowY + 3.0f), ImVec2(X, RowMax.y - 3.0f), Fill, 2.0f);
                DrawList->AddTriangleFilled(ImVec2(X, RowY + 3.0f), ImVec2(X + 9.0f, RowY + 7.0f), ImVec2(X, RowY + 11.0f), Fill);
                const char* Name = Events->Keys[KeyIndex].Name.c_str();
                DrawList->AddText(ImVec2(X + 11.0f, RowY + 5.0f), SeqText, Name);
                HitRight = X + 11.0f + ImGui::CalcTextSize(Name).x;
            }
            else if (const CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))
            {
                const SSequenceAudioClip& Clip = Audio->Clips[KeyIndex];
                const char* Name = Clip.Sound.IsValid() ? Clip.Sound->GetName().c_str() : "<no sound>";
                const float Width = Math::Max(ImGui::CalcTextSize(Name).x + 24.0f, 60.0f);
                DrawList->AddRectFilled(ImVec2(X, RowY + 3.0f), ImVec2(X + Width, RowMax.y - 3.0f), bKeySelected ? Fill : TintFaint, 3.0f);
                DrawList->AddRect(ImVec2(X, RowY + 3.0f), ImVec2(X + Width, RowMax.y - 3.0f), TintColor, 3.0f);
                DrawList->AddText(ImVec2(X + 5.0f, RowY + 5.0f), SeqText, LE_ICON_MUSIC);
                DrawList->AddText(ImVec2(X + 20.0f, RowY + 5.0f), SeqText, Name);
                HitRight = X + Width;
            }
            else
            {
                const float R = bKeySelected ? 6.5f : 5.0f;
                DrawList->AddQuadFilled(ImVec2(X, MidY - R), ImVec2(X + R, MidY), ImVec2(X, MidY + R), ImVec2(X - R, MidY), Fill);
                DrawList->AddQuad(ImVec2(X, MidY - R), ImVec2(X + R, MidY), ImVec2(X, MidY + R), ImVec2(X - R, MidY), SeqKeyOutline, 1.5f);
            }

            const bool bKeyHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(ImVec2(X - 7.0f, RowY), ImVec2(HitRight, RowMax.y));
            if (bKeyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bDraggingTrackKey)
            {
                SelectedTrack = TrackIndex;
                SelectedTrackKey = KeyIndex;
                bDraggingTrackKey = true;
                bPlaying = false;
            }
            if (bKeyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedTrack = TrackIndex;
                SelectedTrackKey = KeyIndex;
                ImGui::OpenPopup("##TrackKeyContext");
            }
        }

        DrawList->PopClipRect();
        (void)World;
    }

    void FSequencerEditMode::DrawTrackPopups(CWorld* World)
    {
        CSequenceTrack* Track = SelectedTrack >= 0 && SelectedTrack < (int32)Sequence->Tracks.size() ? Sequence->Tracks[SelectedTrack].Get() : nullptr;

        if (ImGui::BeginPopup("##TrackContext"))
        {
            if (Track != nullptr)
            {
                if (ImGui::MenuItem(Track->bEnabled ? LE_ICON_EYE_OFF " Disable" : LE_ICON_EYE " Enable"))
                {
                    Track->bEnabled = !Track->bEnabled;
                    Sequence->GetPackage()->MarkDirty();
                    EvaluateAt(World, PlayTime, true);
                }

                if (SequencerTracks::Curve(Track) != nullptr && ImGui::MenuItem(LE_ICON_KEY_PLUS " Key At Playhead"))
                {
                    KeyTrackAtPlayhead(World, Track);
                }

                if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track); Events != nullptr && ImGui::MenuItem(LE_ICON_FLAG " Add Event At Playhead"))
                {
                    SSequenceEventKey Key;
                    Key.Time = Sequence->SnapToFrame(PlayTime);
                    Key.Name = FName("Event");
                    Events->Keys.push_back(Key);
                    SelectedTrackKey = SequencerTracks::SortKeys(Track, (int32)Events->Keys.size() - 1);
                    Sequence->GetPackage()->MarkDirty();
                }

                if (CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track); Audio != nullptr && ImGui::MenuItem(LE_ICON_MUSIC " Add Clip At Playhead"))
                {
                    SSequenceAudioClip& Clip = Audio->Clips.emplace_back();
                    Clip.StartTime = Sequence->SnapToFrame(PlayTime);
                    SelectedTrackKey = (int32)Audio->Clips.size() - 1;
                    Sequence->GetPackage()->MarkDirty();
                }

                ImGui::Separator();
                if (ImGui::MenuItem(LE_ICON_DELETE " Delete Track"))
                {
                    PendingRemoveTrack = SelectedTrack;
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("##TrackKeyContext"))
        {
            SKeyedCurve* Keys = Track != nullptr ? SequencerTracks::Curve(Track) : nullptr;
            if (Keys != nullptr && SelectedTrackKey >= 0 && SelectedTrackKey < Keys->NumKeys() && ImGui::BeginMenu(LE_ICON_VECTOR_CURVE " Interpolation"))
            {
                struct FInterpChoice { const char* Label; ECurveInterpMode Mode; };
                const FInterpChoice Choices[] =
                {
                    { LE_ICON_STAIRS " Constant",        ECurveInterpMode::Constant },
                    { LE_ICON_VECTOR_LINE " Linear",     ECurveInterpMode::Linear   },
                    { LE_ICON_CHART_BELL_CURVE " Cubic", ECurveInterpMode::Cubic    },
                };
                for (const FInterpChoice& Choice : Choices)
                {
                    if (ImGui::MenuItem(Choice.Label, nullptr, Keys->Keys[SelectedTrackKey].InterpMode == Choice.Mode))
                    {
                        Keys->Keys[SelectedTrackKey].InterpMode = Choice.Mode;
                        Keys->ComputeAutoTangents();
                        Sequence->GetPackage()->MarkDirty();
                        EvaluateAt(World, PlayTime, true);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(LE_ICON_DELETE " Delete Key") && Track != nullptr)
            {
                SequencerTracks::RemoveKey(Track, SelectedTrackKey);
                SelectedTrackKey = Constants::kIndexNone;
                Sequence->GetPackage()->MarkDirty();
                EvaluateAt(World, PlayTime, true);
            }
            ImGui::EndPopup();
        }

        if (Track != nullptr && SelectedTrackKey != Constants::kIndexNone && !bDraggingTrackKey
            && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput
            && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            SequencerTracks::RemoveKey(Track, SelectedTrackKey);
            SelectedTrackKey = Constants::kIndexNone;
            Sequence->GetPackage()->MarkDirty();
            EvaluateAt(World, PlayTime, true);
        }
    }

    void FSequencerEditMode::DrawDetails(CWorld* World)
    {
        CSequenceTrack* Track = SelectedTrack >= 0 && SelectedTrack < (int32)Sequence->Tracks.size() ? Sequence->Tracks[SelectedTrack].Get() : nullptr;
        if (Track == nullptr)
        {
            Details.reset();
            DetailsTarget = nullptr;
            return;
        }

        ImGui::Spacing();
        ImGui::SeparatorText(SequencerTracks::RowLabel(Track).c_str());

        // The selected key first, since it is what an author just clicked and wants to change.
        bool bChanged = false;
        if (SKeyedCurve* Keys = SequencerTracks::Curve(Track); Keys != nullptr && SelectedTrackKey >= 0 && SelectedTrackKey < Keys->NumKeys())
        {
            SCurveKey& Key = Keys->Keys[SelectedTrackKey];
            ImGui::SetNextItemWidth(140.0f);
            bChanged |= ImGui::DragFloat("Value", &Key.Value, 0.01f);
            ImGui::SameLine();
            ImGui::TextDisabled("at %.2fs", Key.Time);
            if (bChanged)
            {
                Keys->ComputeAutoTangents();
            }
        }
        else if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track); Events != nullptr && SelectedTrackKey >= 0 && SelectedTrackKey < (int32)Events->Keys.size())
        {
            SSequenceEventKey& Key = Events->Keys[SelectedTrackKey];
            char NameBuffer[128];
            std::snprintf(NameBuffer, sizeof(NameBuffer), "%s", Key.Name.c_str());
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("Name", NameBuffer, sizeof(NameBuffer)))
            {
                Key.Name = FName(NameBuffer);
                bChanged = true;
            }
            ImGui::SameLine();
            char PayloadBuffer[256];
            std::snprintf(PayloadBuffer, sizeof(PayloadBuffer), "%s", Key.Payload.c_str());
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputText("Payload", PayloadBuffer, sizeof(PayloadBuffer)))
            {
                Key.Payload = PayloadBuffer;
                bChanged = true;
            }
        }
        else if (CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track); Audio != nullptr && SelectedTrackKey >= 0 && SelectedTrackKey < (int32)Audio->Clips.size())
        {
            SSequenceAudioClip& Clip = Audio->Clips[SelectedTrackKey];
            FGuid SoundGUID = Clip.Sound.IsValid() ? Clip.Sound->GetGUID() : FGuid();
            ImGui::SetNextItemWidth(220.0f);
            if (ImGuiX::AssetReferenceCombo("##ClipSound", CSoundBase::StaticClass(), SoundGUID, LE_ICON_MUSIC))
            {
                Clip.Sound = Cast<CSoundBase>(LoadObject<CObject>(SoundGUID));
                bChanged = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            bChanged |= ImGui::DragFloat("Volume", &Clip.Volume, 0.01f, 0.0f, 4.0f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            bChanged |= ImGui::DragFloat("Pitch", &Clip.Pitch, 0.01f, 0.01f, 4.0f);
        }

        if (bChanged)
        {
            Sequence->GetPackage()->MarkDirty();
            EvaluateAt(World, PlayTime, true);
        }

        if (DetailsTarget != Track || !Details)
        {
            Details = MakeUnique<FPropertyTable>(static_cast<CObject*>(Track));
            Details->SetShowSearchBar(false);
            Details->SetPostEditCallback([this, World](const FPropertyChangedEvent&)
            {
                if (Sequence.IsValid())
                {
                    Sequence->GetPackage()->MarkDirty();
                    EvaluateAt(World, PlayTime, true);
                }
            });
            DetailsTarget = Track;
        }
        Details->DrawTree();
    }

    void FSequencerEditMode::DrawAddTrackMenu(CWorld* World)
    {
        if (!ImGui::BeginPopup("##AddTrack"))
        {
            return;
        }

        struct FTrackChoice { const char* Label; CClass* Class; bool bNeedsBinding; const char* Tooltip; };
        const FTrackChoice Choices[] =
        {
            { LE_ICON_AXIS_ARROW " Transform",        CSequenceTrack_Transform::StaticClass(),    true,  "Keys the binding's location, rotation and scale." },
            { LE_ICON_TUNE " Property",               CSequenceTrack_Property::StaticClass(),     true,  "A curve on one number of one component, such as a camera's FOV or a light's intensity." },
            { LE_ICON_EYE " Look At",                 CSequenceTrack_LookAt::StaticClass(),       true,  "Aims the binding at another binding or a fixed point, and can pull focus onto it." },
            { LE_ICON_FLAG " Events",                 CSequenceTrack_Event::StaticClass(),        false, "Named moments gameplay reacts to." },
            { LE_ICON_MUSIC " Audio",                 CSequenceTrack_Audio::StaticClass(),        false, "Music and sounds started on cue." },
            { LE_ICON_CIRCLE_HALF_FULL " Fade",       CSequenceTrack_Fade::StaticClass(),         false, "Fades the whole picture to a color." },
            { LE_ICON_TIMER " Time Dilation",         CSequenceTrack_TimeDilation::StaticClass(), false, "Slow motion while the edit keeps real time." },
            { LE_ICON_VIBRATE " Camera Shake",        CSequenceTrack_CameraShake::StaticClass(),  false, "A handheld drift over whichever camera is live." },
        };

        for (const FTrackChoice& Choice : Choices)
        {
            const bool bDisabled = Choice.bNeedsBinding && SelectedBinding == Constants::kIndexNone;
            if (ImGui::MenuItem(Choice.Label, nullptr, false, !bDisabled))
            {
                AddTrackOfClass(World, Choice.Class, Choice.bNeedsBinding ? SelectedBinding : Constants::kIndexNone);
            }
            ImGuiX::TextTooltip("{}", bDisabled ? "Select a binding first." : Choice.Tooltip);
        }
        ImGui::EndPopup();
    }

    CSequenceTrack* FSequencerEditMode::AddTrackOfClass(CWorld* World, CClass* Class, int32 BindingIndex)
    {
        if (!Sequence.IsValid())
        {
            return nullptr;
        }

        if (Class == CSequenceTrack_Transform::StaticClass())
        {
            CSequenceTrack_Transform* Transform = FindOrCreateTransformTrack(BindingIndex);
            Sequence->GetPackage()->MarkDirty();
            return Transform;
        }

        // Outered to the sequence's package so the track is an export and survives a save.
        CSequenceTrack* Track = NewObject<CSequenceTrack>(Class, Sequence->GetPackage());
        Track->BindingIndex = BindingIndex;

        const ECS::FEntity Entity = BindingIndex >= 0 && BindingIndex < (int32)Instance.BoundEntities.size() ? Instance.BoundEntities[BindingIndex] : ECS::NullEntity;
        if (CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))
        {
            // A camera's lens is what a property track drives most, so a camera binding starts on its FOV.
            if (Entity != ECS::NullEntity && World != nullptr && World->IsValidEntity(Entity) && World->TryGetComponent<SCameraComponent>(Entity) != nullptr)
            {
                Property->ComponentType = FName("SCameraComponent");
                Property->PropertyPath = "FOV";
            }
        }

        Sequence->Tracks.push_back(Track);
        SelectedTrack = (int32)Sequence->Tracks.size() - 1;
        SelectedTrackKey = Constants::kIndexNone;
        Sequence->GetPackage()->MarkDirty();

        if (SequencerTracks::Curve(Track) != nullptr)
        {
            KeyTrackAtPlayhead(World, Track);
        }
        return Track;
    }

    void FSequencerEditMode::KeyTrackAtPlayhead(CWorld* World, CSequenceTrack* Track)
    {
        SKeyedCurve* Keys = SequencerTracks::Curve(Track);
        if (Keys == nullptr)
        {
            return;
        }

        const float Time = Sequence->SnapToFrame(PlayTime);
        float Value = Keys->NumKeys() > 0 ? Keys->Evaluate(Time) : SequencerTracks::RestingValue(Track);

        // A property keys what the entity holds now, so posing it in the details panel and keying is the workflow.
        if (const CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))
        {
            const int32 Binding = Property->BindingIndex;
            if (Binding >= 0 && Binding < (int32)Instance.BoundEntities.size())
            {
                Property->SampleValue(World, Instance.BoundEntities[Binding], Value);
            }
        }

        const int32 Index = Keys->UpdateOrAddKey(Time, Value);
        Keys->Keys[Index].InterpMode = ECurveInterpMode::Cubic;
        Keys->ComputeAutoTangents();
        SelectedTrack = Algo::IndexOfIf(Sequence->Tracks, [Track](const TStrongObjectPtr<CSequenceTrack>& Other) { return Other.Get() == Track; });
        SelectedTrackKey = Index;
        Sequence->GetPackage()->MarkDirty();
        EvaluateAt(World, PlayTime, true);
    }

    void FSequencerEditMode::RemoveTrack(CWorld* World, int32 TrackIndex)
    {
        if (!Sequence.IsValid() || TrackIndex < 0 || TrackIndex >= (int32)Sequence->Tracks.size())
        {
            return;
        }

        // Released first, so a shake, sound or time scale the track left running is undone before it goes.
        ReleaseBindings(World);
        Sequence->Tracks.erase(Sequence->Tracks.begin() + TrackIndex);
        Sequence->GetPackage()->MarkDirty();
        SelectedTrack = Constants::kIndexNone;
        SelectedTrackKey = Constants::kIndexNone;
        BindToWorld(World);
        EvaluateAt(World, PlayTime, true);
    }

    bool FSequencerEditMode::GetEditorView(CWorld* World, FVector3& OutLocation, FVector3& OutRotation) const
    {
        if (World == nullptr)
        {
            return false;
        }

        auto View = World->View<FEditorComponent, STransformComponent>();
        for (ECS::FEntity Entity : View)
        {
            const STransformComponent& Transform = View.Get<STransformComponent>(Entity);
            OutLocation = Transform.GetWorldLocation();
            OutRotation = Math::Degrees(Math::YawFirstEulerAngles(Transform.GetWorldRotation()));
            return true;
        }
        return false;
    }

    void FSequencerEditMode::KeyFromView(CWorld* World, int32 BindingIndex)
    {
        FVector3 Location;
        FVector3 Rotation;
        if (!Sequence.IsValid() || BindingIndex < 0 || BindingIndex >= (int32)Sequence->Bindings.size() || !GetEditorView(World, Location, Rotation))
        {
            return;
        }

        CSequenceTrack_Transform* Track = FindOrCreateTransformTrack(BindingIndex);
        const float Time = Sequence->SnapToFrame(PlayTime);
        const auto KeyChannel = [Time](SSequenceVectorCurve& Channel, const FVector3& Value)
        {
            Channel.bEnabled = true;
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                SKeyedCurve& Curve = (Axis == 0 ? Channel.X : (Axis == 1 ? Channel.Y : Channel.Z)).Curve;
                const int32 Index = Curve.UpdateOrAddKey(Time, Value[Axis]);
                Curve.Keys[Index].InterpMode = ECurveInterpMode::Cubic;
            }
        };
        KeyChannel(Track->Location, Location);
        KeyChannel(Track->Rotation, Rotation);
        Track->Rotation.UnwindAngles();
        for (SSequenceVectorCurve* Channel : { &Track->Location, &Track->Rotation })
        {
            Channel->X.Curve.ComputeAutoTangents();
            Channel->Y.Curve.ComputeAutoTangents();
            Channel->Z.Curve.ComputeAutoTangents();
        }

        // Scale stays unkeyed so a camera never inherits a stray scale from the editor entity.
        Track->Scale.bEnabled = false;
        Sequence->GetPackage()->MarkDirty();
        EvaluateAt(World, PlayTime, true);
    }

    void FSequencerEditMode::AddCameraFromView(CWorld* World)
    {
        if (!Sequence.IsValid())
        {
            return;
        }

        int32 Number = 1;
        FName Name;
        do
        {
            Name = FName(Lumina::Format("Camera{}", Number++).c_str());
        }
        while (Algo::IndexOf(Sequence->Bindings, Name, &SSequenceBinding::Name) != Constants::kIndexNone);

        SSequenceBinding& Binding = Sequence->Bindings.emplace_back();
        Binding.Name = Name;
        Binding.Kind = ESequenceBindingKind::Camera;
        const int32 BindingIndex = (int32)Sequence->Bindings.size() - 1;
        Sequence->GetPackage()->MarkDirty();

        BindToWorld(World);
        SelectedBinding = BindingIndex;
        KeyFromView(World, BindingIndex);
        AddCameraCutAtPlayhead(World);
        EvaluateAt(World, PlayTime, true);
    }

    void FSequencerEditMode::SetPreviewCameras(CWorld* World, bool bPreview)
    {
        bPreviewCameras = bPreview;
        Instance.bDriveCamera = bPreview;

        if (World == nullptr)
        {
            return;
        }

        if (bPreview)
        {
            EvaluateAt(World, PlayTime, true);
            return;
        }

        // Hands the viewport back to the editor camera, which the cuts had taken.
        if (Instance.PreviousCamera != ECS::NullEntity && World->IsValidEntity(Instance.PreviousCamera))
        {
            World->SetActiveCamera(Instance.PreviousCamera);
        }
    }
}
