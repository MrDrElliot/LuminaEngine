#include "Containers/StringFormat.h"
#include "AnimationEditorTool.h"
#include "TimelineDrawing.h"
#include "World/ECS/Registry.h"

#include <string>
#include "Core/Math/Math.h"
#include <imgui_internal.h>

#include "ImGuiDrawUtils.h"
#include "implot.h"
#include "Animation/AnimNotify.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectIterator.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"


namespace Lumina
{
    static const char* DetailsName              = "Details";
    static const char* SequencerName            = "Sequencer";

    FAnimationEditorTool::FAnimationEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FAnimationEditorTool::OnInitialize()
    {
        CreateToolWindow(DetailsName, [&](bool bFocused)
        {
            CAnimation* Animation = GetAsset<CAnimation>();
            const bool bNotifySelected = Animation != nullptr && SelectedKind != ENotifyKind::None;

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText(bNotifySelected ? "Notify Details" : "Asset Details");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            if (bNotifySelected)
            {
                DrawNotifyInspector(Animation);
                return;
            }

            PropertyTable.DrawTree();
        });

        CreateToolWindow(SequencerName, [&](bool bFocused)
        {
            DrawSequencer();
        });

        NotifyDetails.SetPostEditCallback([this](const FPropertyChangedEvent&)
        {
            MarkAnimationDirty();
        });
    }

    void FAnimationEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        CreateFloorPlane();

        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);

        CAnimation* Animation = Cast<CAnimation>(Asset.Get());
        if (!Animation->Skeleton.IsValid())
        {
            return;
        }

        CameraState.Speed = 5.0f;

        CreatePreviewEntity();
        if (MeshEntity == ECS::NullEntity)
        {
            return;
        }

        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        FQuat Rotation = Math::FindLookAtRotation(MeshTransform.GetLocation() + FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(Rotation);
    }

    void FAnimationEditorTool::CreatePreviewEntity()
    {
        CAnimation* Animation = GetAsset<CAnimation>();
        float PreviousTime = 0.0f;
        if (const SSimpleAnimationComponent* Previous = GetPreviewComponent())
        {
            PreviousTime = Previous->CurrentTime;
        }

        if (MeshEntity != ECS::NullEntity)
        {
            World->DestroyEntity(MeshEntity);
            MeshEntity = ECS::NullEntity;
        }

        CSkeletalMesh* PreviewMesh = Animation != nullptr ? PreviewMeshSelection.Resolve(Animation->Skeleton.Get()) : nullptr;
        PreviewMeshInUse = PreviewMesh;
        if (PreviewMesh == nullptr)
        {
            return;
        }

        MeshEntity = World->ConstructEntity("MeshEntity");
        World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SetSkeletalMesh(PreviewMesh);
        SSimpleAnimationComponent& AnimComp = World->EmplaceComponent<SSimpleAnimationComponent>(MeshEntity);
        AnimComp.Animation   = Animation;
        AnimComp.bPlaying    = false;
        AnimComp.bLooping    = bLooping;
        AnimComp.CurrentTime = PreviousTime;
    }

    void FAnimationEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        CAnimation* Animation = GetAsset<CAnimation>();
        if (World.IsValid() && Animation != nullptr && PreviewMeshSelection.Resolve(Animation->Skeleton.Get()) != PreviewMeshInUse.Get())
        {
            CreatePreviewEntity();
        }

        if (World && World->GetRenderer())
        {
            World->GetRenderSettings().bDrawBillboards = false;
        }
    }

    void FAnimationEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FAnimationEditorTool::OnAssetLoadFinished()
    {
        if (CAnimation* Animation = GetAsset<CAnimation>())
        {
            EnsureNotifyTracks(Animation->GetAnimationResource());
        }
    }

    void FAnimationEditorTool::OnPropertyEditFinished(const FPropertyChangedEvent& Event)
    {
        // A paused preview holds its last pose, so an additive-settings edit needs a forced resample.
        if (SSimpleAnimationComponent* AnimComp = GetPreviewComponent())
        {
            AnimComp->bDirty = true;
        }
    }

    SSimpleAnimationComponent* FAnimationEditorTool::GetPreviewComponent() const
    {
        if (!World.IsValid() || MeshEntity == ECS::NullEntity)
        {
            return nullptr;
        }
        return World->TryGetComponent<SSimpleAnimationComponent>(MeshEntity);
    }

    void FAnimationEditorTool::SerializeAssetForUndo(FArchive& Ar, CObject* InAsset)
    {
        FAssetEditorTool::SerializeAssetForUndo(Ar, InAsset);

        // The timeline edits live in the resource, which reflection does not reach, while the pose data stays out.
        CAnimation* Animation = Cast<CAnimation>(InAsset);
        FAnimationResource* Resource = Animation != nullptr ? Animation->GetAnimationResource() : nullptr;
        if (Resource == nullptr)
        {
            return;
        }

        Ar << Resource->Notifies;
        Ar << Resource->NotifyStates;
        Ar << Resource->NotifyTracks;
        Ar << Resource->Curves;
        Ar << Resource->SyncTrackName;

        if (Ar.IsReading())
        {
            Resource->RebuildSyncTrack();
        }
    }

    void FAnimationEditorTool::MarkAnimationDirty()
    {
        if (CAnimation* Animation = GetAsset<CAnimation>())
        {
            // Markers are reflected from notifies, so any timeline edit can move them.
            Animation->GetAnimationResource()->RebuildSyncTrack();
        }

        if (Asset.IsValid() && Asset->GetPackage())
        {
            Asset->GetPackage()->MarkDirty();
        }
    }

    float FAnimationEditorTool::SnapTime(float Time, float Duration) const
    {
        if (bSnapToFrame && FrameRate > 0)
        {
            const float Step = 1.0f / (float)FrameRate;
            Time = roundf(Time / Step) * Step;
        }
        return Math::Clamp(Time, 0.0f, Duration);
    }

    void FAnimationEditorTool::ClearSelection()
    {
        SelectedKind  = ENotifyKind::None;
        SelectedIndex = -1;
    }

    void FAnimationEditorTool::EnsureNotifyTracks(FAnimationResource* Resource)
    {
        if (Resource == nullptr || !Resource->NotifyTracks.empty())
        {
            return;
        }

        // Rebuilds the lane list from the notifies, then guarantees one default lane to author into.
        auto AddUnique = [&](const FName& Track)
        {
            if (Track.IsNone())
            {
                return;
            }
            for (const FName& Existing : Resource->NotifyTracks)
            {
                if (Existing == Track)
                {
                    return;
                }
            }
            Resource->NotifyTracks.push_back(Track);
        };

        for (const FAnimationNotify& N : Resource->Notifies)        { AddUnique(N.NotifyTrack); }
        for (const FAnimationNotifyState& S : Resource->NotifyStates) { AddUnique(S.NotifyTrack); }

        if (Resource->NotifyTracks.empty())
        {
            Resource->NotifyTracks.push_back(FName("Default"));
        }
    }

    void FAnimationEditorTool::AddNotifyAt(FAnimationResource* Resource, FName Track, float Time, bool bState)
    {
        const int32 ColorIdx = (int32)(Resource->Notifies.size() + Resource->NotifyStates.size());
        if (bState)
        {
            FAnimationNotifyState State;
            State.NotifyName  = FName("NewNotifyState");
            State.NotifyTrack = Track;
            State.StartTime   = Time;
            State.EndTime     = Math::Min(Time + Math::Max(Resource->Duration * 0.1f, 0.1f), Resource->Duration);
            State.Color       = Timeline::PaletteColor(ColorIdx);
            Resource->NotifyStates.push_back(State);

            SelectedKind  = ENotifyKind::State;
            SelectedIndex = (int)Resource->NotifyStates.size() - 1;
        }
        else
        {
            FAnimationNotify Notify;
            Notify.NotifyName  = FName("NewNotify");
            Notify.NotifyTrack = Track;
            Notify.Time        = Time;
            Notify.Color       = Timeline::PaletteColor(ColorIdx);
            Resource->Notifies.push_back(Notify);

            SelectedKind  = ENotifyKind::Point;
            SelectedIndex = (int)Resource->Notifies.size() - 1;
        }
        MarkAnimationDirty();
    }

    void FAnimationEditorTool::DeleteSelected(FAnimationResource* Resource)
    {
        if (SelectedKind == ENotifyKind::Point && SelectedIndex >= 0 && SelectedIndex < (int)Resource->Notifies.size())
        {
            Resource->Notifies.erase(Resource->Notifies.begin() + SelectedIndex);
            MarkAnimationDirty();
        }
        else if (SelectedKind == ENotifyKind::State && SelectedIndex >= 0 && SelectedIndex < (int)Resource->NotifyStates.size())
        {
            Resource->NotifyStates.erase(Resource->NotifyStates.begin() + SelectedIndex);
            MarkAnimationDirty();
        }
        ClearSelection();
    }

    void FAnimationEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Playback",
            "Play/Pause and Stop drive the preview through the animation system, so the clip "
            "plays exactly as it will at runtime. Drag the playhead on the ruler to scrub.");
        DrawHelpTextRow("Notify Tracks",
            "Add lanes with '+ Track'. Double-click a lane to drop a Notify (instant), or right-click it "
            "for a Notify State (ranged). Drag markers to move them, drag a state's edges to resize, and "
            "press Delete to remove the selection. Snap to Frame quantizes edits to the frame rate. "
            "The wheel zooms, Shift pans, and Ctrl or the track headers scroll.");
        DrawHelpTextRow("Listening in script",
            "Grab the entity's SimpleAnimationComponent and bind by name:\n"
            "  anim:BindNotify(\"Footstep\", function(e, name, t) ... end)\n"
            "  anim:BindNotifyState(\"Trail\", onBegin, onTick, onEnd)\n"
            "Bindings are per-entity and fire as the playhead crosses the notify.");
        DrawHelpTextRow("Curves",
            "Float Curves are named value tracks authored on the clip. Add one, scrub the playhead and "
            "type a value to key it, or edit keys and tangents directly in the curve view. They blend "
            "through the animation graph with the pose: read one with a Get Curve node, or from script "
            "with GetCurveValue(\"Name\").\n"
            "Bone Channels plots the imported per-bone keyframes for inspection (read-only).");
    }

    void FAnimationEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        if (ImGui::BeginMenu(LE_ICON_MOVE_RESIZE " Gizmo Control"))
        {
            const char* operations[] = { "Translate", "Rotate", "Scale" };
            static int currentOp = 0;

            if (ImGui::Combo("##", &currentOp, operations, IM_ARRAYSIZE(operations)))
            {
                switch (currentOp)
                {
                case 0: GuizmoOp = ImGuizmo::TRANSLATE; break;
                case 1: GuizmoOp = ImGuizmo::ROTATE;    break;
                case 2: GuizmoOp = ImGuizmo::SCALE;     break;
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LE_ICON_BONE " Skeleton"))
        {
            DrawSkeletonDebugMenuItems();
            ImGui::EndMenu();
        }

        CAnimation* Animation = GetAsset<CAnimation>();
        if (PreviewMeshSelection.DrawMenu(Animation != nullptr ? Animation->Skeleton.Get() : nullptr))
        {
            CreatePreviewEntity();
        }
    }

    void FAnimationEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0, bottomDockID = 0;

        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.25f, &rightDockID, &leftDockID);
        ImGui::DockBuilderSplitNode(leftDockID, ImGuiDir_Down, 0.44f, &bottomDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(DetailsName).c_str(), rightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SequencerName).c_str(), bottomDockID);
    }

    // Sequencer / Timeline

    void FAnimationEditorTool::DrawSequencer()
    {
        CAnimation* Animation = GetAsset<CAnimation>();
        if (!Animation)
        {
            return;
        }

        FAnimationResource* Resource = Animation->GetAnimationResource();
        if (!Resource)
        {
            return;
        }
        EnsureNotifyTracks(Resource);

        SSimpleAnimationComponent* AnimComp = GetPreviewComponent();
        if (AnimComp == nullptr)
        {
            ImGui::TextDisabled("No preview mesh. Assign a skeleton, then pick a mesh from the Preview Mesh menu to scrub this clip.");
            return;
        }

        const float Duration = Math::Max(Animation->GetDuration(), 0.0001f);

        DrawTransport(AnimComp, Duration);

        ImGui::Separator();

        if (ImGui::BeginTabBar("##AnimTabs", ImGuiTabBarFlags_None))
        {
            if (ImGui::BeginTabItem(LE_ICON_BELL " Notifies"))
            {
                DrawNotifyTimeline(Animation, AnimComp, Duration);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(LE_ICON_CHART_BELL_CURVE " Curves"))
            {
                DrawCurvesTab(Animation, AnimComp, Duration);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    void FAnimationEditorTool::DrawTransport(SSimpleAnimationComponent* AnimComp, float Duration)
    {
        // The editor world is paused, so advance the playhead from the UI clock and set bDirty.
        AnimComp->bPlaying = false;
        AnimComp->bLooping = bLooping;

        if (bIsPlaying)
        {
            AnimComp->CurrentTime += ImGui::GetIO().DeltaTime * Playrate;
            if (AnimComp->CurrentTime >= Duration)
            {
                if (bLooping)
                {
                    AnimComp->CurrentTime = fmodf(AnimComp->CurrentTime, Duration);
                }
                else
                {
                    AnimComp->CurrentTime = Duration;
                    bIsPlaying = false;
                }
            }
            AnimComp->bDirty = true;
        }

        const bool bShortcutsActive = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
        const bool bSpacePressed = bShortcutsActive && ImGui::IsKeyPressed(ImGuiKey_Space, false);

        if (bShortcutsActive && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && SelectedKind != ENotifyKind::None)
        {
            if (CAnimation* Animation = GetAsset<CAnimation>())
            {
                DeleteSelected(Animation->GetAnimationResource());
            }
        }

        if (ImGui::Button(bIsPlaying ? LE_ICON_PAUSE " Pause" : LE_ICON_PLAY " Play", ImVec2(96, 0)) || bSpacePressed)
        {
            if (!bIsPlaying && AnimComp->CurrentTime >= Duration && !bLooping)
            {
                AnimComp->CurrentTime = 0.0f;
            }
            bIsPlaying = !bIsPlaying;
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_STOP " Stop", ImVec2(80, 0)))
        {
            bIsPlaying            = false;
            AnimComp->CurrentTime = 0.0f;
            AnimComp->bDirty      = true;
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_STEP_BACKWARD, ImVec2(36, 0)))
        {
            AnimComp->CurrentTime = SnapTime(AnimComp->CurrentTime - 1.0f / (float)FrameRate, Duration);
            AnimComp->bDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_STEP_FORWARD, ImVec2(36, 0)))
        {
            AnimComp->CurrentTime = SnapTime(AnimComp->CurrentTime + 1.0f / (float)FrameRate, Duration);
            AnimComp->bDirty = true;
        }

        ImGui::SameLine();
        ImGui::Checkbox(LE_ICON_REPEAT " Loop", &bLooping);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("##PlayRate", &Playrate, 0.05f, 4.0f, "Rate %.2fx");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("##Zoom", &TimelineZoom, 1.0f, 16.0f, LE_ICON_MAGNIFY " %.1fx");

        ImGui::SameLine();
        ImGui::Checkbox("Snap", &bSnapToFrame);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragInt("##FPS", &FrameRate, 1.0f, 1, 240, "%d fps");

        // Time / frame readout, right-aligned.
        const int Frame      = (int)roundf(AnimComp->CurrentTime * (float)FrameRate);
        const int TotalFrame = (int)roundf(Duration * (float)FrameRate);
        const FString Readout = Format("{:.3f}s / {:.3f}s   (frame {}/{})", AnimComp->CurrentTime, Duration, Frame, TotalFrame);
        const float TextW = ImGui::CalcTextSize(Readout.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(Math::Max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - TextW - 16.0f));
        ImGui::TextUnformatted(Readout.c_str());
    }

    void FAnimationEditorTool::DrawNotifyTimeline(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration)
    {
        FAnimationResource* Resource = Animation->GetAnimationResource();

        ImGui::BeginChild("TimelineCanvas", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList*  DrawList   = ImGui::GetWindowDrawList();
        const ImVec2 CanvasPos  = ImGui::GetCursorScreenPos();
        const ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
        const float  CanvasY1   = CanvasPos.y + CanvasSize.y;
        const int    NumTracks  = (int)Resource->NotifyTracks.size();
        const float  RowHeight  = Timeline::TrackHeight;

        const float RulerY0     = CanvasPos.y;
        const float TrackAreaY0 = CanvasPos.y + Timeline::RulerHeight;
        const float MaxTrackScroll = Math::Max(0.0f, (float)NumTracks * RowHeight - (CanvasY1 - TrackAreaY0));
        TrackScroll = Math::Clamp(TrackScroll, 0.0f, MaxTrackScroll);
        const float TracksY0 = TrackAreaY0 - TrackScroll;

        Timeline::FView View;
        View.LaneX0    = CanvasPos.x + Timeline::HeaderWidth;
        View.LaneX1    = CanvasPos.x + CanvasSize.x;
        View.ContentX0 = View.LaneX0 + Timeline::LanePad;
        const float LaneX0   = View.LaneX0;
        const float LaneX1   = View.LaneX1;
        const float ContentW = Math::Max(LaneX1 - LaneX0 - Timeline::LanePad * 2.0f, 1.0f);
        const float BasePPS  = ContentW / Duration;
        View.PPS = BasePPS * TimelineZoom;

        // Clamp pan so the clip stays reachable.
        TimelinePanSeconds = Math::Clamp(TimelinePanSeconds, 0.0f, Math::Max(0.0f, Duration - ContentW / View.PPS));
        View.Pan = TimelinePanSeconds;

        const ImGuiIO& IO = ImGui::GetIO();
        const ImVec2 Mouse = IO.MousePos;
        const bool bCanvasHovered = ImGui::IsWindowHovered();
        const bool bLanesHovered = bCanvasHovered && Mouse.y >= TrackAreaY0 && Mouse.x >= LaneX0 && Mouse.x <= LaneX1;

        // The wheel scrolls tracks over the headers or with Ctrl, zooms toward the cursor over the lanes, and pans with Shift.
        if (bCanvasHovered && IO.MouseWheel != 0.0f && (Mouse.x <= LaneX0 || IO.KeyCtrl))
        {
            TrackScroll = Math::Clamp(TrackScroll - IO.MouseWheel * RowHeight, 0.0f, MaxTrackScroll);
        }
        else if (bCanvasHovered && IO.MouseWheel != 0.0f && Mouse.x > LaneX0)
        {
            if (IO.KeyShift)
            {
                TimelinePanSeconds -= IO.MouseWheel * (ContentW / View.PPS) * 0.15f;
            }
            else
            {
                const float CursorT = View.XToTime(Mouse.x);
                TimelineZoom = Math::Clamp(TimelineZoom * (IO.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 1.0f, 16.0f);
                TimelinePanSeconds = CursorT - (Mouse.x - View.ContentX0) / (BasePPS * TimelineZoom);
            }
        }

        // Finds the lane row index for a track name.
        auto TrackRow = [&](const FName& Track) -> int
        {
            for (int t = 0; t < NumTracks; ++t)
            {
                if (Resource->NotifyTracks[t] == Track) return t;
            }
            return -1;
        };

        TVector<int32> TrackCounts(NumTracks, 0);
        for (const FAnimationNotify& N : Resource->Notifies)
        {
            const int Row = TrackRow(N.NotifyTrack);
            if (Row >= 0) { ++TrackCounts[Row]; }
        }
        for (const FAnimationNotifyState& S : Resource->NotifyStates)
        {
            const int Row = TrackRow(S.NotifyTrack);
            if (Row >= 0) { ++TrackCounts[Row]; }
        }

        int HoveredRow = -1;
        if (bCanvasHovered && Mouse.y >= TrackAreaY0)
        {
            const int Row = (int)floorf((Mouse.y - TracksY0) / RowHeight);
            HoveredRow = (Row >= 0 && Row < NumTracks) ? Row : -1;
        }

        DrawList->AddRectFilled(CanvasPos, ImVec2(LaneX1, CanvasY1), Timeline::CanvasColor);
        DrawList->AddRectFilled(CanvasPos, ImVec2(LaneX0, CanvasY1), Timeline::HeaderColor);
        DrawList->AddRectFilled(ImVec2(CanvasPos.x, RulerY0), ImVec2(LaneX1, TrackAreaY0), Timeline::RulerColor);

        ImGui::SetCursorScreenPos(ImVec2(CanvasPos.x + 8.0f, RulerY0 + (Timeline::RulerHeight - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Button(LE_ICON_PLUS " Track"))
        {
            Resource->NotifyTracks.push_back(FName(Format("Track {}", NumTracks).c_str()));
            MarkAnimationDirty();
        }
        ImGuiX::TextTooltip("Add a notify track. Right-click a track header to rename, sync or delete it.");

        // Track headers + lane rows
        ImGui::PushClipRect(ImVec2(CanvasPos.x, TrackAreaY0), ImVec2(LaneX1, CanvasY1), true);
        for (int t = 0; t < NumTracks; ++t)
        {
            const float RowY0 = TracksY0 + t * RowHeight;
            const float RowY1 = RowY0 + RowHeight;
            if (RowY0 > CanvasY1) break;
            if (RowY1 <= TrackAreaY0) continue;

            const bool bRowSelected = (SelectedTrack == t);
            const bool bRowHovered  = (HoveredRow == t);
            const FVector4 TrackColor = Timeline::PaletteColor(t);

            DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), (t & 1) ? Timeline::RowAltColor : Timeline::RowColor);
            if (bRowSelected)
            {
                DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), Timeline::WithAlpha(TrackColor, 0.05f));
            }
            else if (bRowHovered)
            {
                DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), Timeline::RowHoverColor);
            }
            DrawList->AddLine(ImVec2(CanvasPos.x, RowY1 - 0.5f), ImVec2(LaneX1, RowY1 - 0.5f), Timeline::RowLineColor);

            const bool bIsSyncTrack = !Resource->SyncTrackName.IsNone() && Resource->NotifyTracks[t] == Resource->SyncTrackName;
            Timeline::DrawTrackHeader(DrawList, ImVec2(CanvasPos.x, RowY0), ImVec2(LaneX0, RowY1), TrackColor,
                                      bIsSyncTrack ? LE_ICON_SYNC : nullptr, Resource->NotifyTracks[t].c_str(), TrackCounts[t], bRowSelected, bRowHovered);

            ImGui::SetCursorScreenPos(ImVec2(CanvasPos.x, RowY0));
            ImGui::PushID(t);
            if (ImGui::InvisibleButton("##TrackHeader", ImVec2(Timeline::HeaderWidth, RowHeight)))
            {
                SelectedTrack = t;
            }

            if (ImGui::BeginPopupContextItem("track_ctx"))
            {
                static char RenameBuf[128];
                if (ImGui::IsWindowAppearing())
                {
                    snprintf(RenameBuf, sizeof(RenameBuf), "%s", Resource->NotifyTracks[t].c_str());
                }
                ImGui::SetNextItemWidth(160);
                if (ImGui::InputText("Name", RenameBuf, sizeof(RenameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    // A lane owns its entries by name, so they have to be re-tagged or they orphan.
                    const FName PreviousName = Resource->NotifyTracks[t];
                    const FName NewName = FName(RenameBuf);

                    Resource->NotifyTracks[t] = NewName;
                    for (FAnimationNotify& N : Resource->Notifies)
                    {
                        if (N.NotifyTrack == PreviousName) { N.NotifyTrack = NewName; }
                    }
                    for (FAnimationNotifyState& N : Resource->NotifyStates)
                    {
                        if (N.NotifyTrack == PreviousName) { N.NotifyTrack = NewName; }
                    }
                    if (bIsSyncTrack)
                    {
                        Resource->SyncTrackName = NewName;
                    }
                    MarkAnimationDirty();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(LE_ICON_SYNC " Sync Track", nullptr, bIsSyncTrack))
                {
                    // One lane at a time, since a clip has a single cycle to line blends up on.
                    Resource->SyncTrackName = bIsSyncTrack ? FName() : Resource->NotifyTracks[t];
                    MarkAnimationDirty();
                }
                if (bIsSyncTrack)
                {
                    ImGui::TextDisabled("%d sync event(s)", Animation->GetSyncTrack().NumEvents());
                }
                ImGui::Separator();
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Notify"))      { AddNotifyAt(Resource, Resource->NotifyTracks[t], AnimComp->CurrentTime, false); }
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Notify State")) { AddNotifyAt(Resource, Resource->NotifyTracks[t], AnimComp->CurrentTime, true); }
                ImGui::Separator();
                if (NumTracks > 1 && ImGui::MenuItem(LE_ICON_DELETE " Delete Track"))
                {
                    if (bIsSyncTrack)
                    {
                        Resource->SyncTrackName = FName();
                    }
                    const FName Removed = Resource->NotifyTracks[t];
                    Resource->NotifyTracks.erase(Resource->NotifyTracks.begin() + t);
                    Resource->Notifies.erase(Algo::RemoveIf(Resource->Notifies, [&](const FAnimationNotify& N) { return N.NotifyTrack == Removed; }), Resource->Notifies.end());
                    Resource->NotifyStates.erase(Algo::RemoveIf(Resource->NotifyStates, [&](const FAnimationNotifyState& S) { return S.NotifyTrack == Removed; }), Resource->NotifyStates.end());
                    ClearSelection();
                    MarkAnimationDirty();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // Empty-lane right-click -> add a notify at the clicked time.
            const ImRect LaneRect(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1));
            if (bLanesHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && LaneRect.Contains(Mouse))
            {
                SelectedTrack = t;
                LaneAddTime = SnapTime(View.XToTime(Mouse.x), Duration);
                ImGui::OpenPopup("lane_add_ctx");
            }
        }
        ImGui::PopClipRect();

        // Lane add popup (uses cursor time captured on open).
        if (ImGui::BeginPopup("lane_add_ctx"))
        {
            const float ClickT = LaneAddTime;
            ImGui::TextDisabled("at %.3fs (frame %d)", ClickT, (int)roundf(ClickT * (float)FrameRate));
            ImGui::Separator();
            if (SelectedTrack >= 0 && SelectedTrack < NumTracks)
            {
                if (ImGui::MenuItem(LE_ICON_BELL " Add Notify"))
                {
                    AddNotifyAt(Resource, Resource->NotifyTracks[SelectedTrack], ClickT, false);
                }
                if (ImGui::MenuItem(LE_ICON_BELL_OUTLINE " Add Notify State"))
                {
                    AddNotifyAt(Resource, Resource->NotifyTracks[SelectedTrack], ClickT, true);
                }
            }
            ImGui::EndPopup();
        }

        Timeline::DrawRuler(DrawList, View, RulerY0, TrackAreaY0, CanvasY1, Duration, FrameRate);

        DrawList->PushClipRect(ImVec2(LaneX0, TrackAreaY0), ImVec2(LaneX1, CanvasY1), true);
        bool bItemHovered = false;

        // Decay flash timers.
        for (float& F : NotifyFlash) { F = Math::Max(0.0f, F - IO.DeltaTime * 3.0f); }

        // Detect playhead crossings during preview to flash point notifies.
        if (bIsPlaying && AnimComp->CurrentTime != LastPlayheadTime)
        {
            const bool bWrapped = AnimComp->CurrentTime < LastPlayheadTime;
            for (int i = 0; i < (int)Resource->Notifies.size() && i < IM_ARRAYSIZE(NotifyFlash); ++i)
            {
                const float Tm = Resource->Notifies[i].Time;
                const bool bCrossed = bWrapped
                    ? (Tm > LastPlayheadTime || Tm <= AnimComp->CurrentTime)
                    : (Tm > LastPlayheadTime && Tm <= AnimComp->CurrentTime);
                if (bCrossed) { NotifyFlash[i] = 1.0f; }
            }
        }
        LastPlayheadTime = AnimComp->CurrentTime;

        TVector<TVector<ImVec2>> RowLabels(NumTracks);
        TVector<TVector<ImVec2>> RowBars(NumTracks);

        // Notify states (draw bars first, behind point markers)
        for (int i = 0; i < (int)Resource->NotifyStates.size(); ++i)
        {
            FAnimationNotifyState& State = Resource->NotifyStates[i];
            const int Row = TrackRow(State.NotifyTrack);
            if (Row < 0) continue;

            const float RowY0 = TracksY0 + Row * RowHeight;
            const float RowY1 = RowY0 + RowHeight;
            if (RowY1 <= TrackAreaY0 || RowY0 >= CanvasY1) continue;

            const float X0 = View.TimeToX(State.StartTime);
            const float X1 = Math::Max(View.TimeToX(State.EndTime), X0 + 4.0f);
            const Timeline::FRangeGeometry Bar = Timeline::GetRangeGeometry(X0, X1, RowY0, RowY1, State.NotifyName.c_str());
            RowLabels[Row].push_back(Timeline::RangeLabelSpan(Bar, X0, X1, State.NotifyName.c_str()));
            RowBars[Row].push_back(ImVec2(X0, X1));
            const bool bOverTag = !Bar.bNameInside && Mouse.x >= Bar.Tag0.x && Mouse.x <= Bar.Tag1.x && Mouse.y >= Bar.Tag0.y && Mouse.y <= Bar.Tag1.y;

            const bool bOverLeft  = bLanesHovered && fabsf(Mouse.x - X0) <= 5.0f && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1;
            const bool bOverRight = bLanesHovered && fabsf(Mouse.x - X1) <= 5.0f && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1;
            const bool bOverBody  = bLanesHovered && ((Mouse.x >= X0 && Mouse.x <= X1 && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1) || bOverTag);
            const bool bHover     = bOverBody || bOverLeft || bOverRight;
            const bool bSel       = (SelectedKind == ENotifyKind::State && SelectedIndex == i);
            const bool bDragged   = (DragKind == ENotifyKind::State && DragIndex == i);
            bItemHovered |= bHover;

            char DurationText[24];
            snprintf(DurationText, sizeof(DurationText), "%.2fs", State.EndTime - State.StartTime);
            Timeline::DrawRange(DrawList, Bar, X0, X1, State.Color, State.NotifyName.c_str(), DurationText, bSel, bHover,
                                bOverLeft || (bDragged && DragMode == EDragMode::ResizeStart),
                                bOverRight || (bDragged && DragMode == EDragMode::ResizeEnd));

            if (DragMode == EDragMode::None && (bOverLeft || bOverRight))
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            else if (DragMode == EDragMode::None && bOverBody)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            if (DragMode == EDragMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && bHover)
            {
                SelectedKind = ENotifyKind::State;
                SelectedIndex = i;
                SelectedTrack = Row;
                DragKind = ENotifyKind::State;
                DragIndex = i;
                DragMode = bOverLeft ? EDragMode::ResizeStart : (bOverRight ? EDragMode::ResizeEnd : EDragMode::MoveItem);
                DragGrabOffset = View.XToTime(Mouse.x) - State.StartTime;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && bOverBody)
            {
                SelectedKind = ENotifyKind::State; SelectedIndex = i;
                ImGui::OpenPopup("notify_item_ctx");
            }
        }

        // Point notifies (diamonds with a name tag)
        for (int i = 0; i < (int)Resource->Notifies.size(); ++i)
        {
            FAnimationNotify& Notify = Resource->Notifies[i];
            const int Row = TrackRow(Notify.NotifyTrack);
            if (Row < 0) continue;

            const float RowY0 = TracksY0 + Row * RowHeight;
            const float RowY1 = RowY0 + RowHeight;
            if (RowY1 <= TrackAreaY0 || RowY0 >= CanvasY1) continue;

            const float X = View.TimeToX(Notify.Time);
            const Timeline::FPointGeometry Point = Timeline::GetPointGeometry(X, RowY0, RowY1, Notify.NotifyName.c_str(), Timeline::Overlaps(RowBars[Row], X - 1.0f, X + 1.0f));
            const bool bSel  = (SelectedKind == ENotifyKind::Point && SelectedIndex == i);
            const bool bShowTag = bSel || !Timeline::Overlaps(RowLabels[Row], Point.Tag0.x, Point.Tag1.x);
            if (bShowTag)
            {
                RowLabels[Row].push_back(ImVec2(Point.Tag0.x, Point.Tag1.x));
            }
            const bool bOver = bLanesHovered && Timeline::HitPoint(Point, X, Mouse, bShowTag);
            bItemHovered |= bOver;

            const float Flash = (i < IM_ARRAYSIZE(NotifyFlash)) ? NotifyFlash[i] : 0.0f;
            Timeline::DrawPoint(DrawList, Point, X, RowY0, RowY1, Notify.Color, Notify.NotifyName.c_str(), bSel, bOver, Flash, bShowTag);

            if (bOver && DragMode == EDragMode::None)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s  %.3fs (frame %d)", Notify.NotifyName.c_str(), Notify.Time, (int)roundf(Notify.Time * (float)FrameRate));
            }
            if (DragMode == EDragMode::None && bOver && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedKind = ENotifyKind::Point; SelectedIndex = i; SelectedTrack = Row;
                DragKind = ENotifyKind::Point; DragIndex = i; DragMode = EDragMode::MoveItem;
                DragGrabOffset = View.XToTime(Mouse.x) - Notify.Time;
            }
            if (bOver && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedKind = ENotifyKind::Point; SelectedIndex = i;
                ImGui::OpenPopup("notify_item_ctx");
            }
        }

        if (HoveredRow >= 0 && bLanesHovered && !bItemHovered && DragMode == EDragMode::None && TrackCounts[HoveredRow] == 0)
        {
            const char* Hint = "Double-click to add a notify, right-click for more";
            const ImVec2 HintSize = ImGui::CalcTextSize(Hint);
            const float RowY0 = TracksY0 + HoveredRow * RowHeight;
            DrawList->AddText(ImVec2(LaneX0 + (LaneX1 - LaneX0 - HintSize.x) * 0.5f, RowY0 + (RowHeight - HintSize.y) * 0.5f), Timeline::MutedTextColor, Hint);
        }

        DrawList->PopClipRect();

        Timeline::DrawPlayhead(DrawList, View, RulerY0, TrackAreaY0, CanvasY1, AnimComp->CurrentTime, FrameRate);

        DrawList->AddLine(ImVec2(CanvasPos.x, TrackAreaY0 - 0.5f), ImVec2(LaneX1, TrackAreaY0 - 0.5f), Timeline::SeparatorColor);
        DrawList->AddLine(ImVec2(LaneX0 - 0.5f, RulerY0), ImVec2(LaneX0 - 0.5f, CanvasY1), Timeline::SeparatorColor);

        if (MaxTrackScroll > 0.0f)
        {
            const float AreaH  = CanvasY1 - TrackAreaY0;
            const float ThumbH = Math::Max(AreaH * AreaH / ((float)NumTracks * RowHeight), 16.0f);
            const float ThumbY = TrackAreaY0 + (TrackScroll / MaxTrackScroll) * (AreaH - ThumbH);
            DrawList->AddRectFilled(ImVec2(LaneX1 - 6.0f, ThumbY + 2.0f), ImVec2(LaneX1 - 2.0f, ThumbY + ThumbH - 2.0f), IM_COL32(255, 255, 255, 55), 2.0f);
        }

        // Drag handling
        const ImRect RulerRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, TrackAreaY0));
        if (DragMode == EDragMode::None && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && RulerRect.Contains(Mouse))
        {
            DragMode = EDragMode::Playhead;
        }

        // Every item and the ruler set a DragMode, so an unclaimed press landed on empty canvas.
        if (DragMode == EDragMode::None && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        {
            ClearSelection();
            if (HoveredRow >= 0)
            {
                SelectedTrack = HoveredRow;
            }
        }

        if (DragMode == EDragMode::None && bLanesHovered && !bItemHovered && HoveredRow >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            SelectedTrack = HoveredRow;
            AddNotifyAt(Resource, Resource->NotifyTracks[HoveredRow], SnapTime(View.XToTime(Mouse.x), Duration), false);
        }

        if (DragMode != EDragMode::None)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const float RawT = View.XToTime(Mouse.x);
                const float T = SnapTime(RawT, Duration);
                const bool bMoved = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
                if (DragMode == EDragMode::ResizeStart || DragMode == EDragMode::ResizeEnd)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }

                if (DragMode == EDragMode::Playhead)
                {
                    bIsPlaying = false;
                    AnimComp->bPlaying = false;
                    AnimComp->CurrentTime = T;
                    AnimComp->bDirty = true;
                }
                else if (bMoved && DragKind == ENotifyKind::Point && DragIndex >= 0 && DragIndex < (int)Resource->Notifies.size())
                {
                    FAnimationNotify& N = Resource->Notifies[DragIndex];
                    N.Time = SnapTime(RawT - DragGrabOffset, Duration);
                    MarkAnimationDirty();
                    ImGui::SetTooltip("%.3fs (frame %d)", N.Time, (int)roundf(N.Time * (float)FrameRate));
                }
                else if (bMoved && DragKind == ENotifyKind::State && DragIndex >= 0 && DragIndex < (int)Resource->NotifyStates.size())
                {
                    FAnimationNotifyState& S = Resource->NotifyStates[DragIndex];
                    if (DragMode == EDragMode::ResizeStart)      { S.StartTime = Math::Min(T, S.EndTime - 0.001f); }
                    else if (DragMode == EDragMode::ResizeEnd)   { S.EndTime   = Math::Max(T, S.StartTime + 0.001f); }
                    else
                    {
                        const float Len = S.EndTime - S.StartTime;
                        S.StartTime = Math::Clamp(SnapTime(RawT - DragGrabOffset, Duration), 0.0f, Math::Max(Duration - Len, 0.0f));
                        S.EndTime   = S.StartTime + Len;
                    }
                    MarkAnimationDirty();
                    ImGui::SetTooltip("%.3fs to %.3fs (%.3fs)", S.StartTime, S.EndTime, S.EndTime - S.StartTime);
                }
            }
            else
            {
                DragMode = EDragMode::None;
                DragKind = ENotifyKind::None;
                DragIndex = -1;
            }
        }

        // Shared item context menu
        if (ImGui::BeginPopup("notify_item_ctx"))
        {
            if (ImGui::MenuItem(LE_ICON_MAP_MARKER " Move to playhead"))
            {
                if (SelectedKind == ENotifyKind::Point && SelectedIndex >= 0 && SelectedIndex < (int)Resource->Notifies.size())
                {
                    Resource->Notifies[SelectedIndex].Time = AnimComp->CurrentTime;
                }
                else if (SelectedKind == ENotifyKind::State && SelectedIndex >= 0 && SelectedIndex < (int)Resource->NotifyStates.size())
                {
                    FAnimationNotifyState& S = Resource->NotifyStates[SelectedIndex];
                    const float Len = S.EndTime - S.StartTime;
                    S.StartTime = Math::Clamp(AnimComp->CurrentTime, 0.0f, Duration - Len);
                    S.EndTime = S.StartTime + Len;
                }
                MarkAnimationDirty();
            }
            if (ImGui::MenuItem(LE_ICON_DELETE " Delete"))
            {
                DeleteSelected(Resource);
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();
    }

    void FAnimationEditorTool::DrawNotifyInspector(CAnimation* Animation)
    {
        FAnimationResource* Resource = Animation->GetAnimationResource();
        const float Duration = Math::Max(Animation->GetDuration(), 0.0001f);

        char NameBuf[128];
        const float FieldWidth = Math::Max(ImGui::GetContentRegionAvail().x - 90.0f, 120.0f);

        auto TrackCombo = [&](FName& Track)
        {
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::BeginCombo("Track", Track.c_str()))
            {
                for (const FName& Lane : Resource->NotifyTracks)
                {
                    if (ImGui::Selectable(Lane.c_str(), Lane == Track))
                    {
                        Track = Lane;
                        MarkAnimationDirty();
                    }
                }
                ImGui::EndCombo();
            }
        };

        auto NameField = [&](FName& Name, const char* StrId)
        {
            snprintf(NameBuf, sizeof(NameBuf), "%s", Name.c_str());
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::InputText(StrId, NameBuf, sizeof(NameBuf)))
            {
                Name = FName(NameBuf);
                MarkAnimationDirty();
            }
        };

        auto ColorField = [&](FVector4& Color)
        {
            ImVec4 Col(Color.x, Color.y, Color.z, Color.w);
            if (ImGui::ColorEdit4("Color", &Col.x, ImGuiColorEditFlags_NoInputs))
            {
                Color = FVector4(Col.x, Col.y, Col.z, Col.w);
                MarkAnimationDirty();
            }
        };

        if (SelectedKind == ENotifyKind::Point && SelectedIndex >= 0 && SelectedIndex < (int)Resource->Notifies.size())
        {
            FAnimationNotify& N = Resource->Notifies[SelectedIndex];

            ImGui::TextDisabled(LE_ICON_BELL " Notify");
            ImGui::Spacing();

            NameField(N.NotifyName, "Name");
            TrackCombo(N.NotifyTrack);

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Time", &N.Time, 0.005f, 0.0f, Duration, "%.3fs")) { MarkAnimationDirty(); }

            ColorField(N.Color);

            ImGui::Spacing();
            ImGui::SeparatorText("Notify Type");
            if (DrawInstancedStructEditor("Type", N.Notify, SAnimNotify::StaticStruct(), NotifyDetails))
            {
                MarkAnimationDirty();
            }

            ImGui::Spacing();
            if (ImGui::Button(LE_ICON_DELETE " Delete")) { DeleteSelected(Resource); }
        }
        else if (SelectedKind == ENotifyKind::State && SelectedIndex >= 0 && SelectedIndex < (int)Resource->NotifyStates.size())
        {
            FAnimationNotifyState& S = Resource->NotifyStates[SelectedIndex];

            ImGui::TextDisabled(LE_ICON_BELL_OUTLINE " Notify State");
            ImGui::Spacing();

            NameField(S.NotifyName, "Name");
            TrackCombo(S.NotifyTrack);

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Start", &S.StartTime, 0.005f, 0.0f, S.EndTime - 0.001f, "%.3fs")) { MarkAnimationDirty(); }
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("End", &S.EndTime, 0.005f, S.StartTime + 0.001f, Duration, "%.3fs")) { MarkAnimationDirty(); }

            ColorField(S.Color);

            ImGui::Spacing();
            ImGui::SeparatorText("Notify Type");
            if (DrawInstancedStructEditor("Type", S.Notify, SAnimNotifyState::StaticStruct(), NotifyDetails))
            {
                MarkAnimationDirty();
            }

            ImGui::Spacing();
            if (ImGui::Button(LE_ICON_DELETE " Delete")) { DeleteSelected(Resource); }
        }
        else
        {
            // The selection outlived the notify it named (deleted, or the clip reloaded).
            ClearSelection();
        }
    }

    void FAnimationEditorTool::AddCurve(FAnimationResource* Resource)
    {
        FAnimationCurve Curve;
        Curve.Name  = FName(Format("Curve {}", (int)Resource->Curves.size()).c_str());
        Curve.Color = Timeline::PaletteColor((int32)Resource->Curves.size());
        Resource->Curves.push_back(Curve);

        SelectedCurve = (int)Resource->Curves.size() - 1;
        MarkAnimationDirty();
    }

    void FAnimationEditorTool::DeleteCurve(FAnimationResource* Resource, int32 Index)
    {
        if (Index < 0 || Index >= (int32)Resource->Curves.size())
        {
            return;
        }

        Resource->Curves.erase(Resource->Curves.begin() + Index);
        SelectedCurve = Math::Min(SelectedCurve, (int)Resource->Curves.size() - 1);
        CurveWidget.SetCurve(nullptr);
        MarkAnimationDirty();
    }

    void FAnimationEditorTool::DrawCurvesTab(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration)
    {
        if (ImGui::BeginTabBar("##CurveTabs", ImGuiTabBarFlags_None))
        {
            if (ImGui::BeginTabItem(LE_ICON_CHART_LINE " Float Curves"))
            {
                DrawFloatCurves(Animation, AnimComp, Duration);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(LE_ICON_BONE " Bone Channels"))
            {
                DrawBoneChannels(Animation, AnimComp, Duration);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    void FAnimationEditorTool::DrawFloatCurves(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration)
    {
        FAnimationResource* Resource = Animation->GetAnimationResource();
        const float PlayheadTime = SnapTime(AnimComp->CurrentTime, Duration);
        const float FontSize = ImGui::GetFontSize();

        if (SelectedCurve < 0 && !Resource->Curves.empty())
        {
            SelectedCurve = 0;
        }

        ImGui::BeginChild("CurveList", ImVec2(Timeline::HeaderWidth, 0), true);
        if (ImGui::Button(LE_ICON_PLUS " Curve", ImVec2(-1, 0)))
        {
            AddCurve(Resource);
        }
        ImGui::Spacing();

        ImDrawList* ListDrawList = ImGui::GetWindowDrawList();
        constexpr float RowHeight = 28.0f;

        for (int i = 0; i < (int)Resource->Curves.size(); ++i)
        {
            FAnimationCurve& Curve = Resource->Curves[i];
            ImGui::PushID(i);

            const ImVec2 RowPos = ImGui::GetCursorScreenPos();
            const float RowWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##CurveRow", SelectedCurve == i, ImGuiSelectableFlags_None, ImVec2(RowWidth, RowHeight)))
            {
                SelectedCurve = i;
            }

            if (ImGui::BeginPopupContextItem("curve_ctx"))
            {
                SelectedCurve = i;

                static char RenameBuf[128];
                if (ImGui::IsWindowAppearing())
                {
                    snprintf(RenameBuf, sizeof(RenameBuf), "%s", Curve.Name.c_str());
                }
                ImGui::SetNextItemWidth(180);
                if (ImGui::InputText("Name", RenameBuf, sizeof(RenameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    Curve.Name = FName(RenameBuf);
                    MarkAnimationDirty();
                }

                ImVec4 Color(Curve.Color.x, Curve.Color.y, Curve.Color.z, Curve.Color.w);
                if (ImGui::ColorEdit4("Color", &Color.x, ImGuiColorEditFlags_NoInputs))
                {
                    Curve.Color = FVector4(Color.x, Color.y, Color.z, Color.w);
                    MarkAnimationDirty();
                }
                ImGui::Separator();

                if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Duplicate"))
                {
                    FAnimationCurve Copy = Curve;
                    Copy.Name = FName((FString(Curve.Name.c_str()) + " Copy").c_str());
                    Resource->Curves.push_back(Copy);
                    SelectedCurve = (int)Resource->Curves.size() - 1;
                    MarkAnimationDirty();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }

                if (ImGui::MenuItem(LE_ICON_DELETE " Delete"))
                {
                    DeleteCurve(Resource, i);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndPopup();
            }

            ListDrawList->AddRectFilled(ImVec2(RowPos.x + 2.0f, RowPos.y + 6.0f), ImVec2(RowPos.x + 6.0f, RowPos.y + RowHeight - 6.0f), Timeline::ToU32(Curve.Color), 2.0f);

            char ValueText[32];
            if (Curve.Curve.IsEmpty())
            {
                snprintf(ValueText, sizeof(ValueText), "%s", "no keys");
            }
            else
            {
                snprintf(ValueText, sizeof(ValueText), "%.2f", Curve.Curve.Evaluate(PlayheadTime));
            }
            const float ValueW = ImGui::CalcTextSize(ValueText).x;
            const float TextY = RowPos.y + (RowHeight - FontSize) * 0.5f;
            ListDrawList->AddText(ImVec2(RowPos.x + RowWidth - ValueW - 6.0f, TextY), Timeline::MutedTextColor, ValueText);

            ListDrawList->PushClipRect(ImVec2(RowPos.x + 14.0f, RowPos.y), ImVec2(RowPos.x + RowWidth - ValueW - 12.0f, RowPos.y + RowHeight), true);
            ListDrawList->AddText(ImVec2(RowPos.x + 14.0f, TextY), SelectedCurve == i ? Timeline::ItemTextColor : Timeline::TrackTextColor, Curve.Name.c_str());
            ListDrawList->PopClipRect();

            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("CurvePanel", ImVec2(0, 0), true);
        if (SelectedCurve < 0 || SelectedCurve >= (int)Resource->Curves.size())
        {
            CurveWidget.SetCurve(nullptr);

            const char* Title = Resource->Curves.empty() ? "No float curves yet" : "Select a curve to edit its keys";
            const char* Body  = "Float curves are named values that play with the clip. Read one with a Get Curve node, or GetCurveValue in script.";
            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const float WrapWidth = Math::Min(Avail.x - 40.0f, 420.0f);
            const ImVec2 TitleSize = ImGui::CalcTextSize(Title);
            const ImVec2 BodySize = ImGui::CalcTextSize(Body, nullptr, false, WrapWidth);
            const float BlockHeight = TitleSize.y + BodySize.y + ImGui::GetFrameHeight() + 20.0f;
            const ImVec2 Origin = ImGui::GetCursorPos();

            ImGui::SetCursorPos(ImVec2(Origin.x + (Avail.x - TitleSize.x) * 0.5f, Origin.y + Math::Max((Avail.y - BlockHeight) * 0.5f, 0.0f)));
            ImGui::TextUnformatted(Title);
            ImGui::SetCursorPosX(Origin.x + (Avail.x - WrapWidth) * 0.5f);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + WrapWidth);
            ImGui::TextDisabled("%s", Body);
            ImGui::PopTextWrapPos();

            if (Resource->Curves.empty())
            {
                ImGui::Spacing();
                const float ButtonWidth = ImGui::CalcTextSize(LE_ICON_PLUS " Add Curve").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetCursorPosX(Origin.x + (Avail.x - ButtonWidth) * 0.5f);
                if (ImGui::Button(LE_ICON_PLUS " Add Curve"))
                {
                    AddCurve(Resource);
                }
            }

            ImGui::EndChild();
            return;
        }

        const int32 CurveIndex = SelectedCurve;
        CurveWidget.SetCurve(&Resource->Curves[CurveIndex].Curve);
        CurveWidget.SetTimeRange(0.0f, Duration);
        CurveWidget.SetOnModified([this]() { MarkAnimationDirty(); });
        CurveWidget.SetTimeMarker(AnimComp->CurrentTime);
        CurveWidget.SetToolbarExtension([this, Resource, CurveIndex, PlayheadTime]()
        {
            if (CurveIndex < 0 || CurveIndex >= (int32)Resource->Curves.size())
            {
                return;
            }

            SKeyedCurve& Keys = Resource->Curves[CurveIndex].Curve;
            float Value = Keys.Evaluate(PlayheadTime);

            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("##CurveValue", &Value, 0.01f, 0.0f, 0.0f, "%.3f"))
            {
                Keys.UpdateOrAddKey(PlayheadTime, Value);
                Keys.ComputeAutoTangents();
                MarkAnimationDirty();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                CurveWidget.FrameAll();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("Value at the playhead. Editing it keys the curve at %.3fs.", PlayheadTime);
            }

            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_PLUS " Key"))
            {
                Keys.UpdateOrAddKey(PlayheadTime, Value);
                Keys.ComputeAutoTangents();
                MarkAnimationDirty();
                CurveWidget.FrameAll();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("Key the current value at the playhead (%.3fs)", PlayheadTime);
            }
        });
        CurveWidget.Draw("AnimCurve");

        ImGui::EndChild();
    }

    void FAnimationEditorTool::DrawBoneChannels(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration)
    {
        const FCompressedAnimData& Compressed = Animation->GetAnimationResource()->Compressed;

        static const char* PathNames[NumTrackKinds] = { "Translation", "Rotation", "Scale" };
        static const char* PathIcons[NumTrackKinds] =
        {
            LE_ICON_AXIS_ARROW, LE_ICON_ROTATE_360, LE_ICON_ARROW_TOP_RIGHT_BOTTOM_LEFT
        };

        auto TrackFor = [&Compressed](int32 Bone, int32 Kind) -> const FCompressedAnimTrack*
        {
            if (Bone < 0 || Bone >= (int32)Compressed.Bones.size())
            {
                return nullptr;
            }

            switch (Kind)
            {
            case 0: return &Compressed.Bones[Bone].Translation;
            case 1: return &Compressed.Bones[Bone].Rotation;
            case 2: return &Compressed.Bones[Bone].Scale;
            default: return nullptr;
            }
        };

        ImGui::BeginChild("CurveChannels", ImVec2(Timeline::HeaderWidth, 0), true);
        for (int32 b = 0; b < (int32)Compressed.Bones.size(); ++b)
        {
            for (int32 Kind = 0; Kind < NumTrackKinds; ++Kind)
            {
                const FCompressedAnimTrack* Track = TrackFor(b, Kind);
                if (Track->Format == EAnimTrackFormat::None)
                {
                    continue;
                }

                const int32 Id = b * NumTrackKinds + Kind;
                ImGui::PushID(Id);
                const ImVec2 RowPos = ImGui::GetCursorScreenPos();
                const float RowWidth = ImGui::GetContentRegionAvail().x;
                constexpr float RowHeight = 28.0f;
                if (ImGui::Selectable("##Channel", SelectedChannel == Id, ImGuiSelectableFlags_None, ImVec2(RowWidth, RowHeight)))
                {
                    SelectedChannel = Id;
                }

                ImDrawList* ListDrawList = ImGui::GetWindowDrawList();
                const float TextY = RowPos.y + (RowHeight - ImGui::GetFontSize()) * 0.5f;
                const FFixedString KindLabel = Track->Format == EAnimTrackFormat::Constant
                    ? FormatAs<FFixedString>("{} (const)", PathNames[Kind]) : FFixedString(PathNames[Kind]);
                const float KindW = ImGui::CalcTextSize(KindLabel.c_str()).x;
                ListDrawList->AddText(ImVec2(RowPos.x + RowWidth - KindW - 6.0f, TextY), Timeline::MutedTextColor, KindLabel.c_str());

                const FFixedString Label = FormatAs<FFixedString>("{} {}", PathIcons[Kind], Compressed.Bones[b].BoneName.c_str());
                ListDrawList->PushClipRect(ImVec2(RowPos.x + 6.0f, RowPos.y), ImVec2(RowPos.x + RowWidth - KindW - 12.0f, RowPos.y + RowHeight), true);
                ListDrawList->AddText(ImVec2(RowPos.x + 6.0f, TextY), SelectedChannel == Id ? Timeline::ItemTextColor : Timeline::TrackTextColor, Label.c_str());
                ListDrawList->PopClipRect();
                ImGui::PopID();
            }
        }
        if (Compressed.Bones.empty())
        {
            ImGui::TextDisabled("No bone channels.");
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("CurvePlot", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const int32 SelectedBone = SelectedChannel / NumTrackKinds;
        const int32 SelectedTrackKind = SelectedChannel % NumTrackKinds;
        const FCompressedAnimTrack* Track = SelectedChannel >= 0 ? TrackFor(SelectedBone, SelectedTrackKind) : nullptr;

        if (Track != nullptr && Track->Format != EAnimTrackFormat::None)
        {
            ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0.0f, 0.12f));
            if (ImPlot::BeginPlot("##AnimCurves", ImVec2(-1, -1), ImPlotFlags_NoTitle))
            {
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, Duration, ImGuiCond_Always);
                ImPlot::SetupAxis(ImAxis_X1, nullptr);
                ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);

                const float TimeBefore = AnimComp->CurrentTime;
                double CurrentTimeDouble = AnimComp->CurrentTime;
                ImPlot::DragLineX(0, &CurrentTimeDouble, ImGui::ColorConvertU32ToFloat4(Timeline::PlayheadColor), 1.5f);
                AnimComp->CurrentTime = Math::Clamp((float)CurrentTimeDouble, 0.0f, Duration);

                // The editor world is paused, so the animation system only re-evaluates when bDirty is set.
                if (AnimComp->CurrentTime != TimeBefore)
                {
                    bIsPlaying = false;
                    AnimComp->bDirty = true;
                }

                const uint32 NumFrames = Math::Max(1u, Compressed.NumFrames);
                const float FrameStep = NumFrames > 1 ? Duration / (float)(NumFrames - 1) : 0.0f;

                TVector<float> Times, XVals, YVals, ZVals;
                Times.reserve(NumFrames);
                XVals.reserve(NumFrames);
                YVals.reserve(NumFrames);
                ZVals.reserve(NumFrames);

                for (uint32 Frame = 0; Frame < NumFrames; ++Frame)
                {
                    Times.push_back((float)Frame * FrameStep);

                    FVector3 Value;
                    if (SelectedTrackKind == 1)
                    {
                        Value = Math::Degrees(Math::EulerAngles(Compressed.DecodeRotation(*Track, Frame, Frame, 0.0f)));
                    }
                    else if (SelectedTrackKind == 0)
                    {
                        Value = Compressed.DecodeTranslation(*Track, Frame, Frame, 0.0f);
                    }
                    else
                    {
                        Value = Compressed.DecodeScale(*Track, Frame, Frame, 0.0f);
                    }

                    XVals.push_back(Value.x);
                    YVals.push_back(Value.y);
                    ZVals.push_back(Value.z);
                }

                const char* Suffix = SelectedTrackKind == 1 ? " (deg)" : "";
                ImPlot::PlotLine(FormatAs<FFixedString>("X{}", Suffix).c_str(),
                                 Times.data(), XVals.data(), (int)NumFrames);
                ImPlot::PlotLine(FormatAs<FFixedString>("Y{}", Suffix).c_str(),
                                 Times.data(), YVals.data(), (int)NumFrames);
                ImPlot::PlotLine(FormatAs<FFixedString>("Z{}", Suffix).c_str(),
                                 Times.data(), ZVals.data(), (int)NumFrames);

                ImPlot::EndPlot();
            }
            ImPlot::PopStyleVar();
        }
        else
        {
            const char* Hint = "Select a channel to plot its imported keys";
            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const ImVec2 HintSize = ImGui::CalcTextSize(Hint);
            const ImVec2 Origin = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(Origin.x + (Avail.x - HintSize.x) * 0.5f, Origin.y + (Avail.y - HintSize.y) * 0.5f));
            ImGui::TextDisabled("%s", Hint);
        }
        ImGui::EndChild();
    }

    void FAnimationEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();

        // The selection is an index into notify arrays a restore may have shortened.
        ClearSelection();

        if (SSimpleAnimationComponent* AnimComp = GetPreviewComponent())
        {
            AnimComp->bDirty = true;
        }
    }

    void FAnimationEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();

        ClearSelection();
        if (CAnimation* Animation = GetAsset<CAnimation>())
        {
            EnsureNotifyTracks(Animation->GetAnimationResource());
        }
        if (World.IsValid())
        {
            CreatePreviewEntity();
        }
    }
}
