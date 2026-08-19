#include "Containers/StringFormat.h"
#include "AnimationMontageEditorTool.h"
#include <string>
#include "UI/Properties/PropertyTable.h"
#include "Animation/AnimNotify.h"

#include <imgui_internal.h>

#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"

namespace Lumina
{
    static const char* MontageDetailsName  = "Details";
    static const char* MontageTimelineName = "MontageTimeline";

    namespace
    {
        constexpr float kMontageHeaderWidth  = 168.0f;
        constexpr float kMontageRulerHeight  = 26.0f;
        constexpr float kMontageSectionHeight = 24.0f;
        constexpr float kMontageTrackHeight  = 34.0f;
        constexpr float kMontageNotifyHeight = 26.0f;

        const ImU32 kSegmentFill    = IM_COL32(66, 118, 172, 200);
        const ImU32 kSegmentBorder  = IM_COL32(150, 200, 255, 220);
        const ImU32 kSegmentLoopBar = IM_COL32(255, 255, 255, 40);
        const ImU32 kSectionFill    = IM_COL32(120, 92, 168, 210);
        const ImU32 kNotifyColor    = IM_COL32(238, 107, 92, 235);
        const ImU32 kNotifyStateFill = IM_COL32(238, 199, 89, 90);
    }

    FAnimationMontageEditorTool::FAnimationMontageEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FAnimationMontageEditorTool::OnInitialize()
    {
        CreateToolWindow(MontageDetailsName, [&](bool bFocused)
        {
            CAnimationMontage* Montage = GetAsset<CAnimationMontage>();
            const bool bHasSelection = Montage != nullptr && SelectedKind != ESelectionKind::None;

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText(bHasSelection ? "Selection Details" : "Asset Details");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            if (bHasSelection)
            {
                DrawInspector(Montage, Math::Max(Montage->GetDuration(), 1.0f));
                return;
            }

            PropertyTable.DrawTree();
        });

        CreateToolWindow(MontageTimelineName, [&](bool bFocused)
        {
            DrawTimeline();
        });
    }

    void FAnimationMontageEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        CreateFloorPlane();

        DirectionalLightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(DirectionalLightEntity);
        World->EmplaceComponent<SSkyLightComponent>(DirectionalLightEntity);

        CAnimationMontage* Montage = Cast<CAnimationMontage>(Asset.Get());
        if (Montage == nullptr || !Montage->Skeleton.IsValid())
        {
            return;
        }

        CameraState.Speed = 5.0f;

        MeshEntity = World->ConstructEntity("MeshEntity");
        World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SetSkeletalMesh(Montage->Skeleton->PreviewMesh);

        SSimpleAnimationComponent& AnimComp = World->EmplaceComponent<SSimpleAnimationComponent>(MeshEntity);
        AnimComp.bPlaying = false;
        AnimComp.bLooping = false;

        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        FQuat Rotation = Math::FindLookAtRotation(MeshTransform.GetLocation() + FVector3(0.0f, 0.85f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(Rotation);
    }

    void FAnimationMontageEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (World && World->GetRenderer())
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }
    }

    void FAnimationMontageEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FAnimationMontageEditorTool::OnAssetLoadFinished()
    {
        if (CAnimationMontage* Montage = GetAsset<CAnimationMontage>())
        {
            Montage->EnsureNotifyTracks();
        }
    }

    SSimpleAnimationComponent* FAnimationMontageEditorTool::GetPreviewComponent() const
    {
        if (!World.IsValid() || MeshEntity == entt::null)
        {
            return nullptr;
        }
        return World->TryGetComponent<SSimpleAnimationComponent>(MeshEntity);
    }

    void FAnimationMontageEditorTool::MarkMontageDirty()
    {
        if (Asset.IsValid() && Asset->GetPackage())
        {
            Asset->GetPackage()->MarkDirty();
        }
    }

    float FAnimationMontageEditorTool::SnapTime(float Time, float Duration) const
    {
        if (bSnapToFrame && FrameRate > 0)
        {
            const float Step = 1.0f / (float)FrameRate;
            Time = roundf(Time / Step) * Step;
        }
        return Math::Clamp(Time, 0.0f, Duration);
    }

    void FAnimationMontageEditorTool::ClearSelection()
    {
        SelectedKind  = ESelectionKind::None;
        SelectedIndex = -1;
    }

    void FAnimationMontageEditorTool::AppendSegment(CAnimationMontage* Montage, int32 TrackIndex, CAnimation* Animation, float StartTime)
    {
        if (Animation == nullptr || TrackIndex < 0 || TrackIndex >= (int32)Montage->SlotTracks.size())
        {
            return;
        }

        SAnimMontageSlotTrack& Track = Montage->SlotTracks[TrackIndex];

        SAnimMontageSegment& Segment = Track.Segments.emplace_back();
        Segment.Animation = Animation;
        Segment.StartTime = Math::Max(StartTime, 0.0f);

        SelectedKind  = ESelectionKind::Segment;
        SelectedTrack = TrackIndex;
        SelectedIndex = (int32)Track.Segments.size() - 1;

        MarkMontageDirty();
    }

    void FAnimationMontageEditorTool::ReflowTrack(CAnimationMontage* Montage, int32 TrackIndex)
    {
        if (TrackIndex < 0 || TrackIndex >= (int32)Montage->SlotTracks.size())
        {
            return;
        }

        SAnimMontageSlotTrack& Track = Montage->SlotTracks[TrackIndex];
        Algo::StableSort(Track.Segments.begin(), Track.Segments.end(),
            [](const SAnimMontageSegment& A, const SAnimMontageSegment& B) { return A.StartTime < B.StartTime; });

        float Cursor = 0.0f;
        for (SAnimMontageSegment& Segment : Track.Segments)
        {
            Segment.StartTime = Cursor;
            Cursor += Segment.GetTimelineLength();
        }

        ClearSelection();
        MarkMontageDirty();
    }

    void FAnimationMontageEditorTool::DeleteSelected(CAnimationMontage* Montage)
    {
        switch (SelectedKind)
        {
        case ESelectionKind::Segment:
        {
            if (SelectedTrack >= 0 && SelectedTrack < (int32)Montage->SlotTracks.size())
            {
                SAnimMontageSlotTrack& Track = Montage->SlotTracks[SelectedTrack];
                if (SelectedIndex >= 0 && SelectedIndex < (int32)Track.Segments.size())
                {
                    Track.Segments.erase(Track.Segments.begin() + SelectedIndex);
                    MarkMontageDirty();
                }
            }
            break;
        }
        case ESelectionKind::Section:
        {
            if (SelectedIndex >= 0 && SelectedIndex < (int32)Montage->Sections.size())
            {
                Montage->Sections.erase(Montage->Sections.begin() + SelectedIndex);
                MarkMontageDirty();
            }
            break;
        }
        case ESelectionKind::Notify:
        {
            if (SelectedIndex >= 0 && SelectedIndex < (int32)Montage->Notifies.size())
            {
                Montage->Notifies.erase(Montage->Notifies.begin() + SelectedIndex);
                MarkMontageDirty();
            }
            break;
        }
        case ESelectionKind::NotifyState:
        {
            if (SelectedIndex >= 0 && SelectedIndex < (int32)Montage->NotifyStates.size())
            {
                Montage->NotifyStates.erase(Montage->NotifyStates.begin() + SelectedIndex);
                MarkMontageDirty();
            }
            break;
        }
        default:
            break;
        }

        ClearSelection();
    }

    void FAnimationMontageEditorTool::SyncPreviewToPlayhead(CAnimationMontage* Montage)
    {
        SSimpleAnimationComponent* AnimComp = GetPreviewComponent();
        if (AnimComp == nullptr)
        {
            return;
        }

        AnimComp->bPlaying = false;
        AnimComp->bLooping = false;

        if (PreviewTrack < 0 || PreviewTrack >= (int32)Montage->SlotTracks.size())
        {
            return;
        }

        FAnimMontageSlotSample Sample;
        const FName& SlotName = Montage->SlotTracks[PreviewTrack].SlotName;

        if (Montage->EvaluateSlot(SlotName, Playhead, Playhead, Sample) && Sample.Clip != nullptr)
        {
            AnimComp->Animation   = Sample.Clip;
            AnimComp->CurrentTime = Sample.ClipTime;
            AnimComp->bDirty      = true;
        }
    }

    void FAnimationMontageEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        if (ImGui::BeginMenu(LE_ICON_BONE " Skeleton"))
        {
            DrawSkeletonDebugMenuItems();
            ImGui::EndMenu();
        }
    }

    void FAnimationMontageEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Slots",
            "A montage plays its slot tracks through the matching Slot node in an animation graph. "
            "Add a Slot node, name it the same as a track here, and wire your locomotion pose into its Source.");
        DrawHelpTextRow("Segments",
            "Drag an animation from the content browser onto a slot lane to add a segment. "
            "Drag a segment to move it, drag its right edge to change its play rate. "
            "Right-click a lane for Reflow, which packs the segments end to end.");
        DrawHelpTextRow("Sections",
            "Sections split the timeline into named regions. A section runs to the next one in time order, "
            "then continues into its Next Section; leaving Next Section empty ends the montage there. "
            "Point Next Section at itself to loop, and call JumpToMontageSection from script to chain a combo.");
        DrawHelpTextRow("Playing from script",
            "Grab the entity's AnimationGraphComponent:\n"
            "  anim.PlayMontage(Montage, 1.0f)\n"
            "  anim.JumpToMontageSection(Montage, \"Combo2\")\n"
            "  anim.StopMontage(Montage)\n"
            "Notifies fire into the same buffer as graph notifies, so WasNotifyTriggered sees them.");
    }

    void FAnimationMontageEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0, bottomDockID = 0;

        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.25f, &rightDockID, &leftDockID);
        ImGui::DockBuilderSplitNode(leftDockID, ImGuiDir_Down, 0.42f, &bottomDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MontageDetailsName).c_str(), rightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(MontageTimelineName).c_str(), bottomDockID);
    }

    void FAnimationMontageEditorTool::DrawTimeline()
    {
        CAnimationMontage* Montage = GetAsset<CAnimationMontage>();
        if (Montage == nullptr)
        {
            return;
        }

        Montage->EnsureNotifyTracks();

        if (Montage->SlotTracks.empty())
        {
            SAnimMontageSlotTrack& Track = Montage->SlotTracks.emplace_back();
            Track.SlotName = "DefaultSlot";
            MarkMontageDirty();
        }

        // An empty montage still needs a lane wide enough to drop the first clip onto.
        const float Duration = Math::Max(Montage->GetDuration(), 1.0f);

        DrawTransport(Duration);
        SyncPreviewToPlayhead(Montage);

        ImGui::Separator();

        DrawSlotTracks(Montage, Duration);
    }

    void FAnimationMontageEditorTool::DrawTransport(float Duration)
    {
        if (bIsPlaying)
        {
            Playhead += ImGui::GetIO().DeltaTime * PlayRate;
            if (Playhead >= Duration)
            {
                Playhead = bLooping ? fmodf(Playhead, Duration) : Duration;
                bIsPlaying = bLooping;
            }
        }

        if (ImGui::Button(bIsPlaying ? LE_ICON_PAUSE " Pause" : LE_ICON_PLAY " Play", ImVec2(96, 0)))
        {
            if (!bIsPlaying && Playhead >= Duration && !bLooping)
            {
                Playhead = 0.0f;
            }
            bIsPlaying = !bIsPlaying;
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_STOP " Stop", ImVec2(80, 0)))
        {
            bIsPlaying = false;
            Playhead   = 0.0f;
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_STEP_BACKWARD, ImVec2(36, 0)))
        {
            Playhead = SnapTime(Playhead - 1.0f / (float)FrameRate, Duration);
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_STEP_FORWARD, ImVec2(36, 0)))
        {
            Playhead = SnapTime(Playhead + 1.0f / (float)FrameRate, Duration);
        }

        ImGui::SameLine();
        ImGui::Checkbox(LE_ICON_REPEAT " Loop", &bLooping);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("##PlayRate", &PlayRate, 0.05f, 4.0f, "Rate %.2fx");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("##Zoom", &TimelineZoom, 1.0f, 16.0f, LE_ICON_MAGNIFY " %.1fx");

        ImGui::SameLine();
        ImGui::Checkbox("Snap", &bSnapToFrame);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragInt("##FPS", &FrameRate, 1.0f, 1, 240, "%d fps");

        const int Frame      = (int)roundf(Playhead * (float)FrameRate);
        const int TotalFrame = (int)roundf(Duration * (float)FrameRate);
        const FString Readout = Format("{:.3f}s / {:.3f}s   (frame {}/{})", Playhead, Duration, Frame, TotalFrame);
        const float TextW = ImGui::CalcTextSize(Readout.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(Math::Max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - TextW - 16.0f));
        ImGui::TextUnformatted(Readout.c_str());
    }

    void FAnimationMontageEditorTool::DrawSlotTracks(CAnimationMontage* Montage, float Duration)
    {
        if (ImGui::Button(LE_ICON_PLUS " Slot"))
        {
            SAnimMontageSlotTrack& Track = Montage->SlotTracks.emplace_back();
            Track.SlotName = FName(Format("Slot {}", (int)Montage->SlotTracks.size()).c_str());
            MarkMontageDirty();
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Section"))
        {
            SAnimMontageSection& Section = Montage->Sections.emplace_back();
            Section.Name = FName(Format("Section{}", (int)Montage->Sections.size()).c_str());
            Section.StartTime = SnapTime(Playhead, Duration);
            MarkMontageDirty();
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_PLUS " Notify"))
        {
            SAnimMontageNotify& Notify = Montage->Notifies.emplace_back();
            Notify.Name  = "NewNotify";
            Notify.Time  = SnapTime(Playhead, Duration);
            Notify.Track = Montage->NotifyTracks.empty() ? FName("Notifies") : Montage->NotifyTracks[0];
            SelectedKind  = ESelectionKind::Notify;
            SelectedIndex = (int32)Montage->Notifies.size() - 1;
            MarkMontageDirty();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(drag a clip from the content browser onto a slot lane)");

        const int32 NumTracks = (int32)Montage->SlotTracks.size();
        const float CanvasHeight = kMontageRulerHeight + kMontageSectionHeight + NumTracks * kMontageTrackHeight + kMontageNotifyHeight + 8.0f;

        ImGui::BeginChild("MontageCanvas", ImVec2(0, CanvasHeight), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList*  DrawList   = ImGui::GetWindowDrawList();
        const ImVec2 CanvasPos  = ImGui::GetCursorScreenPos();
        const ImVec2 CanvasSize = ImGui::GetContentRegionAvail();

        const float LaneX0 = CanvasPos.x + kMontageHeaderWidth;
        const float LaneX1 = CanvasPos.x + CanvasSize.x;
        const float LaneW  = Math::Max(LaneX1 - LaneX0, 1.0f);

        const float RulerY0    = CanvasPos.y;
        const float SectionY0  = RulerY0 + kMontageRulerHeight;
        const float TracksY0   = SectionY0 + kMontageSectionHeight;
        const float NotifyY0   = TracksY0 + NumTracks * kMontageTrackHeight;
        const float CanvasY1   = CanvasPos.y + CanvasSize.y;

        const float BasePPS = LaneW / Duration;
        const float PPS     = BasePPS * TimelineZoom;

        const float MaxPan = Math::Max(0.0f, Duration - LaneW / PPS);
        TimelinePanSeconds = Math::Clamp(TimelinePanSeconds, 0.0f, MaxPan);

        auto TimeToX = [&](float T) { return LaneX0 + (T - TimelinePanSeconds) * PPS; };
        auto XToTime = [&](float X) { return TimelinePanSeconds + (X - LaneX0) / PPS; };

        const ImGuiIO& IO = ImGui::GetIO();
        const bool bCanvasHovered = ImGui::IsWindowHovered();

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
                TimelinePanSeconds = CursorT - (IO.MousePos.x - LaneX0) / (BasePPS * TimelineZoom);
            }
        }

        DrawList->AddRectFilled(CanvasPos, ImVec2(LaneX1, CanvasY1), IM_COL32(24, 24, 28, 255));
        DrawList->AddRectFilled(CanvasPos, ImVec2(CanvasPos.x + kMontageHeaderWidth, CanvasY1), IM_COL32(32, 33, 38, 255));
        DrawList->AddRectFilled(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, RulerY0 + kMontageRulerHeight), IM_COL32(40, 41, 47, 255));

        DrawList->PushClipRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, CanvasY1), true);
        {
            float TargetSec = 90.0f / PPS;
            const float FrameSec = 1.0f / (float)FrameRate;
            float Step = FrameSec;
            while (Step < TargetSec) { Step *= (Step * 5.0f < TargetSec ? 5.0f : 2.0f); }

            const float FirstT = floorf(TimelinePanSeconds / Step) * Step;
            for (float T = FirstT; T <= XToTime(LaneX1); T += Step)
            {
                if (T < 0.0f) continue;
                const float X = TimeToX(T);
                DrawList->AddLine(ImVec2(X, RulerY0 + kMontageRulerHeight - 7.0f), ImVec2(X, CanvasY1), IM_COL32(255, 255, 255, 16));
                DrawList->AddLine(ImVec2(X, RulerY0 + kMontageRulerHeight - 7.0f), ImVec2(X, RulerY0 + kMontageRulerHeight), IM_COL32(200, 200, 210, 120));
                FString Label = Format("{:.2f}", T).c_str();
                DrawList->AddText(ImVec2(X + 3.0f, RulerY0 + 3.0f), IM_COL32(180, 182, 190, 200), Label.c_str());
            }
        }
        DrawList->PopClipRect();

        SortSections(Montage);

        // Section lane, drawn here because it shares the canvas geometry above.
        DrawList->AddRectFilled(ImVec2(CanvasPos.x, SectionY0), ImVec2(LaneX1, SectionY0 + kMontageSectionHeight), IM_COL32(30, 30, 36, 255));
        DrawList->AddText(ImVec2(CanvasPos.x + 8, SectionY0 + 4), IM_COL32(170, 172, 182, 220), "Sections");

        DrawList->PushClipRect(ImVec2(LaneX0, SectionY0), ImVec2(LaneX1, SectionY0 + kMontageSectionHeight), true);
        for (int32 i = 0; i < (int32)Montage->Sections.size(); ++i)
        {
            const SAnimMontageSection& Section = Montage->Sections[i];
            const float X0 = TimeToX(Section.StartTime);
            const float X1 = TimeToX(Montage->GetSectionEndTime(i));

            const bool bSel = (SelectedKind == ESelectionKind::Section && SelectedIndex == i);
            DrawList->AddRectFilled(ImVec2(X0 + 1, SectionY0 + 3), ImVec2(X1 - 1, SectionY0 + kMontageSectionHeight - 3), kSectionFill, 3.0f);
            if (bSel)
            {
                DrawList->AddRect(ImVec2(X0 + 1, SectionY0 + 3), ImVec2(X1 - 1, SectionY0 + kMontageSectionHeight - 3), IM_COL32(255, 255, 255, 230), 3.0f, 0, 2.0f);
            }
            DrawList->AddText(ImVec2(X0 + 6, SectionY0 + 4), IM_COL32(255, 255, 255, 235), Section.Name.c_str());

            const ImRect Rect(ImVec2(X0, SectionY0), ImVec2(X1, SectionY0 + kMontageSectionHeight));
            if (bCanvasHovered && DragMode == EDragMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && Rect.Contains(IO.MousePos))
            {
                SelectedKind  = ESelectionKind::Section;
                SelectedIndex = i;
                DragKind = ESelectionKind::Section;
                DragIndex = i;
                DragMode = EDragMode::MoveItem;
                DragGrabOffset = XToTime(IO.MousePos.x) - Section.StartTime;
            }
        }
        DrawList->PopClipRect();

        // Slot lanes.
        for (int32 t = 0; t < NumTracks; ++t)
        {
            SAnimMontageSlotTrack& Track = Montage->SlotTracks[t];

            const float RowY0 = TracksY0 + t * kMontageTrackHeight;
            const float RowY1 = RowY0 + kMontageTrackHeight;

            const ImU32 RowBg = (t & 1) ? IM_COL32(28, 28, 33, 255) : IM_COL32(24, 24, 28, 255);
            DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), RowBg);
            DrawList->AddLine(ImVec2(CanvasPos.x, RowY1), ImVec2(LaneX1, RowY1), IM_COL32(0, 0, 0, 120));

            const bool bIsPreview = (PreviewTrack == t);
            DrawList->AddRectFilled(ImVec2(CanvasPos.x + 6, RowY0 + 8), ImVec2(CanvasPos.x + 16, RowY1 - 8),
                                    bIsPreview ? IM_COL32(120, 220, 140, 255) : IM_COL32(90, 92, 100, 255), 2.0f);

            ImGui::SetCursorScreenPos(ImVec2(CanvasPos.x + 22, RowY0 + 6));
            ImGui::PushID(t);
            if (ImGui::Selectable(Track.SlotName.c_str(), bIsPreview, ImGuiSelectableFlags_None, ImVec2(kMontageHeaderWidth - 30, kMontageTrackHeight - 12)))
            {
                PreviewTrack = t;
            }

            if (ImGui::BeginPopupContextItem("slot_ctx"))
            {
                static char RenameBuf[128];
                if (ImGui::IsWindowAppearing())
                {
                    snprintf(RenameBuf, sizeof(RenameBuf), "%s", Track.SlotName.c_str());
                }
                ImGui::SetNextItemWidth(160);
                if (ImGui::InputText("Slot Name", RenameBuf, sizeof(RenameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    Track.SlotName = FName(RenameBuf);
                    MarkMontageDirty();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(LE_ICON_ARROW_COLLAPSE_LEFT " Reflow Segments"))
                {
                    ReflowTrack(Montage, t);
                }
                if (NumTracks > 1 && ImGui::MenuItem(LE_ICON_DELETE " Delete Slot"))
                {
                    Montage->SlotTracks.erase(Montage->SlotTracks.begin() + t);
                    PreviewTrack = 0;
                    ClearSelection();
                    MarkMontageDirty();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // Drop target covering the lane, so a clip can be dragged straight onto it.
            ImGui::SetCursorScreenPos(ImVec2(LaneX0, RowY0));
            ImGui::PushID(1000 + t);
            ImGui::InvisibleButton("lane_drop", ImVec2(Math::Max(LaneX1 - LaneX0, 1.0f), kMontageTrackHeight));
            if (ImGui::BeginDragDropTarget())
            {
                if (CAnimation* Dropped = DragDrop::AcceptAsset<CAnimation>())
                {
                    AppendSegment(Montage, t, Dropped, SnapTime(XToTime(IO.MousePos.x), Duration * 4.0f));
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();

            DrawList->PushClipRect(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), true);
            for (int32 s = 0; s < (int32)Track.Segments.size(); ++s)
            {
                SAnimMontageSegment& Segment = Track.Segments[s];

                const float X0 = TimeToX(Segment.StartTime);
                const float X1 = TimeToX(Segment.GetEndTime());
                const float BarY0 = RowY0 + 5.0f;
                const float BarY1 = RowY1 - 5.0f;

                const bool bSel = (SelectedKind == ESelectionKind::Segment && SelectedTrack == t && SelectedIndex == s);

                DrawList->AddRectFilled(ImVec2(X0, BarY0), ImVec2(X1, BarY1), kSegmentFill, 3.0f);
                DrawList->AddRect(ImVec2(X0, BarY0), ImVec2(X1, BarY1),
                                  bSel ? IM_COL32(255, 255, 255, 235) : kSegmentBorder, 3.0f, 0, bSel ? 2.0f : 1.0f);

                const int32 Loops = Math::Max(Segment.LoopCount, 1);
                for (int32 L = 1; L < Loops; ++L)
                {
                    const float LX = X0 + (X1 - X0) * ((float)L / (float)Loops);
                    DrawList->AddLine(ImVec2(LX, BarY0), ImVec2(LX, BarY1), kSegmentLoopBar, 1.0f);
                }

                const char* ClipName = Segment.Animation.IsValid() ? Segment.Animation->GetName().c_str() : "<empty>";
                DrawList->AddText(ImVec2(X0 + 6.0f, BarY0 + 2.0f), IM_COL32(255, 255, 255, 235), ClipName);

                const ImVec2 M = IO.MousePos;
                const bool bOverBody  = bCanvasHovered && M.x >= X0 && M.x <= X1 && M.y >= BarY0 && M.y <= BarY1;
                const bool bOverRight = bCanvasHovered && fabsf(M.x - X1) <= 4.0f && M.y >= BarY0 && M.y <= BarY1;

                if (DragMode == EDragMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (bOverBody || bOverRight))
                {
                    SelectedKind  = ESelectionKind::Segment;
                    SelectedTrack = t;
                    SelectedIndex = s;
                    DragKind  = ESelectionKind::Segment;
                    DragTrack = t;
                    DragIndex = s;
                    DragMode  = bOverRight ? EDragMode::ResizeEnd : EDragMode::MoveItem;
                    DragGrabOffset = XToTime(M.x) - Segment.StartTime;
                }
            }
            DrawList->PopClipRect();
        }

        // Notify lane.
        DrawList->AddRectFilled(ImVec2(CanvasPos.x, NotifyY0), ImVec2(LaneX1, NotifyY0 + kMontageNotifyHeight), IM_COL32(30, 30, 36, 255));
        DrawList->AddText(ImVec2(CanvasPos.x + 8, NotifyY0 + 5), IM_COL32(170, 172, 182, 220), "Notifies");

        DrawList->PushClipRect(ImVec2(LaneX0, NotifyY0), ImVec2(LaneX1, NotifyY0 + kMontageNotifyHeight), true);

        for (int32 i = 0; i < (int32)Montage->NotifyStates.size(); ++i)
        {
            SAnimMontageNotifyState& State = Montage->NotifyStates[i];
            const float X0 = TimeToX(State.StartTime);
            const float X1 = TimeToX(State.EndTime);

            const bool bSel = (SelectedKind == ESelectionKind::NotifyState && SelectedIndex == i);
            DrawList->AddRectFilled(ImVec2(X0, NotifyY0 + 4), ImVec2(X1, NotifyY0 + kMontageNotifyHeight - 4), kNotifyStateFill, 3.0f);
            DrawList->AddRect(ImVec2(X0, NotifyY0 + 4), ImVec2(X1, NotifyY0 + kMontageNotifyHeight - 4),
                              bSel ? IM_COL32(255, 255, 255, 230) : IM_COL32(238, 199, 89, 200), 3.0f, 0, bSel ? 2.0f : 1.0f);
            DrawList->AddText(ImVec2(X0 + 5, NotifyY0 + 5), IM_COL32(255, 255, 255, 220), State.Name.c_str());

            const ImVec2 M = IO.MousePos;
            const bool bOverBody  = bCanvasHovered && M.x >= X0 && M.x <= X1 && M.y >= NotifyY0 && M.y <= NotifyY0 + kMontageNotifyHeight;
            const bool bOverLeft  = bCanvasHovered && fabsf(M.x - X0) <= 4.0f && M.y >= NotifyY0 && M.y <= NotifyY0 + kMontageNotifyHeight;
            const bool bOverRight = bCanvasHovered && fabsf(M.x - X1) <= 4.0f && M.y >= NotifyY0 && M.y <= NotifyY0 + kMontageNotifyHeight;

            if (DragMode == EDragMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (bOverBody || bOverLeft || bOverRight))
            {
                SelectedKind  = ESelectionKind::NotifyState;
                SelectedIndex = i;
                DragKind  = ESelectionKind::NotifyState;
                DragIndex = i;
                DragMode  = bOverLeft ? EDragMode::ResizeStart : (bOverRight ? EDragMode::ResizeEnd : EDragMode::MoveItem);
                DragGrabOffset = XToTime(M.x) - State.StartTime;
            }
        }

        for (int32 i = 0; i < (int32)Montage->Notifies.size(); ++i)
        {
            SAnimMontageNotify& Notify = Montage->Notifies[i];
            const float X = TimeToX(Notify.Time);
            const float Top = NotifyY0 + 3.0f;
            const float Bot = NotifyY0 + kMontageNotifyHeight - 3.0f;

            const bool bSel = (SelectedKind == ESelectionKind::Notify && SelectedIndex == i);

            DrawList->AddLine(ImVec2(X, Top), ImVec2(X, Bot), kNotifyColor, 2.0f);
            DrawList->AddTriangleFilled(ImVec2(X, Top), ImVec2(X + 11.0f, Top + 4.0f), ImVec2(X, Top + 8.0f), kNotifyColor);
            if (bSel)
            {
                DrawList->AddCircleFilled(ImVec2(X, Bot), 3.0f, IM_COL32(255, 255, 255, 255));
            }

            const ImVec2 M = IO.MousePos;
            const bool bOver = bCanvasHovered && fabsf(M.x - X) <= 6.0f && M.y >= Top && M.y <= Bot;
            if (bOver)
            {
                ImGui::SetTooltip("%s @ %.3fs", Notify.Name.c_str(), Notify.Time);
            }
            if (DragMode == EDragMode::None && bOver && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedKind  = ESelectionKind::Notify;
                SelectedIndex = i;
                DragKind  = ESelectionKind::Notify;
                DragIndex = i;
                DragMode  = EDragMode::MoveItem;
                DragGrabOffset = 0.0f;
            }
        }

        DrawList->PopClipRect();

        // Playhead over everything.
        {
            const float PX = TimeToX(Playhead);
            if (PX >= LaneX0 - 1.0f && PX <= LaneX1 + 1.0f)
            {
                DrawList->PushClipRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, CanvasY1), true);
                DrawList->AddLine(ImVec2(PX, RulerY0), ImVec2(PX, CanvasY1), IM_COL32(255, 220, 60, 230), 1.5f);
                DrawList->AddTriangleFilled(ImVec2(PX - 6, RulerY0), ImVec2(PX + 6, RulerY0), ImVec2(PX, RulerY0 + 8), IM_COL32(255, 220, 60, 255));
                DrawList->PopClipRect();
            }
        }

        const ImRect RulerRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, RulerY0 + kMontageRulerHeight));
        if (DragMode == EDragMode::None && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && RulerRect.Contains(IO.MousePos))
        {
            DragMode = EDragMode::Playhead;
        }

        // Every item and the ruler set a DragMode, so an unclaimed press landed on empty canvas.
        if (DragMode == EDragMode::None && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ClearSelection();
        }

        if (DragMode != EDragMode::None)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const float RawT = XToTime(IO.MousePos.x);

                if (DragMode == EDragMode::Playhead)
                {
                    bIsPlaying = false;
                    Playhead = SnapTime(RawT, Duration);
                }
                else if (DragKind == ESelectionKind::Segment &&
                         DragTrack >= 0 && DragTrack < (int32)Montage->SlotTracks.size())
                {
                    SAnimMontageSlotTrack& Track = Montage->SlotTracks[DragTrack];
                    if (DragIndex >= 0 && DragIndex < (int32)Track.Segments.size())
                    {
                        SAnimMontageSegment& Segment = Track.Segments[DragIndex];
                        if (DragMode == EDragMode::ResizeEnd)
                        {
                            // The clip's content is fixed, so dragging the right edge retimes it.
                            const float NewLength = Math::Max(RawT - Segment.StartTime, 0.02f);
                            const float Content = Segment.GetTrimmedLength() * (float)Math::Max(Segment.LoopCount, 1);
                            Segment.PlayRate = Math::Clamp(Content / NewLength, 0.05f, 20.0f);
                        }
                        else
                        {
                            Segment.StartTime = Math::Max(SnapTime(RawT - DragGrabOffset, Duration * 4.0f), 0.0f);
                        }
                        MarkMontageDirty();
                    }
                }
                else if (DragKind == ESelectionKind::Section && DragIndex >= 0 && DragIndex < (int32)Montage->Sections.size())
                {
                    Montage->Sections[DragIndex].StartTime = SnapTime(RawT - DragGrabOffset, Duration);
                    MarkMontageDirty();
                }
                else if (DragKind == ESelectionKind::Notify && DragIndex >= 0 && DragIndex < (int32)Montage->Notifies.size())
                {
                    Montage->Notifies[DragIndex].Time = SnapTime(RawT, Duration);
                    MarkMontageDirty();
                }
                else if (DragKind == ESelectionKind::NotifyState && DragIndex >= 0 && DragIndex < (int32)Montage->NotifyStates.size())
                {
                    SAnimMontageNotifyState& State = Montage->NotifyStates[DragIndex];
                    const float T = SnapTime(RawT, Duration);
                    if (DragMode == EDragMode::ResizeStart)
                    {
                        State.StartTime = Math::Min(T, State.EndTime - 0.001f);
                    }
                    else if (DragMode == EDragMode::ResizeEnd)
                    {
                        State.EndTime = Math::Max(T, State.StartTime + 0.001f);
                    }
                    else
                    {
                        const float Len = State.EndTime - State.StartTime;
                        State.StartTime = Math::Clamp(SnapTime(RawT - DragGrabOffset, Duration), 0.0f, Math::Max(Duration - Len, 0.0f));
                        State.EndTime = State.StartTime + Len;
                    }
                    MarkMontageDirty();
                }
            }
            else
            {
                DragMode  = EDragMode::None;
                DragKind  = ESelectionKind::None;
                DragTrack = -1;
                DragIndex = -1;
            }
        }

        ImGui::EndChild();
    }

    void FAnimationMontageEditorTool::SortSections(CAnimationMontage* Montage)
    {
        // Reordering mid-drag would invalidate DragIndex, so leave the array alone until it ends.
        if (DragMode != EDragMode::None)
        {
            return;
        }

        Algo::StableSort(Montage->Sections.begin(), Montage->Sections.end(),
            [](const SAnimMontageSection& A, const SAnimMontageSection& B) { return A.StartTime < B.StartTime; });
    }

    void FAnimationMontageEditorTool::NotifyTrackCombo(CAnimationMontage* Montage, FName& Track, const char* StrId)
    {
        ImGui::SetNextItemWidth(Math::Max(ImGui::GetContentRegionAvail().x - 90.0f, 120.0f));
        if (ImGui::BeginCombo(StrId, Track.c_str()))
        {
            for (const FName& Lane : Montage->NotifyTracks)
            {
                if (ImGui::Selectable(Lane.c_str(), Lane == Track))
                {
                    Track = Lane;
                    MarkMontageDirty();
                }
            }
            ImGui::EndCombo();
        }
    }

    void FAnimationMontageEditorTool::DrawInspector(CAnimationMontage* Montage, float Duration)
    {
        char NameBuf[128];
        const float FieldWidth = Math::Max(ImGui::GetContentRegionAvail().x - 90.0f, 120.0f);

        switch (SelectedKind)
        {
        case ESelectionKind::Segment:
        {
            if (SelectedTrack < 0 || SelectedTrack >= (int32)Montage->SlotTracks.size())
            {
                break;
            }
            SAnimMontageSlotTrack& Track = Montage->SlotTracks[SelectedTrack];
            if (SelectedIndex < 0 || SelectedIndex >= (int32)Track.Segments.size())
            {
                break;
            }

            SAnimMontageSegment& Segment = Track.Segments[SelectedIndex];
            ImGui::TextDisabled(LE_ICON_ANIMATION " Segment on '%s'", Track.SlotName.c_str());

            const float ClipDuration = Segment.Animation.IsValid() ? Segment.Animation->GetDuration() : 0.0f;

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Start", &Segment.StartTime, 0.01f, 0.0f, 1000.0f, "%.3fs")) { MarkMontageDirty(); }
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Clip Start", &Segment.ClipStartTime, 0.01f, 0.0f, ClipDuration, "%.3fs")) { MarkMontageDirty(); }
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Clip End", &Segment.ClipEndTime, 0.01f, 0.0f, ClipDuration, "%.3fs")) { MarkMontageDirty(); }

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Rate", &Segment.PlayRate, 0.01f, 0.05f, 20.0f, "%.2fx")) { MarkMontageDirty(); }
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragInt("Loops", &Segment.LoopCount, 0.1f, 1, 64)) { MarkMontageDirty(); }
            ImGui::TextDisabled("occupies %.3fs", Segment.GetTimelineLength());
            break;
        }
        case ESelectionKind::Section:
        {
            if (SelectedIndex < 0 || SelectedIndex >= (int32)Montage->Sections.size())
            {
                break;
            }

            SAnimMontageSection& Section = Montage->Sections[SelectedIndex];
            ImGui::TextDisabled(LE_ICON_BOOKMARK " Section");

            snprintf(NameBuf, sizeof(NameBuf), "%s", Section.Name.c_str());
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf))) { Section.Name = FName(NameBuf); MarkMontageDirty(); }

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Start##Sec", &Section.StartTime, 0.01f, 0.0f, Duration, "%.3fs")) { MarkMontageDirty(); }

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::BeginCombo("Next", Section.NextSection.IsNone() ? "<end montage>" : Section.NextSection.c_str()))
            {
                if (ImGui::Selectable("<end montage>", Section.NextSection.IsNone()))
                {
                    Section.NextSection = FName();
                    MarkMontageDirty();
                }
                for (const SAnimMontageSection& Other : Montage->Sections)
                {
                    if (ImGui::Selectable(Other.Name.c_str(), Other.Name == Section.NextSection))
                    {
                        Section.NextSection = Other.Name;
                        MarkMontageDirty();
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ESelectionKind::Notify:
        {
            if (SelectedIndex < 0 || SelectedIndex >= (int32)Montage->Notifies.size())
            {
                break;
            }

            SAnimMontageNotify& Notify = Montage->Notifies[SelectedIndex];
            ImGui::TextDisabled(LE_ICON_BELL " Notify");

            snprintf(NameBuf, sizeof(NameBuf), "%s", Notify.Name.c_str());
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::InputText("Name##N", NameBuf, sizeof(NameBuf))) { Notify.Name = FName(NameBuf); MarkMontageDirty(); }

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Time##N", &Notify.Time, 0.01f, 0.0f, Duration, "%.3fs")) { MarkMontageDirty(); }

            NotifyTrackCombo(Montage, Notify.Track, "Track##N");

            if (ImGui::Button(LE_ICON_PLUS " Make Ranged"))
            {
                SAnimMontageNotifyState& State = Montage->NotifyStates.emplace_back();
                State.Name      = Notify.Name;
                State.Track     = Notify.Track;
                State.StartTime = Notify.Time;
                State.EndTime   = Math::Min(Notify.Time + Math::Max(Duration * 0.1f, 0.1f), Duration);
                Montage->Notifies.erase(Montage->Notifies.begin() + SelectedIndex);
                SelectedKind  = ESelectionKind::NotifyState;
                SelectedIndex = (int32)Montage->NotifyStates.size() - 1;
                MarkMontageDirty();
                break;
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Notify Type");
            if (DrawInstancedStructEditor("Type##N", Notify.Notify, SAnimNotify::StaticStruct(), NotifyDetails))
            {
                MarkMontageDirty();
            }
            break;
        }
        case ESelectionKind::NotifyState:
        {
            if (SelectedIndex < 0 || SelectedIndex >= (int32)Montage->NotifyStates.size())
            {
                break;
            }

            SAnimMontageNotifyState& State = Montage->NotifyStates[SelectedIndex];
            ImGui::TextDisabled(LE_ICON_BELL_OUTLINE " Notify State");

            snprintf(NameBuf, sizeof(NameBuf), "%s", State.Name.c_str());
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::InputText("Name##NS", NameBuf, sizeof(NameBuf))) { State.Name = FName(NameBuf); MarkMontageDirty(); }

            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("Start##NS", &State.StartTime, 0.01f, 0.0f, Duration, "%.3fs")) { MarkMontageDirty(); }
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("End##NS", &State.EndTime, 0.01f, 0.0f, Duration, "%.3fs")) { MarkMontageDirty(); }

            NotifyTrackCombo(Montage, State.Track, "Track##NS");

            ImGui::Spacing();
            ImGui::SeparatorText("Notify Type");
            if (DrawInstancedStructEditor("Type##NS", State.Notify, SAnimNotifyState::StaticStruct(), NotifyDetails))
            {
                MarkMontageDirty();
            }
            break;
        }
        default:
        {
            ImGui::TextDisabled("Select a segment, section or notify on the timeline to edit it.");
            return;
        }
        }

        if (SelectedKind != ESelectionKind::None)
        {
            ImGui::Spacing();
            if (ImGui::Button(LE_ICON_DELETE " Delete"))
            {
                DeleteSelected(Montage);
            }
        }
    }
}
