#include "SequencerEditMode.h"

#include "Assets/AssetTypes/Audio/SoundBase.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "World/ECS/Registry.h"
#include "World/World.h"
#include "World/WorldTypes.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace Lumina::SequencerEditModeInternal
{
    constexpr float TrackHeight = 26.0f;
    constexpr float LabelWidth  = 190.0f;
    constexpr float RulerHeight = 24.0f;

    // Half a frame at 240fps, tight enough to keep adjacent keys apart and loose enough for float error.
    constexpr float KeyMatchEpsilon = 0.002f;

    constexpr ImU32 ColPanel     = IM_COL32(24, 26, 31, 255);
    constexpr ImU32 ColRuler     = IM_COL32(19, 21, 25, 255);
    constexpr ImU32 ColRow       = IM_COL32(33, 36, 43, 255);
    constexpr ImU32 ColRowAlt    = IM_COL32(29, 32, 38, 255);
    constexpr ImU32 ColRowSelected    = IM_COL32(48, 70, 100, 255);
    constexpr ImU32 ColRowHovered  = IM_COL32(41, 46, 56, 255);
    constexpr ImU32 ColCutRow    = IM_COL32(38, 33, 26, 255);
    constexpr ImU32 ColGridMinor   = IM_COL32(255, 255, 255, 12);
    constexpr ImU32 ColGridMajor   = IM_COL32(255, 255, 255, 32);
    constexpr ImU32 ColText        = IM_COL32(226, 230, 238, 255);
    constexpr ImU32 ColTextDim     = IM_COL32(150, 157, 170, 255);
    constexpr ImU32 ColTextOff     = IM_COL32(110, 114, 124, 255);
    constexpr ImU32 ColKey         = IM_COL32(255, 205, 90, 255);
    constexpr ImU32 ColKeySelected = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 ColKeyOutline  = IM_COL32(20, 18, 12, 220);
    constexpr ImU32 ColPlayhead    = IM_COL32(120, 255, 150, 235);

    struct FTimelineRow
    {
        int32 Binding = Constants::kIndexNone;
        int32 Track = Constants::kIndexNone;
    };

    struct FInterpChoice
    {
        const char*         Label;
        ECurveInterpMode    Mode;
    };

    constexpr FInterpChoice InterpChoices[] =
    {
        { LE_ICON_STAIRS " Constant",        ECurveInterpMode::Constant },
        { LE_ICON_VECTOR_LINE " Linear",     ECurveInterpMode::Linear   },
        { LE_ICON_CHART_BELL_CURVE " Cubic", ECurveInterpMode::Cubic    },
    };

    bool IsEditorWorld(const CWorld* World)
    {
        return World != nullptr && World->GetWorldType() == EWorldType::Editor;
    }

    ImU32 PickRowColor(bool bSelected, bool bHovered, int32 RowIndex)
    {
        if (bSelected)
        {
            return ColRowSelected;
        }
        if (bHovered)
        {
            return ColRowHovered;
        }
        return (RowIndex & 1) ? ColRowAlt : ColRow;
    }

    // Steps of 1, 2 and 5 per decade, so labels never land on awkward fractions.
    float ChooseRulerStep(float VisibleSeconds, float TrackWidth)
    {
        const float Rough = (VisibleSeconds / Math::Max(TrackWidth, 1.0f)) * 90.0f;

        float Magnitude = 0.001f;
        while (Magnitude * 10.0f < Rough)
        {
            Magnitude *= 10.0f;
        }

        const float Normalized = Rough / Magnitude;
        const float Step = Normalized <= 1.0f ? 1.0f : (Normalized <= 2.0f ? 2.0f : (Normalized <= 5.0f ? 5.0f : 10.0f));
        return Step * Magnitude;
    }

    void DrawKeyShape(ImDrawList* DrawList, float X, float MidY, float Radius, ECurveInterpMode Mode, ImU32 Fill)
    {
        switch (Mode)
        {
        case ECurveInterpMode::Constant:
            {
                const ImVec2 Min(X - Radius * 0.8f, MidY - Radius * 0.8f);
                const ImVec2 Max(X + Radius * 0.8f, MidY + Radius * 0.8f);
                DrawList->AddRectFilled(Min, Max, Fill, 1.0f);
                DrawList->AddRect(Min, Max, ColKeyOutline, 1.0f, 1.5f);
                break;
            }
        case ECurveInterpMode::Cubic:
        case ECurveInterpMode::CubicUser:
            DrawList->AddCircleFilled(ImVec2(X, MidY), Radius * 0.9f, Fill, 12);
            DrawList->AddCircle(ImVec2(X, MidY), Radius * 0.9f, ColKeyOutline, 12, 1.5f);
            break;
        default:
            DrawList->AddQuadFilled(ImVec2(X, MidY - Radius), ImVec2(X + Radius, MidY), ImVec2(X, MidY + Radius), ImVec2(X - Radius, MidY), Fill);
            DrawList->AddQuad(ImVec2(X, MidY - Radius), ImVec2(X + Radius, MidY), ImVec2(X, MidY + Radius), ImVec2(X - Radius, MidY), ColKeyOutline, 1.5f);
            break;
        }
    }

    // An asset-backed curve is shared, so editing it here would edit every other user of it.
    template<typename TrackType, typename FunctionType>
    void ForEachInlineCurve(TrackType* Track, FunctionType&& Function)
    {
        for (auto* Vector : { &Track->Location, &Track->Rotation, &Track->Scale })
        {
            for (auto* Channel : { &Vector->X, &Vector->Y, &Vector->Z })
            {
                if (!Channel->bUseAsset)
                {
                    Function(Channel->Curve);
                }
            }
        }
    }

    bool HasKeyNear(const SKeyedCurve& Curve, float Time)
    {
        return Algo::AnyOf(Curve.Keys, [Time](const SCurveKey& Key) { return Math::Abs(Key.Time - Time) <= KeyMatchEpsilon; });
    }

    void KeyVector(SSequenceVectorCurve& Vector, float Time, const FVector3& Value, bool bCubic)
    {
        SCurve* Axes[3] = { &Vector.X, &Vector.Y, &Vector.Z };
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            SKeyedCurve& Curve = Axes[Axis]->Curve;
            const int32 Index = Curve.UpdateOrAddKey(Time, Value[Axis]);
            if (bCubic)
            {
                Curve.Keys[Index].InterpMode = ECurveInterpMode::Cubic;
            }
        }
    }

    void RefreshTangents(CSequenceTrack_Transform* Track)
    {
        ForEachInlineCurve(Track, [](SKeyedCurve& Curve) { Curve.ComputeAutoTangents(); });
    }

    bool HasTransformKeyAt(const CSequenceTrack_Transform* Track, float Time)
    {
        bool bFound = false;
        ForEachInlineCurve(Track, [&](const SKeyedCurve& Curve) { bFound |= HasKeyNear(Curve, Time); });
        return bFound;
    }

    void MoveTransformKeys(CSequenceTrack_Transform* Track, float FromTime, float ToTime)
    {
        ForEachInlineCurve(Track, [&](SKeyedCurve& Curve)
        {
            const auto KeyItr = Algo::FindIf(Curve.Keys, [&](const SCurveKey& Key) { return Math::Abs(Key.Time - FromTime) <= KeyMatchEpsilon; });
            if (KeyItr != Curve.Keys.end())
            {
                KeyItr->Time = ToTime;
            }
            Curve.SortKeys();
            Curve.ComputeAutoTangents();
        });
    }

    void DeleteTransformKeys(CSequenceTrack_Transform* Track, float AtTime)
    {
        ForEachInlineCurve(Track, [&](SKeyedCurve& Curve)
        {
            for (int32 Index = (int32)Curve.Keys.size() - 1; Index >= 0; --Index)
            {
                if (Math::Abs(Curve.Keys[Index].Time - AtTime) <= KeyMatchEpsilon)
                {
                    Curve.RemoveKey(Index);
                }
            }
            Curve.ComputeAutoTangents();
        });
    }

    // Tangents are zero on a linear key, and a zero-slope Hermite is not the arc the mode is chosen for.
    void SetTransformKeyInterp(CSequenceTrack_Transform* Track, float AtTime, ECurveInterpMode Mode)
    {
        ForEachInlineCurve(Track, [&](SKeyedCurve& Curve)
        {
            const auto KeyItr = Algo::FindIf(Curve.Keys, [&](const SCurveKey& Key) { return Math::Abs(Key.Time - AtTime) <= KeyMatchEpsilon; });
            if (KeyItr != Curve.Keys.end())
            {
                KeyItr->InterpMode = Mode;
            }
            Curve.ComputeAutoTangents();
        });
    }

    // Read from Location.X, since the nine channels are keyed and retimed as a unit.
    ECurveInterpMode GetTransformKeyInterp(const CSequenceTrack_Transform* Track, float AtTime)
    {
        if (Track == nullptr)
        {
            return ECurveInterpMode::Linear;
        }

        const TVector<SCurveKey>& Keys = Track->Location.X.Resolve().Keys;
        const auto KeyItr = Algo::FindIf(Keys, [&](const SCurveKey& Key) { return Math::Abs(Key.Time - AtTime) <= KeyMatchEpsilon; });
        return KeyItr != Keys.end() ? KeyItr->InterpMode : ECurveInterpMode::Linear;
    }

    const SKeyedCurve* FindCurve(const CSequenceTrack* Track)
    {
        if (const CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))        { return &Property->Curve.Curve; }
        if (const CSequenceTrack_Fade* Fade = Cast<CSequenceTrack_Fade>(Track))                    { return &Fade->Amount.Curve; }
        if (const CSequenceTrack_TimeDilation* Dilation = Cast<CSequenceTrack_TimeDilation>(Track)) { return &Dilation->Scale.Curve; }
        if (const CSequenceTrack_LookAt* LookAt = Cast<CSequenceTrack_LookAt>(Track))              { return &LookAt->Weight.Curve; }
        if (const CSequenceTrack_CameraShake* Shake = Cast<CSequenceTrack_CameraShake>(Track))     { return &Shake->Intensity.Curve; }
        return nullptr;
    }

    SKeyedCurve* FindCurve(CSequenceTrack* Track)
    {
        return const_cast<SKeyedCurve*>(FindCurve(static_cast<const CSequenceTrack*>(Track)));
    }

    float RestingValue(const CSequenceTrack* Track)
    {
        return Track->IsA<CSequenceTrack_Fade>() ? 0.0f : 1.0f;
    }

    int32 KeyCount(const CSequenceTrack* Track)
    {
        if (const SKeyedCurve* Curve = FindCurve(Track))                            { return Curve->NumKeys(); }
        if (const CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track)) { return (int32)Events->Keys.size(); }
        if (const CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))  { return (int32)Audio->Clips.size(); }
        return 0;
    }

    float KeyTime(const CSequenceTrack* Track, int32 Index)
    {
        if (const SKeyedCurve* Curve = FindCurve(Track))                            { return Curve->Keys[Index].Time; }
        if (const CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track)) { return Events->Keys[Index].Time; }
        if (const CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))  { return Audio->Clips[Index].StartTime; }
        return 0.0f;
    }

    int32 RetimeKey(CSequenceTrack* Track, int32 Index, float NewTime)
    {
        if (SKeyedCurve* Curve = FindCurve(Track))
        {
            SCurveKey Moved = Curve->Keys[Index];
            Moved.Time = NewTime;
            Curve->Keys.erase(Curve->Keys.begin() + Index);
            const auto Where = std::upper_bound(Curve->Keys.begin(), Curve->Keys.end(), NewTime, [](float Value, const SCurveKey& Other) { return Value < Other.Time; });
            const int32 Landed = (int32)(Where - Curve->Keys.begin());
            Curve->Keys.insert(Where, Moved);
            Curve->ComputeAutoTangents();
            return Landed;
        }
        if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track))
        {
            SSequenceEventKey Moved = Events->Keys[Index];
            Moved.Time = NewTime;
            Events->Keys.erase(Events->Keys.begin() + Index);
            const auto Where = std::upper_bound(Events->Keys.begin(), Events->Keys.end(), NewTime, [](float Value, const SSequenceEventKey& Other) { return Value < Other.Time; });
            const int32 Landed = (int32)(Where - Events->Keys.begin());
            Events->Keys.insert(Where, Moved);
            return Landed;
        }
        if (CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))
        {
            Audio->Clips[Index].StartTime = NewTime;
        }
        return Index;
    }

    void RemoveKey(CSequenceTrack* Track, int32 Index)
    {
        if (Index < 0 || Index >= KeyCount(Track))
        {
            return;
        }

        if (SKeyedCurve* Curve = FindCurve(Track))
        {
            Curve->RemoveKey(Index);
            Curve->ComputeAutoTangents();
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

    FStringView RowLabel(const CSequenceTrack* Track)
    {
        if (const CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))
        {
            return Property->PropertyPath.empty() ? FStringView("Property (unset)") : FStringView(Property->PropertyPath.c_str(), Property->PropertyPath.size());
        }
        return Track->GetTrackDisplayName();
    }

    bool IsGenericRowTrack(const CSequenceTrack* Track)
    {
        return Track != nullptr && !Track->IsA<CSequenceTrack_Transform>() && !Track->IsA<CSequenceTrack_CameraCut>();
    }

    void GatherTimelineRows(const CSequence* Sequence, TVector<FTimelineRow>& OutRows)
    {
        const int32 BindingCount = (int32)Sequence->Bindings.size();
        const int32 TrackCount = (int32)Sequence->Tracks.size();

        OutRows.clear();
        OutRows.reserve(BindingCount + TrackCount);

        for (int32 BindingIndex = 0; BindingIndex < BindingCount; ++BindingIndex)
        {
            OutRows.push_back({ BindingIndex, Constants::kIndexNone });
            for (int32 TrackIndex = 0; TrackIndex < TrackCount; ++TrackIndex)
            {
                const CSequenceTrack* Track = Sequence->Tracks[TrackIndex].Get();
                if (IsGenericRowTrack(Track) && Track->BindingIndex == BindingIndex)
                {
                    OutRows.push_back({ BindingIndex, TrackIndex });
                }
            }
        }

        for (int32 TrackIndex = 0; TrackIndex < TrackCount; ++TrackIndex)
        {
            const CSequenceTrack* Track = Sequence->Tracks[TrackIndex].Get();
            if (IsGenericRowTrack(Track) && (Track->BindingIndex < 0 || Track->BindingIndex >= BindingCount))
            {
                OutRows.push_back({ Constants::kIndexNone, TrackIndex });
            }
        }
    }
}

namespace Lumina
{
    FSequencerEditMode::FSequencerEditMode() = default;
    FSequencerEditMode::~FSequencerEditMode() = default;

    void FSequencerEditMode::OnEnter(CWorld* World)
    {
        PlayTime = 0.0f;
        bPlaying = false;
        BindToWorld(World);
    }

    void FSequencerEditMode::OnExit(CWorld* World)
    {
        bPlaying = false;
        bScrubbing = false;
        bDraggingKey = false;
        bDraggingTrackKey = false;
        DraggingCut = Constants::kIndexNone;
        ReleaseBindings(World);
    }

    void FSequencerEditMode::BindToWorld(CWorld* World)
    {
        ReleaseBindings(World);

        // Authoring keys whatever the bound entities do, so a play session's gameplay must never reach the asset.
        if (!Sequence.IsValid() || !SequencerEditModeInternal::IsEditorWorld(World))
        {
            return;
        }

        Instance.bDriveCamera = bPreviewCameras;
        Instance.Bind(Sequence.Get(), World);
        bBound = Instance.bBound;
        RefreshAutoKeyWatch(World);
    }

    void FSequencerEditMode::ReleaseBindings(CWorld* World)
    {
        Instance.Release(World, true);
        Instance.FiredEvents.clear();
        AutoKeyWatch.clear();
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

        RefreshAutoKeyWatch(World);
    }

    void FSequencerEditMode::MarkSequenceDirty()
    {
        if (Sequence.IsValid() && Sequence->GetPackage() != nullptr)
        {
            Sequence->GetPackage()->MarkDirty();
        }
    }

    void FSequencerEditMode::CommitEdit(CWorld* World)
    {
        MarkSequenceDirty();
        EvaluateAt(World, PlayTime, true);
    }

    void FSequencerEditMode::ResetSelection()
    {
        SelectedBinding = Constants::kIndexNone;
        SelectedKeyBinding = Constants::kIndexNone;
        SelectedTrack = Constants::kIndexNone;
        SelectedTrackKey = Constants::kIndexNone;
        SelectedCut = Constants::kIndexNone;
        DraggingCut = Constants::kIndexNone;
        PendingRemoveBinding = Constants::kIndexNone;
        PendingRemoveTrack = Constants::kIndexNone;
        bDraggingKey = false;
        bDraggingTrackKey = false;
        bScrubbing = false;

        // Compared by address, so a freed track's slot could otherwise be reused under a table that still points at it.
        Details.reset();
        DetailsTarget = nullptr;
        bDetailsEdited = false;
    }

    void FSequencerEditMode::RefreshAutoKeyWatch(CWorld* World)
    {
        AutoKeyWatch.assign(Instance.BoundEntities.size(), FAutoKeyPose());

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
                FAutoKeyPose& Pose = AutoKeyWatch[i];
                Pose.Entity = Entity;
                Pose.Location = Transform->GetLocation();
                Pose.Rotation = Math::Degrees(Math::YawFirstEulerAngles(Transform->GetRotation()));
                Pose.Scale = Transform->GetScale();
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

            const FAutoKeyPose& Watched = AutoKeyWatch[i];
            const FVector3 Rotation = Math::Degrees(Math::YawFirstEulerAngles(Transform->GetRotation()));

            const bool bMoved = Math::LengthSquared(Transform->GetLocation() - Watched.Location) > PositionEpsilonSq
                             || Math::LengthSquared(Rotation - Watched.Rotation) > AngleEpsilonSq
                             || Math::LengthSquared(Transform->GetScale() - Watched.Scale) > PositionEpsilonSq;

            if (bMoved)
            {
                KeyTransform(World, i);
            }
        }

        RefreshAutoKeyWatch(World);
    }

    void FSequencerEditMode::Tick(CWorld* World, const SCameraComponent&, bool, ImVec2, ImVec2)
    {
        if (!Sequence.IsValid() || !bBound)
        {
            return;
        }

        // During playback every driven transform is the sequence's own output, so there is nothing to key.
        if (!bPlaying)
        {
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

    void FSequencerEditMode::DrawToolbar(CWorld* World, float)
    {
        if (!SequencerEditModeInternal::IsEditorWorld(World))
        {
            ImGui::TextDisabled(LE_ICON_FILMSTRIP " Sequences are authored in the editor world. Stop playing to edit.");
            return;
        }

        FGuid SequenceGUID = Sequence.IsValid() && Sequence->GetPackage() != nullptr ? Sequence->GetGUID() : FGuid();

        ImGui::SetNextItemWidth(240.0f);
        if (ImGuiX::AssetReferenceCombo("##Sequence", CSequence::StaticClass(), SequenceGUID, LE_ICON_FILMSTRIP))
        {
            ReleaseBindings(World);
            ResetSelection();
            Sequence = Cast<CSequence>(LoadObject<CObject>(SequenceGUID));
            PlayTime = 0.0f;
            bPlaying = false;
            TimelineZoom = 1.0f;
            TimelineScroll = 0.0f;
            BindToWorld(World);
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

            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
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

            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            {
                DeleteSelectedKey(World);
            }
        }

        DrawTransportBar(World);
        ImGui::Separator();
        DrawBindingList(World);
        ImGui::Separator();
        DrawTimeline(World);
        DrawDetails(World);

        if (PendingRemoveBinding != Constants::kIndexNone)
        {
            const int32 BindingIndex = PendingRemoveBinding;
            PendingRemoveBinding = Constants::kIndexNone;
            RemoveBinding(World, BindingIndex);
        }

        if (PendingRemoveTrack != Constants::kIndexNone)
        {
            const int32 TrackIndex = PendingRemoveTrack;
            PendingRemoveTrack = Constants::kIndexNone;
            RemoveTrack(World, TrackIndex);
        }

        // Peeked first, so an asset drag never highlights the panel as a target.
        const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
        if (Peek != nullptr && Peek->Kind == DragDrop::EPayloadKind::Entity
            && ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("##BindEntityDrop")))
        {
            CWorld* SourceWorld = nullptr;
            ECS::FEntity Dropped = ECS::NullEntity;
            if (DragDrop::AcceptEntity(&SourceWorld, &Dropped) && SourceWorld == World)
            {
                SelectedBinding = AddBinding(World, Dropped);
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::End();
    }

    void FSequencerEditMode::DrawTransportBar(CWorld* World)
    {
        const int32 FrameCount = Sequence->GetFrameCount();

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

        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        int32 FrameEntry = Sequence->TimeToFrame(PlayTime);
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

        ImGui::SameLine();
        ImGui::TextUnformatted("|");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        float DurationEntry = Sequence->Duration;
        if (ImGui::DragFloat("##Duration", &DurationEntry, 0.05f, 0.1f, 3600.0f, "Length %.2fs"))
        {
            Sequence->Duration = Math::Max(DurationEntry, 0.1f);
            MarkSequenceDirty();

            // Shortening can strand the playhead and the view past the new end.
            PlayTime = Math::Min(PlayTime, Sequence->Duration);
            TimelineScroll = Math::Clamp(TimelineScroll, 0.0f, Math::Max(Sequence->Duration - Sequence->Duration * TimelineZoom, 0.0f));
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
            MarkSequenceDirty();
        }
        ImGuiX::TextTooltip("Display and snapping rate. Evaluation stays continuous, so this never quantizes playback.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        float LetterboxEntry = Sequence->LetterboxAspect;
        if (ImGui::DragFloat("##Letterbox", &LetterboxEntry, 0.01f, 0.0f, 4.0f, LetterboxEntry > 0.0f ? "Bars %.2f" : "No bars"))
        {
            Sequence->LetterboxAspect = Math::Clamp(LetterboxEntry, 0.0f, 4.0f);
            CommitEdit(World);
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

    void FSequencerEditMode::DrawBindingList(CWorld* World)
    {
        const bool bNoBinding = SelectedBinding == Constants::kIndexNone;

        if (ImGui::Button(LE_ICON_PLUS " Bind Selected"))
        {
            SelectedBinding = AddBindingFromSelection(World);
        }
        ImGuiX::TextTooltip("Add the selected world entity as a possessed binding, or drag one from the outliner onto this panel.");

        ImGui::SameLine();
        ImGui::BeginDisabled(bNoBinding);
        if (ImGui::Button(LE_ICON_KEY " Key Transform"))
        {
            KeyTransform(World, SelectedBinding);
        }
        ImGuiX::TextTooltip("Capture the bound entity's current transform as a key at the playhead.");

        ImGui::SameLine();
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
        ImGui::BeginDisabled(bNoBinding);
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
        ImGui::TextDisabled("%d bindings, %d tracks", (int)Sequence->Bindings.size(), (int)Sequence->Tracks.size());
    }

    void FSequencerEditMode::DrawAddTrackMenu(CWorld* World)
    {
        if (!ImGui::BeginPopup("##AddTrack"))
        {
            return;
        }

        struct FTrackChoice
        {
            const char* Label;
            CClass*     Class;
            bool        bNeedsBinding;
            const char* Tooltip;
        };

        const FTrackChoice Choices[] =
        {
            { LE_ICON_AXIS_ARROW " Transform",  CSequenceTrack_Transform::StaticClass(),    true,  "Keys the binding's location, rotation and scale." },
            { LE_ICON_TUNE " Property",         CSequenceTrack_Property::StaticClass(),     true,  "A curve on one number of one component, such as a camera's FOV or a light's intensity." },
            { LE_ICON_EYE " Look At",           CSequenceTrack_LookAt::StaticClass(),       true,  "Aims the binding at another binding or a fixed point, and can pull focus onto it." },
            { LE_ICON_FLAG " Events",           CSequenceTrack_Event::StaticClass(),        false, "Named moments gameplay reacts to." },
            { LE_ICON_MUSIC " Audio",           CSequenceTrack_Audio::StaticClass(),        false, "Music and sounds started on cue." },
            { LE_ICON_CIRCLE_HALF_FULL " Fade", CSequenceTrack_Fade::StaticClass(),         false, "Fades the whole picture to a color." },
            { LE_ICON_TIMER " Time Dilation",   CSequenceTrack_TimeDilation::StaticClass(), false, "Slow motion while the edit keeps real time." },
            { LE_ICON_VIBRATE " Camera Shake",  CSequenceTrack_CameraShake::StaticClass(),  false, "A handheld drift over whichever camera is live." },
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

    float FSequencerEditMode::TimeToX(float Time) const
    {
        return ViewLeft + ((Time - ViewStart) / ViewSeconds) * ViewWidth;
    }

    float FSequencerEditMode::XToTime(float X) const
    {
        return ViewStart + ((X - ViewLeft) / Math::Max(ViewWidth, 1.0f)) * ViewSeconds;
    }

    void FSequencerEditMode::DrawTimeline(CWorld* World)
    {
        using namespace SequencerEditModeInternal;

        const float Duration = Math::Max(Sequence->Duration, 0.01f);

        ImGui::SetNextItemWidth(-1.0f);
        float ScrubTime = PlayTime;
        if (ImGui::SliderFloat("##Scrub", &ScrubTime, 0.0f, Duration, "%.2fs"))
        {
            bPlaying = false;
            EvaluateAt(World, Sequence->SnapToFrame(ScrubTime), true);
        }

        TVector<FTimelineRow> Rows;
        GatherTimelineRows(Sequence.Get(), Rows);

        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float PanelWidth = Math::Max(ImGui::GetContentRegionAvail().x, 128.0f);
        const float BodyTop = Origin.y + RulerHeight;
        const float TotalHeight = RulerHeight + (float)(Rows.size() + 1) * TrackHeight;

        ViewLeft = Origin.x + LabelWidth;
        ViewWidth = Math::Max(PanelWidth - LabelWidth, 64.0f);

        ImGui::InvisibleButton("##Timeline", ImVec2(PanelWidth, TotalHeight));
        const bool bTimelineHovered = ImGui::IsItemHovered();
        const ImGuiIO& IO = ImGui::GetIO();

        const float VisibleSeconds = Duration * TimelineZoom;
        if (bTimelineHovered && IO.MouseWheel != 0.0f)
        {
            if (IO.KeyCtrl)
            {
                const float Anchor01 = Math::Clamp((IO.MousePos.x - ViewLeft) / ViewWidth, 0.0f, 1.0f);
                const float AnchorTime = TimelineScroll + Anchor01 * VisibleSeconds;

                TimelineZoom = Math::Clamp(TimelineZoom * (IO.MouseWheel > 0.0f ? 0.85f : 1.18f), 0.02f, 1.0f);
                TimelineScroll = Math::Clamp(AnchorTime - Anchor01 * (Duration * TimelineZoom), 0.0f, Math::Max(Duration - Duration * TimelineZoom, 0.0f));
            }
            else
            {
                TimelineScroll = Math::Clamp(TimelineScroll - IO.MouseWheel * VisibleSeconds * 0.1f, 0.0f, Math::Max(Duration - VisibleSeconds, 0.0f));
            }
        }

        ViewSeconds = Math::Max(Duration * TimelineZoom, 0.01f);
        ViewStart = TimelineScroll;

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(Origin, ImVec2(Origin.x + PanelWidth, Origin.y + TotalHeight), ColPanel, 4.0f);

        DrawTimeRuler(DrawList, Origin);
        DrawCameraCutRow(World, DrawList, BodyTop, bTimelineHovered);

        for (int32 RowIndex = 0; RowIndex < (int32)Rows.size(); ++RowIndex)
        {
            const float RowY = BodyTop + (float)(RowIndex + 1) * TrackHeight;
            if (Rows[RowIndex].Track != Constants::kIndexNone)
            {
                DrawTrackRow(DrawList, Rows[RowIndex].Track, RowIndex, RowY, bTimelineHovered);
            }
            else
            {
                DrawBindingRow(DrawList, Rows[RowIndex].Binding, RowIndex, RowY, bTimelineHovered);
            }
        }

        const float PlayheadX = TimeToX(Math::Clamp(PlayTime, 0.0f, Duration));
        if (PlayheadX >= ViewLeft && PlayheadX <= ViewLeft + ViewWidth)
        {
            DrawList->AddLine(ImVec2(PlayheadX, Origin.y), ImVec2(PlayheadX, Origin.y + TotalHeight), ColPlayhead, 2.0f);
            DrawList->AddTriangleFilled(ImVec2(PlayheadX - 6.0f, Origin.y), ImVec2(PlayheadX + 6.0f, Origin.y), ImVec2(PlayheadX, Origin.y + 9.0f), ColPlayhead);
        }

        UpdateKeyDrags(World, Duration);
        DrawBindingPopups(World);
        DrawTrackPopups(World);

        // A press on the ruler scrubs until release, wherever the cursor wanders, and nothing else starts one.
        const bool bOnRuler = bTimelineHovered && ImGui::IsMouseHoveringRect(ImVec2(ViewLeft, Origin.y), ImVec2(ViewLeft + ViewWidth, BodyTop));
        if (bOnRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            bScrubbing = true;
        }
        if (bScrubbing && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            bScrubbing = false;
        }
        if (bScrubbing)
        {
            bPlaying = false;
            EvaluateAt(World, Sequence->SnapToFrame(Math::Clamp(XToTime(IO.MousePos.x), 0.0f, Duration)), true);
        }
    }

    void FSequencerEditMode::DrawTimeRuler(ImDrawList* DrawList, const ImVec2& Origin)
    {
        using namespace SequencerEditModeInternal;

        const ImVec2 RulerMax(ViewLeft + ViewWidth, Origin.y + RulerHeight);

        DrawList->AddRectFilled(Origin, RulerMax, ColRuler);
        DrawList->AddLine(ImVec2(Origin.x, RulerMax.y), RulerMax, ColGridMajor);
        DrawList->AddText(ImVec2(Origin.x + 10.0f, Origin.y + 4.0f), ColTextDim, "Tracks");

        DrawList->PushClipRect(ImVec2(ViewLeft, Origin.y), RulerMax, true);

        // Stepped by index rather than by accumulating, so a long sequence cannot stall on float error.
        const float Step = ChooseRulerStep(ViewSeconds, ViewWidth);
        const float SubStep = Step * 0.25f;
        const float ViewEnd = ViewStart + ViewSeconds;

        for (int64 Tick = (int64)Math::Floor(ViewStart / SubStep); (float)Tick * SubStep <= ViewEnd; ++Tick)
        {
            const float X = TimeToX((float)Tick * SubStep);
            DrawList->AddLine(ImVec2(X, RulerMax.y - 5.0f), ImVec2(X, RulerMax.y), ColGridMinor);
        }

        char Label[32];
        for (int64 Tick = (int64)Math::Floor(ViewStart / Step); (float)Tick * Step <= ViewEnd; ++Tick)
        {
            const float Time = (float)Tick * Step;
            const float X = TimeToX(Time);
            DrawList->AddLine(ImVec2(X, RulerMax.y - 10.0f), ImVec2(X, RulerMax.y), ColGridMajor);

            if (Step < 1.0f)
            {
                snprintf(Label, sizeof(Label), "f%d", Sequence->TimeToFrame(Time));
            }
            else
            {
                snprintf(Label, sizeof(Label), "%gs", Time);
            }

            DrawList->AddText(ImVec2(X + 3.0f, Origin.y + 4.0f), ColTextDim, Label);
        }

        DrawList->PopClipRect();
    }

    void FSequencerEditMode::DrawCameraCutRow(CWorld* World, ImDrawList* DrawList, float RowY, bool bTimelineHovered)
    {
        using namespace SequencerEditModeInternal;

        const float Left = ViewLeft - LabelWidth;
        const ImVec2 RowMax(ViewLeft + ViewWidth, RowY + TrackHeight);

        DrawList->AddRectFilled(ImVec2(Left, RowY), RowMax, ColCutRow);
        DrawList->AddLine(ImVec2(ViewLeft, RowY), ImVec2(ViewLeft, RowMax.y), ColGridMajor);
        DrawList->AddText(ImVec2(Left + 10.0f, RowY + 5.0f), IM_COL32(255, 205, 130, 235), LE_ICON_MOVIE_OPEN "  Camera Cuts");

        CSequenceTrack_CameraCut* Track = FindCameraCutTrack();
        if (Track == nullptr)
        {
            return;
        }

        const float Duration = Math::Max(Sequence->Duration, 0.01f);
        const ImVec2 MousePos = ImGui::GetMousePos();
        constexpr float EdgeGrab = 5.0f;

        int32 PendingRemoval = Constants::kIndexNone;

        DrawList->PushClipRect(ImVec2(ViewLeft, RowY), RowMax, true);

        for (int32 i = 0; i < (int32)Track->Cuts.size(); ++i)
        {
            const SSequenceCameraCut& Cut = Track->Cuts[i];

            const float StartX = TimeToX(Cut.StartTime);
            const float EndX = Math::Max(TimeToX(Cut.EndTime), StartX + 3.0f);
            const ImVec2 ClipMin(StartX, RowY + 2.0f);
            const ImVec2 ClipMax(EndX, RowY + TrackHeight - 4.0f);

            DrawList->AddRectFilled(ClipMin, ClipMax, i == SelectedCut ? IM_COL32(255, 190, 80, 235) : IM_COL32(190, 135, 55, 220), 2.0f);
            DrawList->AddRect(ClipMin, ClipMax, IM_COL32(20, 16, 10, 220), 2.0f);

            const bool bValidBinding = Cut.BindingIndex >= 0 && Cut.BindingIndex < (int32)Sequence->Bindings.size();
            DrawList->PushClipRect(ClipMin, ClipMax, true);
            DrawList->AddText(ImVec2(ClipMin.x + 4.0f, ClipMin.y + 1.0f), IM_COL32(25, 20, 12, 255), bValidBinding ? Sequence->Bindings[Cut.BindingIndex].Name.c_str() : "<unbound>");
            DrawList->PopClipRect();

            const bool bHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(ClipMin, ClipMax);

            if (DraggingCut == Constants::kIndexNone && bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedCut = i;
                DraggingCut = i;

                // The grab offset keeps the clip from jumping its start under the cursor on the first frame.
                if (MousePos.x - StartX <= EdgeGrab)
                {
                    DragEdge = -1;
                }
                else if (EndX - MousePos.x <= EdgeGrab)
                {
                    DragEdge = 1;
                }
                else
                {
                    DragEdge = 0;
                    DragGrabOffset = Math::Clamp(XToTime(MousePos.x), 0.0f, Duration) - Cut.StartTime;
                }
            }

            if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedCut = i;
                PendingRemoval = i;
            }
        }

        DrawList->PopClipRect();

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
                const float Time = Sequence->SnapToFrame(Math::Clamp(XToTime(MousePos.x), 0.0f, Duration));

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
                    Cut.StartTime = Sequence->SnapToFrame(Math::Clamp(Time - DragGrabOffset, 0.0f, Math::Max(Duration - Length, 0.0f)));
                    Cut.EndTime = Cut.StartTime + Length;
                }

                CommitEdit(World);
            }
        }

        if (PendingRemoval != Constants::kIndexNone)
        {
            Track->Cuts.erase(Track->Cuts.begin() + PendingRemoval);
            SelectedCut = Constants::kIndexNone;
            DraggingCut = Constants::kIndexNone;
            CommitEdit(World);
        }
    }

    void FSequencerEditMode::DrawBindingRow(ImDrawList* DrawList, int32 BindingIndex, int32 RowIndex, float RowY, bool bTimelineHovered)
    {
        using namespace SequencerEditModeInternal;

        const float Left = ViewLeft - LabelWidth;
        const ImVec2 RowMin(Left, RowY);
        const ImVec2 RowMax(ViewLeft + ViewWidth, RowY + TrackHeight);

        const bool bSelected = BindingIndex == SelectedBinding;
        const bool bHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(RowMin, RowMax);

        DrawList->AddRectFilled(RowMin, RowMax, PickRowColor(bSelected, bHovered, RowIndex));
        DrawList->AddLine(ImVec2(ViewLeft, RowY), ImVec2(ViewLeft, RowMax.y), ColGridMajor);

        DrawList->PushClipRect(RowMin, ImVec2(ViewLeft - 4.0f, RowMax.y), true);
        DrawList->AddText(ImVec2(Left + 10.0f, RowY + 5.0f), bSelected ? ColText : ColTextDim, Sequence->Bindings[BindingIndex].Name.c_str());
        DrawList->PopClipRect();

        // A click on the row clears the key selection, and a click that lands on a key below takes it back.
        if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            SelectedBinding = BindingIndex;
            SelectedKeyBinding = Constants::kIndexNone;
            SelectedTrack = Constants::kIndexNone;
            SelectedTrackKey = Constants::kIndexNone;
        }

        // The track area to the right belongs to keys, which have their own context menu.
        if (bHovered && ImGui::GetMousePos().x < ViewLeft && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            SelectedBinding = BindingIndex;
            ImGui::OpenPopup("##BindingContext");
        }

        const CSequenceTrack_Transform* Transform = FindTransformTrack(BindingIndex);
        if (Transform == nullptr)
        {
            return;
        }

        const float MidY = RowY + TrackHeight * 0.5f;
        DrawList->PushClipRect(ImVec2(ViewLeft, RowY), RowMax, true);

        for (const SCurveKey& RowKey : Transform->Location.X.Resolve().Keys)
        {
            const float X = TimeToX(RowKey.Time);
            const bool bKeySelected = SelectedKeyBinding == BindingIndex && Math::Abs(RowKey.Time - SelectedKeyTime) <= KeyMatchEpsilon;
            const float Radius = bKeySelected ? 7.5f : 6.0f;

            DrawKeyShape(DrawList, X, MidY, Radius, RowKey.InterpMode, bKeySelected ? ColKeySelected : ColKey);

            const bool bKeyHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(ImVec2(X - Radius - 2.0f, MidY - Radius - 2.0f), ImVec2(X + Radius + 2.0f, MidY + Radius + 2.0f));
            if (!bKeyHovered)
            {
                continue;
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bDraggingKey)
            {
                SelectedKeyBinding = BindingIndex;
                SelectedKeyTime = RowKey.Time;
                bDraggingKey = true;
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedBinding = BindingIndex;
                SelectedKeyBinding = BindingIndex;
                SelectedKeyTime = RowKey.Time;
                ImGui::OpenPopup("##KeyContext");
            }
        }

        DrawList->PopClipRect();
    }

    void FSequencerEditMode::DrawTrackRow(ImDrawList* DrawList, int32 TrackIndex, int32 RowIndex, float RowY, bool bTimelineHovered)
    {
        using namespace SequencerEditModeInternal;

        CSequenceTrack* Track = Sequence->Tracks[TrackIndex].Get();

        const float Left = ViewLeft - LabelWidth;
        const ImVec2 RowMin(Left, RowY);
        const ImVec2 RowMax(ViewLeft + ViewWidth, RowY + TrackHeight);
        const bool bSelected = TrackIndex == SelectedTrack;
        const bool bHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(RowMin, RowMax);

        DrawList->AddRectFilled(RowMin, RowMax, PickRowColor(bSelected, bHovered, RowIndex));
        DrawList->AddLine(ImVec2(ViewLeft, RowY), ImVec2(ViewLeft, RowMax.y), ColGridMajor);

        const FVector4 Tint = Track->GetTrackColor();
        const float Alpha = Track->bEnabled ? 1.0f : 0.35f;
        const ImU32 TintColor = ImGui::ColorConvertFloat4ToU32(ImVec4(Tint.x, Tint.y, Tint.z, Alpha));
        const ImU32 TintFaint = ImGui::ColorConvertFloat4ToU32(ImVec4(Tint.x, Tint.y, Tint.z, 0.28f * Alpha));

        const float Indent = Track->BindingIndex != Constants::kIndexNone ? 24.0f : 10.0f;
        DrawList->AddRectFilled(ImVec2(Left + Indent - 7.0f, RowY + 6.0f), ImVec2(Left + Indent - 3.0f, RowMax.y - 6.0f), TintColor, 1.0f);

        const FStringView Label = RowLabel(Track);
        DrawList->PushClipRect(RowMin, ImVec2(ViewLeft - 4.0f, RowMax.y), true);
        DrawList->AddText(ImVec2(Left + Indent, RowY + 5.0f), Track->bEnabled ? (bSelected ? ColText : ColTextDim) : ColTextOff, Label.data(), Label.data() + Label.size());
        DrawList->PopClipRect();

        if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bDraggingTrackKey)
        {
            SelectedTrack = TrackIndex;
            SelectedTrackKey = Constants::kIndexNone;
            SelectedKeyBinding = Constants::kIndexNone;
        }

        if (bHovered && ImGui::GetMousePos().x < ViewLeft && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            SelectedTrack = TrackIndex;
            ImGui::OpenPopup("##TrackContext");
        }

        DrawList->PushClipRect(ImVec2(ViewLeft, RowY), RowMax, true);
        const float MidY = RowY + TrackHeight * 0.5f;

        if (const SKeyedCurve* Curve = FindCurve(Track); Curve != nullptr && Curve->NumKeys() > 0)
        {
            float MinValue = 0.0f;
            float MaxValue = 1.0f;
            Curve->GetValueRange(MinValue, MaxValue);
            MinValue = Math::Min(MinValue, 0.0f);
            MaxValue = Math::Max(MaxValue, MinValue + 1e-3f);

            const float Top = RowY + 4.0f;
            const float Height = TrackHeight - 8.0f;
            ImVec2 Previous;
            for (float X = ViewLeft; X <= ViewLeft + ViewWidth; X += 3.0f)
            {
                const float Value = Curve->Evaluate(XToTime(X));
                const ImVec2 Point(X, Top + Height * (1.0f - (Value - MinValue) / (MaxValue - MinValue)));
                if (X > ViewLeft)
                {
                    DrawList->AddQuadFilled(ImVec2(Previous.x, RowMax.y - 4.0f), Previous, Point, ImVec2(Point.x, RowMax.y - 4.0f), TintFaint);
                    DrawList->AddLine(Previous, Point, TintColor, 1.5f);
                }
                Previous = Point;
            }
        }

        const int32 Count = KeyCount(Track);
        for (int32 KeyIndex = 0; KeyIndex < Count; ++KeyIndex)
        {
            const float X = TimeToX(KeyTime(Track, KeyIndex));
            const bool bKeySelected = bSelected && KeyIndex == SelectedTrackKey;
            const ImU32 Fill = bKeySelected ? ColKeySelected : TintColor;
            float HitRight = X + 6.0f;

            if (const CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track))
            {
                const char* Name = Events->Keys[KeyIndex].Name.c_str();
                DrawList->AddLine(ImVec2(X, RowY + 3.0f), ImVec2(X, RowMax.y - 3.0f), Fill, 2.0f);
                DrawList->AddTriangleFilled(ImVec2(X, RowY + 3.0f), ImVec2(X + 9.0f, RowY + 7.0f), ImVec2(X, RowY + 11.0f), Fill);
                DrawList->AddText(ImVec2(X + 11.0f, RowY + 5.0f), ColText, Name);
                HitRight = X + 11.0f + ImGui::CalcTextSize(Name).x;
            }
            else if (const CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track))
            {
                const SSequenceAudioClip& Clip = Audio->Clips[KeyIndex];
                const char* Name = Clip.Sound.IsValid() ? Clip.Sound->GetName().c_str() : "<no sound>";
                const float Width = Math::Max(ImGui::CalcTextSize(Name).x + 24.0f, 60.0f);
                DrawList->AddRectFilled(ImVec2(X, RowY + 3.0f), ImVec2(X + Width, RowMax.y - 3.0f), bKeySelected ? Fill : TintFaint, 3.0f);
                DrawList->AddRect(ImVec2(X, RowY + 3.0f), ImVec2(X + Width, RowMax.y - 3.0f), TintColor, 3.0f);
                DrawList->AddText(ImVec2(X + 5.0f, RowY + 5.0f), ColText, LE_ICON_MUSIC);
                DrawList->AddText(ImVec2(X + 20.0f, RowY + 5.0f), ColText, Name);
                HitRight = X + Width;
            }
            else
            {
                DrawKeyShape(DrawList, X, MidY, bKeySelected ? 6.5f : 5.0f, ECurveInterpMode::Linear, Fill);
            }

            const bool bKeyHovered = bTimelineHovered && ImGui::IsMouseHoveringRect(ImVec2(X - 7.0f, RowY), ImVec2(HitRight, RowMax.y));
            if (!bKeyHovered)
            {
                continue;
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bDraggingTrackKey)
            {
                SelectedTrack = TrackIndex;
                SelectedTrackKey = KeyIndex;
                bDraggingTrackKey = true;
                bPlaying = false;
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedTrack = TrackIndex;
                SelectedTrackKey = KeyIndex;
                ImGui::OpenPopup("##TrackKeyContext");
            }
        }

        DrawList->PopClipRect();
    }

    void FSequencerEditMode::UpdateKeyDrags(CWorld* World, float Duration)
    {
        using namespace SequencerEditModeInternal;

        const float DragTime = Sequence->SnapToFrame(Math::Clamp(XToTime(ImGui::GetMousePos().x), 0.0f, Duration));

        if (bDraggingKey)
        {
            CSequenceTrack_Transform* Track = FindTransformTrack(SelectedKeyBinding);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || Track == nullptr)
            {
                bDraggingKey = false;
            }
            // A key dropped onto another would merge with it, and the next frame's match could carry off the wrong one.
            else if (Math::Abs(DragTime - SelectedKeyTime) > KeyMatchEpsilon && !HasTransformKeyAt(Track, DragTime))
            {
                MoveTransformKeys(Track, SelectedKeyTime, DragTime);
                SelectedKeyTime = DragTime;
                CommitEdit(World);
            }
        }

        if (bDraggingTrackKey)
        {
            CSequenceTrack* Track = SelectedTrack >= 0 && SelectedTrack < (int32)Sequence->Tracks.size() ? Sequence->Tracks[SelectedTrack].Get() : nullptr;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || Track == nullptr || SelectedTrackKey < 0 || SelectedTrackKey >= KeyCount(Track))
            {
                bDraggingTrackKey = false;
            }
            else if (Math::Abs(DragTime - KeyTime(Track, SelectedTrackKey)) > 1e-4f)
            {
                // Two curve keys at one time form a zero-width segment, so a curve key hops over an occupied frame.
                const SKeyedCurve* Curve = FindCurve(Track);
                if (Curve == nullptr || !HasKeyNear(*Curve, DragTime))
                {
                    SelectedTrackKey = RetimeKey(Track, SelectedTrackKey, DragTime);
                    CommitEdit(World);
                }
            }
        }
    }

    void FSequencerEditMode::DrawBindingPopups(CWorld* World)
    {
        using namespace SequencerEditModeInternal;

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
            CSequenceTrack_Transform* Track = FindTransformTrack(SelectedKeyBinding);

            // The mode belongs to the key it leaves, so this shapes the segment from here to the next key.
            if (Track != nullptr && ImGui::BeginMenu(LE_ICON_VECTOR_CURVE " Interpolation"))
            {
                const ECurveInterpMode Current = GetTransformKeyInterp(Track, SelectedKeyTime);
                for (const FInterpChoice& Choice : InterpChoices)
                {
                    if (ImGui::MenuItem(Choice.Label, nullptr, Current == Choice.Mode))
                    {
                        SetTransformKeyInterp(Track, SelectedKeyTime, Choice.Mode);
                        CommitEdit(World);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_DELETE " Delete Key"))
            {
                DeleteSelectedKey(World);
            }

            ImGui::EndPopup();
        }
    }

    void FSequencerEditMode::DrawTrackPopups(CWorld* World)
    {
        using namespace SequencerEditModeInternal;

        CSequenceTrack* Track = SelectedTrack >= 0 && SelectedTrack < (int32)Sequence->Tracks.size() ? Sequence->Tracks[SelectedTrack].Get() : nullptr;

        if (ImGui::BeginPopup("##TrackContext"))
        {
            if (Track != nullptr)
            {
                if (ImGui::MenuItem(Track->bEnabled ? LE_ICON_EYE_OFF " Disable" : LE_ICON_EYE " Enable"))
                {
                    Track->bEnabled = !Track->bEnabled;
                    CommitEdit(World);
                }

                if (FindCurve(Track) != nullptr && ImGui::MenuItem(LE_ICON_KEY_PLUS " Key At Playhead"))
                {
                    KeyTrackAtPlayhead(World, Track);
                }

                if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track); Events != nullptr && ImGui::MenuItem(LE_ICON_FLAG " Add Event At Playhead"))
                {
                    SSequenceEventKey& Added = Events->Keys.emplace_back();
                    Added.Time = Sequence->SnapToFrame(PlayTime);
                    Added.Name = FName("Event");
                    SelectedTrackKey = RetimeKey(Track, (int32)Events->Keys.size() - 1, Added.Time);
                    MarkSequenceDirty();
                }

                if (CSequenceTrack_Audio* Audio = Cast<CSequenceTrack_Audio>(Track); Audio != nullptr && ImGui::MenuItem(LE_ICON_MUSIC " Add Clip At Playhead"))
                {
                    SSequenceAudioClip& Clip = Audio->Clips.emplace_back();
                    Clip.StartTime = Sequence->SnapToFrame(PlayTime);
                    SelectedTrackKey = (int32)Audio->Clips.size() - 1;
                    MarkSequenceDirty();
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
            SKeyedCurve* Curve = Track != nullptr ? FindCurve(Track) : nullptr;
            if (Curve != nullptr && SelectedTrackKey >= 0 && SelectedTrackKey < Curve->NumKeys() && ImGui::BeginMenu(LE_ICON_VECTOR_CURVE " Interpolation"))
            {
                for (const FInterpChoice& Choice : InterpChoices)
                {
                    if (ImGui::MenuItem(Choice.Label, nullptr, Curve->Keys[SelectedTrackKey].InterpMode == Choice.Mode))
                    {
                        Curve->Keys[SelectedTrackKey].InterpMode = Choice.Mode;
                        Curve->ComputeAutoTangents();
                        CommitEdit(World);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(LE_ICON_DELETE " Delete Key"))
            {
                DeleteSelectedKey(World);
            }
            ImGui::EndPopup();
        }
    }

    void FSequencerEditMode::DrawDetails(CWorld* World)
    {
        using namespace SequencerEditModeInternal;

        CSequenceTrack* Track = SelectedTrack >= 0 && SelectedTrack < (int32)Sequence->Tracks.size() ? Sequence->Tracks[SelectedTrack].Get() : nullptr;
        if (Track == nullptr)
        {
            Details.reset();
            DetailsTarget = nullptr;
            return;
        }

        const FStringView Label = RowLabel(Track);
        ImGui::Spacing();
        ImGui::SeparatorText(FString(Label.data(), Label.size()).c_str());

        bool bChanged = false;
        if (SKeyedCurve* Curve = FindCurve(Track); Curve != nullptr && SelectedTrackKey >= 0 && SelectedTrackKey < Curve->NumKeys())
        {
            SCurveKey& Selected = Curve->Keys[SelectedTrackKey];
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragFloat("Value", &Selected.Value, 0.01f))
            {
                Curve->ComputeAutoTangents();
                bChanged = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("at %.2fs", Selected.Time);
        }
        else if (CSequenceTrack_Event* Events = Cast<CSequenceTrack_Event>(Track); Events != nullptr && SelectedTrackKey >= 0 && SelectedTrackKey < (int32)Events->Keys.size())
        {
            SSequenceEventKey& Selected = Events->Keys[SelectedTrackKey];

            char NameBuffer[128];
            snprintf(NameBuffer, sizeof(NameBuffer), "%s", Selected.Name.c_str());
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("Name", NameBuffer, sizeof(NameBuffer)))
            {
                Selected.Name = FName(NameBuffer);
                bChanged = true;
            }

            ImGui::SameLine();
            char PayloadBuffer[256];
            snprintf(PayloadBuffer, sizeof(PayloadBuffer), "%s", Selected.Payload.c_str());
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputText("Payload", PayloadBuffer, sizeof(PayloadBuffer)))
            {
                Selected.Payload = PayloadBuffer;
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

        if (DetailsTarget != Track || !Details)
        {
            Details = MakeUnique<FPropertyTable>(static_cast<CObject*>(Track));
            Details->SetShowSearchBar(false);
            Details->SetPostEditCallback([this](const FPropertyChangedEvent&) { bDetailsEdited = true; });
            DetailsTarget = Track;
            bDetailsEdited = false;
        }
        Details->DrawTree();

        // Flagged rather than evaluated in the callback, so the table never holds on to a world.
        if (bChanged || bDetailsEdited)
        {
            bDetailsEdited = false;
            CommitEdit(World);
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
                for (const SSequenceCameraCut& Cut : Cuts->Cuts)
                {
                    OutTimes.push_back(Cut.StartTime);
                    OutTimes.push_back(Cut.EndTime);
                }
            }
            else
            {
                const int32 Count = SequencerEditModeInternal::KeyCount(Track.Get());
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    OutTimes.push_back(SequencerEditModeInternal::KeyTime(Track.Get(), Index));
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

        // Half a frame of slack, so a playhead already parked on a key steps off it instead of re-landing.
        const float Slack = 0.5f / (float)Math::Max(Sequence->FrameRate, 1);

        const float* Target = nullptr;
        if (Direction > 0)
        {
            Target = Algo::FindIf(Times, [&](float Time) { return Time > PlayTime + Slack; });
            Target = Target != Times.end() ? Target : nullptr;
        }
        else
        {
            for (int32 Index = (int32)Times.size() - 1; Index >= 0 && Target == nullptr; --Index)
            {
                Target = Times[Index] < PlayTime - Slack ? &Times[Index] : nullptr;
            }
        }

        if (Target != nullptr)
        {
            bPlaying = false;
            EvaluateAt(World, Math::Clamp(*Target, 0.0f, Sequence->Duration), true);
        }
    }

    void FSequencerEditMode::StepFrames(CWorld* World, int32 FrameDelta)
    {
        bPlaying = false;

        const int32 Frame = Math::Clamp(Sequence->TimeToFrame(PlayTime) + FrameDelta, 0, Math::Max(Sequence->GetFrameCount(), 0));
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
        MarkSequenceDirty();

        PlayTime = Math::Min(PlayTime, Sequence->Duration);
        TimelineScroll = 0.0f;
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
        if (Selected == ECS::NullEntity)
        {
            ImGuiX::Notifications::NotifyWarning("Select an entity in the world to bind it.");
            return Constants::kIndexNone;
        }
        return AddBinding(World, Selected);
    }

    int32 FSequencerEditMode::AddBinding(CWorld* World, ECS::FEntity Entity)
    {
        if (!Sequence.IsValid() || World == nullptr || !World->IsValidEntity(Entity))
        {
            return Constants::kIndexNone;
        }

        FName EntityName = "Entity";
        if (const SNameComponent* NameComponent = World->TryGetComponent<SNameComponent>(Entity))
        {
            EntityName = NameComponent->Name;
        }

        // Possession resolves by name, so a second binding of the same name could only ever find the same entity.
        const int32 Existing = Algo::IndexOf(Sequence->Bindings, EntityName, &SSequenceBinding::Name);
        if (Existing != Constants::kIndexNone)
        {
            return Existing;
        }

        SSequenceBinding& Binding = Sequence->Bindings.emplace_back();
        Binding.Name = EntityName;
        Binding.Kind = ESequenceBindingKind::Possess;

        MarkSequenceDirty();
        BindToWorld(World);

        return (int32)Sequence->Bindings.size() - 1;
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
            if (!Track.IsValid() || Track->BindingIndex == BindingIndex)
            {
                continue;
            }

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

                Cuts->Cuts = Move(KeptCuts);
            }

            Kept.push_back(Track);
        }

        Sequence->Tracks = Move(Kept);
        Sequence->Bindings.erase(Sequence->Bindings.begin() + BindingIndex);
        MarkSequenceDirty();

        ResetSelection();
        BindToWorld(World);
    }

    CSequenceTrack_Transform* FSequencerEditMode::FindTransformTrack(int32 BindingIndex) const
    {
        if (!Sequence.IsValid() || BindingIndex == Constants::kIndexNone)
        {
            return nullptr;
        }

        for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
        {
            if (Track.IsValid() && Track->BindingIndex == BindingIndex)
            {
                if (CSequenceTrack_Transform* Transform = Cast<CSequenceTrack_Transform>(Track.Get()))
                {
                    return Transform;
                }
            }
        }

        return nullptr;
    }

    CSequenceTrack_Transform* FSequencerEditMode::FindOrCreateTransformTrack(int32 BindingIndex)
    {
        if (CSequenceTrack_Transform* Existing = FindTransformTrack(BindingIndex))
        {
            return Existing;
        }

        // Outered to the sequence's package so the track is an export and survives a save.
        CSequenceTrack_Transform* Created = NewObject<CSequenceTrack_Transform>(Sequence->GetPackage(), "TransformTrack");
        Created->BindingIndex = BindingIndex;

        // Keying is the usual way a track gets made, and a track that drives nothing is a trap.
        Created->Location.bEnabled = true;
        Created->Rotation.bEnabled = true;
        Created->Scale.bEnabled = true;

        Sequence->Tracks.push_back(Created);
        return Created;
    }

    void FSequencerEditMode::KeyTransform(CWorld* World, int32 BindingIndex)
    {
        if (World == nullptr || !Sequence.IsValid() || BindingIndex < 0 || BindingIndex >= (int32)Instance.BoundEntities.size())
        {
            return;
        }

        const ECS::FEntity Entity = Instance.BoundEntities[BindingIndex];
        if (Entity == ECS::NullEntity || !World->IsValidEntity(Entity))
        {
            ImGuiX::Notifications::NotifyWarning("That binding did not resolve to an entity in this world.");
            return;
        }

        const STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        CSequenceTrack_Transform* Track = FindOrCreateTransformTrack(BindingIndex);
        const float Time = Sequence->SnapToFrame(PlayTime);

        SequencerEditModeInternal::KeyVector(Track->Location, Time, Transform->GetLocation(), false);
        SequencerEditModeInternal::KeyVector(Track->Rotation, Time, Math::Degrees(Math::YawFirstEulerAngles(Transform->GetRotation())), false);
        SequencerEditModeInternal::KeyVector(Track->Scale, Time, Transform->GetScale(), false);
        Track->Rotation.UnwindAngles();
        SequencerEditModeInternal::RefreshTangents(Track);

        MarkSequenceDirty();
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

        Track->Location.bEnabled = true;
        Track->Rotation.bEnabled = true;
        SequencerEditModeInternal::KeyVector(Track->Location, Time, Location, true);
        SequencerEditModeInternal::KeyVector(Track->Rotation, Time, Rotation, true);
        Track->Rotation.UnwindAngles();
        SequencerEditModeInternal::RefreshTangents(Track);

        // Scale stays unkeyed so a camera never inherits a stray scale from the editor entity.
        Track->Scale.bEnabled = false;

        CommitEdit(World);
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
        MarkSequenceDirty();

        BindToWorld(World);
        SelectedBinding = BindingIndex;
        KeyFromView(World, BindingIndex);
        AddCameraCutAtPlayhead(World);
        EvaluateAt(World, PlayTime, true);
    }

    void FSequencerEditMode::DeleteSelectedKey(CWorld* World)
    {
        if (bDraggingKey || bDraggingTrackKey)
        {
            return;
        }

        if (SelectedKeyBinding != Constants::kIndexNone)
        {
            if (CSequenceTrack_Transform* Track = FindTransformTrack(SelectedKeyBinding))
            {
                SequencerEditModeInternal::DeleteTransformKeys(Track, SelectedKeyTime);
                CommitEdit(World);
            }
            SelectedKeyBinding = Constants::kIndexNone;
            return;
        }

        if (SelectedTrackKey != Constants::kIndexNone && SelectedTrack >= 0 && SelectedTrack < (int32)Sequence->Tracks.size())
        {
            if (CSequenceTrack* Track = Sequence->Tracks[SelectedTrack].Get())
            {
                SequencerEditModeInternal::RemoveKey(Track, SelectedTrackKey);
                CommitEdit(World);
            }
            SelectedTrackKey = Constants::kIndexNone;
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
            if (CSequenceTrack_CameraCut* Cut = Track.IsValid() ? Cast<CSequenceTrack_CameraCut>(Track.Get()) : nullptr)
            {
                return Cut;
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
        if (!Sequence.IsValid() || SelectedBinding < 0 || SelectedBinding >= (int32)Sequence->Bindings.size())
        {
            ImGuiX::Notifications::NotifyWarning("Select the binding holding the camera you want to cut to.");
            return;
        }

        // A binding with no camera would produce a cut that silently does nothing at runtime.
        if (World != nullptr && SelectedBinding < (int32)Instance.BoundEntities.size())
        {
            const ECS::FEntity Entity = Instance.BoundEntities[SelectedBinding];
            if (Entity != ECS::NullEntity && World->IsValidEntity(Entity) && World->TryGetComponent<SCameraComponent>(Entity) == nullptr)
            {
                ImGuiX::Notifications::NotifyWarning("'{0}' has no camera component.", Sequence->Bindings[SelectedBinding].Name);
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
        CommitEdit(World);
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
            MarkSequenceDirty();
            return Transform;
        }

        // Outered to the sequence's package so the track is an export and survives a save.
        CSequenceTrack* Track = NewObject<CSequenceTrack>(Class, Sequence->GetPackage());
        Track->BindingIndex = BindingIndex;

        // A camera's lens is what a property track drives most, so a camera binding starts on its FOV.
        if (CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))
        {
            const ECS::FEntity Entity = BindingIndex >= 0 && BindingIndex < (int32)Instance.BoundEntities.size() ? Instance.BoundEntities[BindingIndex] : ECS::NullEntity;
            if (Entity != ECS::NullEntity && World != nullptr && World->IsValidEntity(Entity) && World->TryGetComponent<SCameraComponent>(Entity) != nullptr)
            {
                Property->ComponentType = FName("SCameraComponent");
                Property->PropertyPath = "FOV";
            }
        }

        Sequence->Tracks.push_back(Track);
        SelectedTrack = (int32)Sequence->Tracks.size() - 1;
        SelectedTrackKey = Constants::kIndexNone;
        MarkSequenceDirty();

        if (SequencerEditModeInternal::FindCurve(Track) != nullptr)
        {
            KeyTrackAtPlayhead(World, Track);
        }
        return Track;
    }

    void FSequencerEditMode::KeyTrackAtPlayhead(CWorld* World, CSequenceTrack* Track)
    {
        SKeyedCurve* Curve = SequencerEditModeInternal::FindCurve(Track);
        if (Curve == nullptr)
        {
            return;
        }

        const float Time = Sequence->SnapToFrame(PlayTime);
        float Value = Curve->NumKeys() > 0 ? Curve->Evaluate(Time) : SequencerEditModeInternal::RestingValue(Track);

        // A property keys what the entity holds now, so posing it in the details panel and keying is the workflow.
        if (const CSequenceTrack_Property* Property = Cast<CSequenceTrack_Property>(Track))
        {
            const int32 Binding = Property->BindingIndex;
            if (Binding >= 0 && Binding < (int32)Instance.BoundEntities.size())
            {
                Property->SampleValue(World, Instance.BoundEntities[Binding], Value);
            }
        }

        const int32 Index = Curve->UpdateOrAddKey(Time, Value);
        Curve->Keys[Index].InterpMode = ECurveInterpMode::Cubic;
        Curve->ComputeAutoTangents();

        SelectedTrack = Algo::IndexOfIf(Sequence->Tracks, [Track](const TStrongObjectPtr<CSequenceTrack>& Other) { return Other.Get() == Track; });
        SelectedTrackKey = Index;
        CommitEdit(World);
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
        MarkSequenceDirty();

        SelectedTrack = Constants::kIndexNone;
        SelectedTrackKey = Constants::kIndexNone;
        bDraggingTrackKey = false;
        Details.reset();
        DetailsTarget = nullptr;

        BindToWorld(World);
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

        if (Instance.PreviousCamera != ECS::NullEntity && World->IsValidEntity(Instance.PreviousCamera))
        {
            World->GetCamera()->SetActiveCamera(Instance.PreviousCamera);
        }
    }
}
