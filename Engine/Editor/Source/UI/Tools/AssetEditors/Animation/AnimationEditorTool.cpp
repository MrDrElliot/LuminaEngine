#include "AnimationEditorTool.h"

#include "Core/Math/Math.h"
#include <imgui_internal.h>

#include "ImGuiDrawUtils.h"
#include "implot.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
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
    static const char* MeshPropertiesName       = "MeshProperties";
    static const char* SequencerName            = "Sequencer";

    namespace
    {
        // Layout constants for the notify timeline.
        constexpr float kHeaderWidth = 168.0f;
        constexpr float kRulerHeight = 26.0f;
        constexpr float kTrackHeight = 28.0f;

        // A palette so new tracks/notifies get distinct, readable colors.
        const FVector4 kPalette[] =
        {
            { 0.93f, 0.42f, 0.36f, 1.0f }, // red
            { 0.40f, 0.75f, 0.95f, 1.0f }, // blue
            { 0.55f, 0.85f, 0.45f, 1.0f }, // green
            { 0.95f, 0.78f, 0.35f, 1.0f }, // amber
            { 0.78f, 0.55f, 0.92f, 1.0f }, // purple
            { 0.40f, 0.88f, 0.80f, 1.0f }, // teal
        };

        ImU32 ToU32(const FVector4& C)
        {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(C.x, C.y, C.z, C.w));
        }

        FVector4 PaletteColor(int32 Index)
        {
            return kPalette[((Index % (int32)IM_ARRAYSIZE(kPalette)) + IM_ARRAYSIZE(kPalette)) % IM_ARRAYSIZE(kPalette)];
        }
    }

    FAnimationEditorTool::FAnimationEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FAnimationEditorTool::OnInitialize()
    {
        CreateToolWindow(MeshPropertiesName, [&](bool bFocused)
        {
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Asset Details");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();
            PropertyTable.DrawTree();
        });

        CreateToolWindow(SequencerName, [&](bool bFocused)
        {
            DrawSequencer();
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

        MeshEntity = World->ConstructEntity("MeshEntity");
        World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SetSkeletalMesh(Animation->Skeleton->PreviewMesh);
        SSimpleAnimationComponent& AnimComp = World->EmplaceComponent<SSimpleAnimationComponent>(MeshEntity);
        AnimComp.Animation = Animation;
        AnimComp.bPlaying  = false;
        AnimComp.bLooping  = bLooping;

        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        FQuat Rotation = Math::FindLookAtRotation(MeshTransform.GetLocation() + FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(Rotation);
    }

    void FAnimationEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);
        
        if (World && World->GetRenderer())
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
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

    SSimpleAnimationComponent* FAnimationEditorTool::GetPreviewComponent() const
    {
        if (!World.IsValid() || MeshEntity == entt::null)
        {
            return nullptr;
        }
        return World->TryGetComponent<SSimpleAnimationComponent>(MeshEntity);
    }

    void FAnimationEditorTool::MarkAnimationDirty()
    {
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

        // Migrate legacy clips: rebuild the lane list from whatever tracks the existing
        // notifies reference, then guarantee at least one default lane to author into.
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
            State.Color       = PaletteColor(ColorIdx);
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
            Notify.Color       = PaletteColor(ColorIdx);
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
            "Add lanes with '+ Track'. Right-click a lane to add a Notify (instant) or a "
            "Notify State (ranged). Drag markers to move them, drag a state's edges to resize. "
            "Snap to Frame quantizes edits to the frame rate.");
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
    }

    void FAnimationEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0, bottomDockID = 0;

        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.25f, &rightDockID, &leftDockID);
        ImGui::DockBuilderSplitNode(leftDockID, ImGuiDir_Down, 0.38f, &bottomDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MeshPropertiesName).c_str(), rightDockID);
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
            ImGui::TextDisabled("No preview mesh - assign a Skeleton with a Preview Mesh to scrub this clip.");
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
                DrawNotifyInspector(Animation);
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
        // The editor world is paused (system delta-time is 0), so advance the playhead
        // from the UI clock and let the animation system resample the pose via bDirty.
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

        if (ImGui::Button(bIsPlaying ? LE_ICON_PAUSE " Pause" : LE_ICON_PLAY " Play", ImVec2(96, 0)))
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
        std::string Readout = std::format("{:.3f}s / {:.3f}s   (frame {}/{})", AnimComp->CurrentTime, Duration, Frame, TotalFrame);
        const float TextW = ImGui::CalcTextSize(Readout.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(Math::Max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - TextW - 16.0f));
        ImGui::TextUnformatted(Readout.c_str());
    }

    void FAnimationEditorTool::DrawNotifyTimeline(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration)
    {
        FAnimationResource* Resource = Animation->GetAnimationResource();

        // Track header column toolbar.
        if (ImGui::Button(LE_ICON_PLUS " Track"))
        {
            Resource->NotifyTracks.push_back(FName(std::format("Track {}", (int)Resource->NotifyTracks.size()).c_str()));
            MarkAnimationDirty();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d tracks - right-click a lane to add a notify)", (int)Resource->NotifyTracks.size());

        ImGui::BeginChild("TimelineCanvas", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList*  DrawList   = ImGui::GetWindowDrawList();
        const ImVec2 CanvasPos  = ImGui::GetCursorScreenPos();
        const ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
        const int    NumTracks  = (int)Resource->NotifyTracks.size();

        const float LaneX0 = CanvasPos.x + kHeaderWidth;
        const float LaneX1 = CanvasPos.x + CanvasSize.x;
        const float LaneW  = Math::Max(LaneX1 - LaneX0, 1.0f);
        const float RulerY0 = CanvasPos.y;
        const float TracksY0 = CanvasPos.y + kRulerHeight;

        const float BasePPS = LaneW / Duration;
        const float PPS     = BasePPS * TimelineZoom;

        // Clamp pan so the clip stays reachable.
        const float MaxPan = Math::Max(0.0f, Duration - LaneW / PPS);
        TimelinePanSeconds = Math::Clamp(TimelinePanSeconds, 0.0f, MaxPan);

        auto TimeToX = [&](float T) { return LaneX0 + (T - TimelinePanSeconds) * PPS; };
        auto XToTime = [&](float X) { return TimelinePanSeconds + (X - LaneX0) / PPS; };

        const ImGuiIO& IO = ImGui::GetIO();
        const bool bCanvasHovered = ImGui::IsWindowHovered();

        // Wheel over the lanes: zoom toward the cursor; shift+wheel pans.
        if (bCanvasHovered && IO.MouseWheel != 0.0f && IO.MousePos.x > LaneX0)
        {
            if (IO.KeyShift)
            {
                TimelinePanSeconds -= IO.MouseWheel * (LaneW / PPS) * 0.15f;
            }
            else
            {
                const float CursorT = XToTime(IO.MousePos.x);
                TimelineZoom = Math::Clamp(TimelineZoom * (IO.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 1.0f, 16.0f);
                const float NewPPS = BasePPS * TimelineZoom;
                TimelinePanSeconds = CursorT - (IO.MousePos.x - LaneX0) / NewPPS;
            }
        }

        // Backgrounds.
        DrawList->AddRectFilled(CanvasPos, ImVec2(LaneX1, CanvasPos.y + CanvasSize.y), IM_COL32(24, 24, 28, 255));
        DrawList->AddRectFilled(CanvasPos, ImVec2(CanvasPos.x + kHeaderWidth, CanvasPos.y + CanvasSize.y), IM_COL32(32, 33, 38, 255));
        DrawList->AddRectFilled(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, RulerY0 + kRulerHeight), IM_COL32(40, 41, 47, 255));

        DrawList->PushClipRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, CanvasPos.y + CanvasSize.y), true);

        // Ruler ticks
        {
            // Choose a tick step that yields ~90px spacing, quantized to frames.
            float TargetSec = 90.0f / PPS;
            const float FrameSec = 1.0f / (float)FrameRate;
            float Step = FrameSec;
            while (Step < TargetSec) { Step *= (Step * 5.0f < TargetSec ? 5.0f : 2.0f); }

            const float FirstT = floorf(TimelinePanSeconds / Step) * Step;
            for (float T = FirstT; T <= XToTime(LaneX1); T += Step)
            {
                if (T < 0.0f) continue;
                const float X = TimeToX(T);
                DrawList->AddLine(ImVec2(X, RulerY0 + kRulerHeight - 7.0f), ImVec2(X, CanvasPos.y + CanvasSize.y), IM_COL32(255, 255, 255, 16));
                DrawList->AddLine(ImVec2(X, RulerY0 + kRulerHeight - 7.0f), ImVec2(X, RulerY0 + kRulerHeight), IM_COL32(200, 200, 210, 120));
                FString Label = std::format("{:.2f}", T).c_str();
                DrawList->AddText(ImVec2(X + 3.0f, RulerY0 + 3.0f), IM_COL32(180, 182, 190, 200), Label.c_str());
            }
        }

        DrawList->PopClipRect();

        // Track headers + lane rows
        for (int t = 0; t < NumTracks; ++t)
        {
            const float RowY0 = TracksY0 + t * kTrackHeight;
            const float RowY1 = RowY0 + kTrackHeight;
            if (RowY0 > CanvasPos.y + CanvasSize.y) break;

            const ImU32 RowBg = (t & 1) ? IM_COL32(28, 28, 33, 255) : IM_COL32(24, 24, 28, 255);
            DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), RowBg);
            DrawList->AddLine(ImVec2(CanvasPos.x, RowY1), ImVec2(LaneX1, RowY1), IM_COL32(0, 0, 0, 120));

            // Header: color chip + name + context menu.
            const FVector4 TrackColor = PaletteColor(t);
            DrawList->AddRectFilled(ImVec2(CanvasPos.x + 6, RowY0 + 7), ImVec2(CanvasPos.x + 16, RowY0 + kTrackHeight - 7), ToU32(TrackColor), 2.0f);

            ImGui::SetCursorScreenPos(ImVec2(CanvasPos.x + 22, RowY0 + 5));
            ImGui::PushID(t);
            const bool bTrackSelected = (SelectedTrack == t);
            if (ImGui::Selectable(Resource->NotifyTracks[t].c_str(), bTrackSelected, ImGuiSelectableFlags_None, ImVec2(kHeaderWidth - 30, kTrackHeight - 10)))
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
                    Resource->NotifyTracks[t] = FName(RenameBuf);
                    MarkAnimationDirty();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Notify"))      { AddNotifyAt(Resource, Resource->NotifyTracks[t], AnimComp->CurrentTime, false); }
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Notify State")) { AddNotifyAt(Resource, Resource->NotifyTracks[t], AnimComp->CurrentTime, true); }
                ImGui::Separator();
                if (NumTracks > 1 && ImGui::MenuItem(LE_ICON_DELETE " Delete Track"))
                {
                    Resource->NotifyTracks.erase(Resource->NotifyTracks.begin() + t);
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
            if (bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && LaneRect.Contains(IO.MousePos))
            {
                SelectedTrack = t;
                LaneAddTime = SnapTime(XToTime(IO.MousePos.x), Duration);
                ImGui::OpenPopup("lane_add_ctx");
            }
        }

        // Lane add popup (uses cursor time captured on open).
        if (ImGui::BeginPopup("lane_add_ctx"))
        {
            const float ClickT = LaneAddTime;
            ImGui::TextDisabled("at %.3fs", ClickT);
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

        // Helper: find the lane row index for a track name.
        auto TrackRow = [&](const FName& Track) -> int
        {
            for (int t = 0; t < NumTracks; ++t)
            {
                if (Resource->NotifyTracks[t] == Track) return t;
            }
            return -1;
        };

        DrawList->PushClipRect(ImVec2(LaneX0, TracksY0), ImVec2(LaneX1, CanvasPos.y + CanvasSize.y), true);

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

        // Notify states (draw bars first, behind point flags)
        for (int i = 0; i < (int)Resource->NotifyStates.size(); ++i)
        {
            FAnimationNotifyState& State = Resource->NotifyStates[i];
            const int Row = TrackRow(State.NotifyTrack);
            if (Row < 0) continue;

            const float RowY0 = TracksY0 + Row * kTrackHeight;
            const float X0 = TimeToX(State.StartTime);
            const float X1 = TimeToX(State.EndTime);
            const float BarY0 = RowY0 + 5.0f;
            const float BarY1 = RowY0 + kTrackHeight - 5.0f;

            const bool bSel = (SelectedKind == ENotifyKind::State && SelectedIndex == i);
            FVector4 C = State.Color;
            DrawList->AddRectFilled(ImVec2(X0, BarY0), ImVec2(X1, BarY1), ToU32(FVector4(C.x, C.y, C.z, 0.35f)), 3.0f);
            DrawList->AddRect(ImVec2(X0, BarY0), ImVec2(X1, BarY1), bSel ? IM_COL32(255, 255, 255, 230) : ToU32(C), 3.0f, 0, bSel ? 2.0f : 1.0f);
            // Edge grips.
            DrawList->AddRectFilled(ImVec2(X0, BarY0), ImVec2(X0 + 3.0f, BarY1), ToU32(C));
            DrawList->AddRectFilled(ImVec2(X1 - 3.0f, BarY0), ImVec2(X1, BarY1), ToU32(C));
            DrawList->AddText(ImVec2(X0 + 6.0f, BarY0 + 1.0f), IM_COL32(255, 255, 255, 220), State.NotifyName.c_str());

            // Hit testing.
            const ImVec2 M = IO.MousePos;
            const bool bOverBody = bCanvasHovered && M.x >= X0 && M.x <= X1 && M.y >= BarY0 && M.y <= BarY1;
            const bool bOverLeft  = bCanvasHovered && fabsf(M.x - X0) <= 4.0f && M.y >= BarY0 && M.y <= BarY1;
            const bool bOverRight = bCanvasHovered && fabsf(M.x - X1) <= 4.0f && M.y >= BarY0 && M.y <= BarY1;

            if (DragMode == EDragMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (bOverBody || bOverLeft || bOverRight))
            {
                SelectedKind = ENotifyKind::State;
                SelectedIndex = i;
                DragKind = ENotifyKind::State;
                DragIndex = i;
                DragMode = bOverLeft ? EDragMode::ResizeStart : (bOverRight ? EDragMode::ResizeEnd : EDragMode::MoveItem);
            }
            if (bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && bOverBody)
            {
                SelectedKind = ENotifyKind::State; SelectedIndex = i;
                ImGui::OpenPopup("notify_item_ctx");
            }
        }

        // Point notifies (flags)
        for (int i = 0; i < (int)Resource->Notifies.size(); ++i)
        {
            FAnimationNotify& Notify = Resource->Notifies[i];
            const int Row = TrackRow(Notify.NotifyTrack);
            if (Row < 0) continue;

            const float RowY0 = TracksY0 + Row * kTrackHeight;
            const float X = TimeToX(Notify.Time);
            const float Top = RowY0 + 4.0f;
            const float Bot = RowY0 + kTrackHeight - 4.0f;

            const bool bSel = (SelectedKind == ENotifyKind::Point && SelectedIndex == i);
            const float Flash = (i < IM_ARRAYSIZE(NotifyFlash)) ? NotifyFlash[i] : 0.0f;
            FVector4 C = Notify.Color;
            if (Flash > 0.0f) { C = Math::Mix(C, FVector4(1.0f), Flash * 0.8f); }

            // Flag shape: stem + pennant.
            DrawList->AddLine(ImVec2(X, Top), ImVec2(X, Bot), ToU32(C), 2.0f);
            ImVec2 Flag[3] = { ImVec2(X, Top), ImVec2(X + 11.0f, Top + 4.0f), ImVec2(X, Top + 8.0f) };
            DrawList->AddTriangleFilled(Flag[0], Flag[1], Flag[2], ToU32(C));
            if (bSel)
            {
                DrawList->AddCircleFilled(ImVec2(X, Bot), 3.0f, IM_COL32(255, 255, 255, 255));
            }

            const ImVec2 M = IO.MousePos;
            const bool bOver = bCanvasHovered && fabsf(M.x - X) <= 6.0f && M.y >= Top && M.y <= Bot;
            if (bOver)
            {
                ImGui::SetTooltip("%s @ %.3fs", Notify.NotifyName.c_str(), Notify.Time);
            }
            if (DragMode == EDragMode::None && bOver && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedKind = ENotifyKind::Point; SelectedIndex = i;
                DragKind = ENotifyKind::Point; DragIndex = i; DragMode = EDragMode::MoveItem;
            }
            if (bOver && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedKind = ENotifyKind::Point; SelectedIndex = i;
                ImGui::OpenPopup("notify_item_ctx");
            }
        }

        DrawList->PopClipRect();

        // Playhead (drawn over everything)
        {
            const float PX = TimeToX(AnimComp->CurrentTime);
            if (PX >= LaneX0 - 1.0f && PX <= LaneX1 + 1.0f)
            {
                DrawList->PushClipRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, CanvasPos.y + CanvasSize.y), true);
                DrawList->AddLine(ImVec2(PX, RulerY0), ImVec2(PX, CanvasPos.y + CanvasSize.y), IM_COL32(255, 220, 60, 230), 1.5f);
                DrawList->AddTriangleFilled(ImVec2(PX - 6, RulerY0), ImVec2(PX + 6, RulerY0), ImVec2(PX, RulerY0 + 8), IM_COL32(255, 220, 60, 255));
                DrawList->PopClipRect();
            }
        }

        // Drag handling
        const ImRect RulerRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, RulerY0 + kRulerHeight));
        if (DragMode == EDragMode::None && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && RulerRect.Contains(IO.MousePos))
        {
            DragMode = EDragMode::Playhead;
        }

        if (DragMode != EDragMode::None)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const float T = SnapTime(XToTime(IO.MousePos.x), Duration);
                if (DragMode == EDragMode::Playhead)
                {
                    bIsPlaying = false;
                    AnimComp->bPlaying = false;
                    AnimComp->CurrentTime = T;
                    AnimComp->bDirty = true;
                }
                else if (DragKind == ENotifyKind::Point && DragIndex >= 0 && DragIndex < (int)Resource->Notifies.size())
                {
                    Resource->Notifies[DragIndex].Time = T;
                    MarkAnimationDirty();
                }
                else if (DragKind == ENotifyKind::State && DragIndex >= 0 && DragIndex < (int)Resource->NotifyStates.size())
                {
                    FAnimationNotifyState& S = Resource->NotifyStates[DragIndex];
                    if (DragMode == EDragMode::ResizeStart)      { S.StartTime = Math::Min(T, S.EndTime - 0.001f); }
                    else if (DragMode == EDragMode::ResizeEnd)   { S.EndTime   = Math::Max(T, S.StartTime + 0.001f); }
                    else /* MoveItem */
                    {
                        const float Len = S.EndTime - S.StartTime;
                        S.StartTime = Math::Clamp(T, 0.0f, Duration - Len);
                        S.EndTime   = S.StartTime + Len;
                    }
                    MarkAnimationDirty();
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

        auto TrackCombo = [&](FName& Track)
        {
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("Track", Track.c_str()))
            {
                for (int t = 0; t < (int)Resource->NotifyTracks.size(); ++t)
                {
                    const bool bSel = (Resource->NotifyTracks[t] == Track);
                    if (ImGui::Selectable(Resource->NotifyTracks[t].c_str(), bSel))
                    {
                        Track = Resource->NotifyTracks[t];
                        MarkAnimationDirty();
                    }
                }
                ImGui::EndCombo();
            }
        };

        ImGui::Separator();

        if (SelectedKind == ENotifyKind::Point && SelectedIndex >= 0 && SelectedIndex < (int)Resource->Notifies.size())
        {
            FAnimationNotify& N = Resource->Notifies[SelectedIndex];
            ImGui::TextDisabled(LE_ICON_BELL " Notify");
            ImGui::SameLine();

            snprintf(NameBuf, sizeof(NameBuf), "%s", N.NotifyName.c_str());
            ImGui::SetNextItemWidth(180);
            if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf))) { N.NotifyName = FName(NameBuf); MarkAnimationDirty(); }

            ImGui::SameLine(); TrackCombo(N.NotifyTrack);

            ImGui::SameLine(); ImGui::SetNextItemWidth(120);
            if (ImGui::DragFloat("Time", &N.Time, 0.005f, 0.0f, Duration, "%.3fs")) { MarkAnimationDirty(); }

            ImVec4 Col(N.Color.x, N.Color.y, N.Color.z, N.Color.w);
            ImGui::SameLine();
            if (ImGui::ColorEdit4("##c", &Col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            {
                N.Color = FVector4(Col.x, Col.y, Col.z, Col.w); MarkAnimationDirty();
            }
            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_DELETE " Delete")) { DeleteSelected(Resource); }
        }
        else if (SelectedKind == ENotifyKind::State && SelectedIndex >= 0 && SelectedIndex < (int)Resource->NotifyStates.size())
        {
            FAnimationNotifyState& S = Resource->NotifyStates[SelectedIndex];
            ImGui::TextDisabled(LE_ICON_BELL_OUTLINE " State");
            ImGui::SameLine();

            snprintf(NameBuf, sizeof(NameBuf), "%s", S.NotifyName.c_str());
            ImGui::SetNextItemWidth(180);
            if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf))) { S.NotifyName = FName(NameBuf); MarkAnimationDirty(); }

            ImGui::SameLine(); TrackCombo(S.NotifyTrack);

            ImGui::SameLine(); ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat("Start", &S.StartTime, 0.005f, 0.0f, S.EndTime - 0.001f, "%.3f")) { MarkAnimationDirty(); }
            ImGui::SameLine(); ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat("End", &S.EndTime, 0.005f, S.StartTime + 0.001f, Duration, "%.3f")) { MarkAnimationDirty(); }

            ImVec4 Col(S.Color.x, S.Color.y, S.Color.z, S.Color.w);
            ImGui::SameLine();
            if (ImGui::ColorEdit4("##c", &Col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            {
                S.Color = FVector4(Col.x, Col.y, Col.z, Col.w); MarkAnimationDirty();
            }
            ImGui::SameLine();
            if (ImGui::Button(LE_ICON_DELETE " Delete")) { DeleteSelected(Resource); }
        }
        else
        {
            ImGui::TextDisabled("Select a notify to edit its properties, or right-click a lane to add one.");
        }
    }

    void FAnimationEditorTool::AddCurve(FAnimationResource* Resource)
    {
        FAnimationCurve Curve;
        Curve.Name  = FName(std::format("Curve {}", (int)Resource->Curves.size()).c_str());
        Curve.Color = PaletteColor((int32)Resource->Curves.size());
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

        ImGui::BeginChild("CurveList", ImVec2(240, 0), true);
        if (ImGui::Button(LE_ICON_PLUS " Curve", ImVec2(-1, 0)))
        {
            AddCurve(Resource);
        }
        ImGui::Separator();

        for (int i = 0; i < (int)Resource->Curves.size(); ++i)
        {
            FAnimationCurve& Curve = Resource->Curves[i];
            ImGui::PushID(i);

            const ImVec2 RowPos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(std::format("      {}", Curve.Name.c_str()).c_str(), SelectedCurve == i))
            {
                SelectedCurve = i;
            }

            const float RowHeight = ImGui::GetItemRectSize().y;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(RowPos.x + 2, RowPos.y + 3),
                                                      ImVec2(RowPos.x + 12, RowPos.y + RowHeight - 3),
                                                      ToU32(Curve.Color), 2.0f);

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
            ImGui::PopID();
        }

        if (Resource->Curves.empty())
        {
            ImGui::TextDisabled("No curves.");
            ImGui::TextDisabled("Add one, then read it");
            ImGui::TextDisabled("with a Get Curve node.");
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("CurvePanel", ImVec2(0, 0), true);
        if (SelectedCurve < 0 || SelectedCurve >= (int)Resource->Curves.size())
        {
            ImGui::TextDisabled("Select a curve to edit its keys.");
            CurveWidget.SetCurve(nullptr);
            ImGui::EndChild();
            return;
        }

        FAnimationCurve& Curve = Resource->Curves[SelectedCurve];

        char NameBuf[128];
        snprintf(NameBuf, sizeof(NameBuf), "%s", Curve.Name.c_str());
        ImGui::SetNextItemWidth(180);
        if (ImGui::InputText("##CurveName", NameBuf, sizeof(NameBuf)))
        {
            Curve.Name = FName(NameBuf);
            MarkAnimationDirty();
        }

        ImVec4 Col(Curve.Color.x, Curve.Color.y, Curve.Color.z, Curve.Color.w);
        ImGui::SameLine();
        if (ImGui::ColorEdit4("##CurveColor", &Col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        {
            Curve.Color = FVector4(Col.x, Col.y, Col.z, Col.w);
            MarkAnimationDirty();
        }

        // Re-keying at the playhead is the primary authoring gesture: scrub, type a value, done.
        const float PlayheadTime = SnapTime(AnimComp->CurrentTime, Duration);
        float ValueAtPlayhead = Curve.Curve.Evaluate(PlayheadTime);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("##CurveValue", &ValueAtPlayhead, 0.01f, 0.0f, 0.0f, "%.3f"))
        {
            Curve.Curve.UpdateOrAddKey(PlayheadTime, ValueAtPlayhead);
            Curve.Curve.ComputeAutoTangents();
            MarkAnimationDirty();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Value at the playhead. Editing keys the curve at %.3fs.", PlayheadTime);
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Key"))
        {
            Curve.Curve.UpdateOrAddKey(PlayheadTime, ValueAtPlayhead);
            Curve.Curve.ComputeAutoTangents();
            MarkAnimationDirty();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Key the current value at the playhead (%.3fs)", PlayheadTime);
        }

        ImGui::SameLine();
        ImGui::TextDisabled("%d keys", Curve.Curve.NumKeys());

        CurveWidget.SetCurve(&Curve.Curve);
        CurveWidget.SetOnModified([this]() { MarkAnimationDirty(); });
        CurveWidget.SetTimeMarker(AnimComp->CurrentTime);
        CurveWidget.Draw("AnimCurve");

        ImGui::EndChild();
    }

    void FAnimationEditorTool::DrawBoneChannels(CAnimation* Animation, SSimpleAnimationComponent* AnimComp, float Duration)
    {
        FAnimationResource* Resource = Animation->GetAnimationResource();

        ImGui::BeginChild("CurveChannels", ImVec2(240, 0), true);
        if (ImGui::CollapsingHeader("Channels", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int i = 0; i < (int)Resource->Channels.size(); ++i)
            {
                const FAnimationChannel& Channel = Resource->Channels[i];
                const char* PathIcon = "";
                switch (Channel.TargetPath)
                {
                    case FAnimationChannel::ETargetPath::Translation: PathIcon = LE_ICON_AXIS_ARROW; break;
                    case FAnimationChannel::ETargetPath::Rotation:    PathIcon = LE_ICON_ROTATE_360; break;
                    case FAnimationChannel::ETargetPath::Scale:       PathIcon = LE_ICON_ARROW_TOP_RIGHT_BOTTOM_LEFT; break;
                    case FAnimationChannel::ETargetPath::Weights:     PathIcon = LE_ICON_WEIGHT; break;
                }

                ImGui::PushID(i);
                if (ImGui::Selectable(std::format("{} {}", PathIcon, Channel.TargetBone.c_str()).c_str(), SelectedChannel == i))
                {
                    SelectedChannel = i;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("CurvePlot", ImVec2(0, 0), true);
        if (SelectedChannel >= 0 && SelectedChannel < (int)Resource->Channels.size())
        {
            FAnimationChannel& Channel = Resource->Channels[SelectedChannel];

            const char* PathName = "";
            switch (Channel.TargetPath)
            {
                case FAnimationChannel::ETargetPath::Translation: PathName = "Translation"; break;
                case FAnimationChannel::ETargetPath::Rotation:    PathName = "Rotation"; break;
                case FAnimationChannel::ETargetPath::Scale:       PathName = "Scale"; break;
                case FAnimationChannel::ETargetPath::Weights:     PathName = "Weights"; break;
            }
            ImGui::Text("Channel: %s - (%s)", Channel.TargetBone.c_str(), PathName);

            if (ImPlot::BeginPlot("##AnimCurves", ImVec2(-1, -1)))
            {
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, Duration, ImGuiCond_Always);
                ImPlot::SetupAxis(ImAxis_X1, "Time (s)");
                ImPlot::SetupAxis(ImAxis_Y1, "Value");

                const float TimeBefore = AnimComp->CurrentTime;
                double CurrentTimeDouble = AnimComp->CurrentTime;
                ImPlot::DragLineX(0, &CurrentTimeDouble, ImVec4(1, 1, 0, 1), 2.0f);
                AnimComp->CurrentTime = Math::Clamp((float)CurrentTimeDouble, 0.0f, Duration);

                // Scrubbing the curve playhead must resample the pose: the editor world
                // is paused, so the animation system only re-evaluates when bDirty is set.
                if (AnimComp->CurrentTime != TimeBefore)
                {
                    bIsPlaying = false;
                    AnimComp->bDirty = true;
                }

                switch (Channel.TargetPath)
                {
                    case FAnimationChannel::ETargetPath::Translation:
                    case FAnimationChannel::ETargetPath::Scale:
                    {
                        TVector<FVector3>& Data = (Channel.TargetPath == FAnimationChannel::ETargetPath::Translation)
                            ? Channel.Translations : Channel.Scales;
                        if (!Data.empty())
                        {
                            TVector<float> XVals, YVals, ZVals;
                            for (const auto& V : Data) { XVals.push_back(V.x); YVals.push_back(V.y); ZVals.push_back(V.z); }
                            ImPlot::PlotLine("X", Channel.Timestamps.data(), XVals.data(), (int)Data.size());
                            ImPlot::PlotLine("Y", Channel.Timestamps.data(), YVals.data(), (int)Data.size());
                            ImPlot::PlotLine("Z", Channel.Timestamps.data(), ZVals.data(), (int)Data.size());
                        }
                        break;
                    }
                    case FAnimationChannel::ETargetPath::Rotation:
                    {
                        if (!Channel.Rotations.empty())
                        {
                            TVector<float> XVals, YVals, ZVals;
                            for (const auto& Q : Channel.Rotations)
                            {
                                FVector3 Euler = Math::Degrees(Math::EulerAngles(Q));
                                XVals.push_back(Euler.x); YVals.push_back(Euler.y); ZVals.push_back(Euler.z);
                            }
                            ImPlot::PlotLine("X (deg)", Channel.Timestamps.data(), XVals.data(), (int)Channel.Rotations.size());
                            ImPlot::PlotLine("Y (deg)", Channel.Timestamps.data(), YVals.data(), (int)Channel.Rotations.size());
                            ImPlot::PlotLine("Z (deg)", Channel.Timestamps.data(), ZVals.data(), (int)Channel.Rotations.size());
                        }
                        break;
                    }
                    default: break;
                }

                ImPlot::EndPlot();
            }
        }
        else
        {
            ImGui::TextDisabled("Select a channel to view its curves.");
        }
        ImGui::EndChild();
    }
}
