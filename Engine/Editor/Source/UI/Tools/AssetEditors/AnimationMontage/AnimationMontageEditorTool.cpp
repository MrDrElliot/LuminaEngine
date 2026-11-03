#include "Containers/StringFormat.h"
#include "AnimationMontageEditorTool.h"
#include "UI/Tools/AssetEditors/Animation/TimelineDrawing.h"
#include "World/ECS/Registry.h"
#include <string>
#include "UI/Properties/PropertyTable.h"
#include "Animation/AnimNotify.h"

#include <imgui_internal.h>

#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
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

        NotifyDetails.SetPostEditCallback([this](const FPropertyChangedEvent&)
        {
            MarkMontageDirty();
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

    void FAnimationMontageEditorTool::CreatePreviewEntity()
    {
        if (MeshEntity != ECS::NullEntity)
        {
            World->DestroyEntity(MeshEntity);
            MeshEntity = ECS::NullEntity;
        }

        CAnimationMontage* Montage = GetAsset<CAnimationMontage>();
        CSkeletalMesh* PreviewMesh = Montage != nullptr ? PreviewMeshSelection.Resolve(Montage->Skeleton.Get()) : nullptr;
        PreviewMeshInUse = PreviewMesh;
        if (PreviewMesh == nullptr)
        {
            return;
        }

        MeshEntity = World->ConstructEntity("MeshEntity");
        World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SetSkeletalMesh(PreviewMesh);

        SSimpleAnimationComponent& AnimComp = World->EmplaceComponent<SSimpleAnimationComponent>(MeshEntity);
        AnimComp.bPlaying = false;
        AnimComp.bLooping = false;
    }

    void FAnimationMontageEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        CAnimationMontage* Montage = GetAsset<CAnimationMontage>();
        if (World.IsValid() && Montage != nullptr && PreviewMeshSelection.Resolve(Montage->Skeleton.Get()) != PreviewMeshInUse.Get())
        {
            CreatePreviewEntity();
        }

        if (World && World->GetRenderer())
        {
            World->GetRenderSettings().bDrawBillboards = false;
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
        if (!World.IsValid() || MeshEntity == ECS::NullEntity)
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
        Algo::StableSort(Track.Segments,
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
                const FName Removed = Montage->Sections[SelectedIndex].Name;
                Montage->Sections.erase(Montage->Sections.begin() + SelectedIndex);
                for (SAnimMontageSection& Other : Montage->Sections)
                {
                    if (Other.NextSection == Removed)
                    {
                        Other.NextSection = FName();
                    }
                }
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

        CAnimationMontage* Montage = GetAsset<CAnimationMontage>();
        if (PreviewMeshSelection.DrawMenu(Montage != nullptr ? Montage->Skeleton.Get() : nullptr))
        {
            CreatePreviewEntity();
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
            "Point Next Section at itself to loop, and call JumpToMontageSection from script to chain a combo. "
            "Double-click the Sections row to add one, and right-click a section to pick its Next Section.");
        DrawHelpTextRow("Notifies",
            "Each notify track is its own row. Double-click a row to add a notify, right-click it for a notify state, "
            "and drag a state's edges to resize it. Use + Add in the corner for new slots, sections and notify tracks.");
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

        DrawTransport(Montage->GetDuration() > 0.0f ? Montage->GetDuration() : Duration);
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

        const bool bShortcutsActive = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
        const bool bSpacePressed = bShortcutsActive && ImGui::IsKeyPressed(ImGuiKey_Space, false);

        if (bShortcutsActive && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && SelectedKind != ESelectionKind::None)
        {
            if (CAnimationMontage* Montage = GetAsset<CAnimationMontage>())
            {
                DeleteSelected(Montage);
            }
        }

        if (ImGui::Button(bIsPlaying ? LE_ICON_PAUSE " Pause" : LE_ICON_PLAY " Play", ImVec2(96, 0)) || bSpacePressed)
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
        ImGui::BeginChild("MontageCanvas", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList*  DrawList   = ImGui::GetWindowDrawList();
        const ImVec2 CanvasPos  = ImGui::GetCursorScreenPos();
        const ImVec2 CanvasSize = ImGui::GetContentRegionAvail();
        const float  CanvasY1   = CanvasPos.y + CanvasSize.y;

        const int32 NumSlots  = (int32)Montage->SlotTracks.size();
        const int32 NumLanes  = (int32)Montage->NotifyTracks.size();
        constexpr float SectionRowH = 34.0f;
        constexpr float SlotRowH    = 44.0f;
        const float NotifyRowH = Timeline::TrackHeight;

        const float RulerY0     = CanvasPos.y;
        const float TrackAreaY0 = CanvasPos.y + Timeline::RulerHeight;
        const float ContentH    = SectionRowH + (float)NumSlots * SlotRowH + (float)NumLanes * NotifyRowH;
        const float MaxScroll   = Math::Max(0.0f, ContentH - (CanvasY1 - TrackAreaY0));
        TrackScroll = Math::Clamp(TrackScroll, 0.0f, MaxScroll);

        const float SectionY0 = TrackAreaY0 - TrackScroll;
        const float SlotsY0   = SectionY0 + SectionRowH;
        const float LanesY0   = SlotsY0 + (float)NumSlots * SlotRowH;

        Timeline::FView View;
        View.LaneX0    = CanvasPos.x + Timeline::HeaderWidth;
        View.LaneX1    = CanvasPos.x + CanvasSize.x;
        View.ContentX0 = View.LaneX0 + Timeline::LanePad;
        const float LaneX0   = View.LaneX0;
        const float LaneX1   = View.LaneX1;
        const float ContentW = Math::Max(LaneX1 - LaneX0 - Timeline::LanePad * 2.0f, 1.0f);
        const float BasePPS  = ContentW / Duration;
        View.PPS = BasePPS * TimelineZoom;
        TimelinePanSeconds = Math::Clamp(TimelinePanSeconds, 0.0f, Math::Max(0.0f, Duration - ContentW / View.PPS));
        View.Pan = TimelinePanSeconds;

        const ImGuiIO& IO = ImGui::GetIO();
        const ImVec2 Mouse = IO.MousePos;
        const bool bCanvasHovered = ImGui::IsWindowHovered();
        const bool bLanesHovered = bCanvasHovered && Mouse.y >= TrackAreaY0 && Mouse.x >= LaneX0 && Mouse.x <= LaneX1;

        if (bCanvasHovered && IO.MouseWheel != 0.0f && (Mouse.x <= LaneX0 || IO.KeyCtrl))
        {
            TrackScroll = Math::Clamp(TrackScroll - IO.MouseWheel * NotifyRowH, 0.0f, MaxScroll);
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

        auto LaneOf = [&](const FName& Track) -> int32
        {
            for (int32 n = 0; n < NumLanes; ++n)
            {
                if (Montage->NotifyTracks[n] == Track) return n;
            }
            return -1;
        };

        TVector<int32> LaneCounts(NumLanes, 0);
        for (const SAnimMontageNotify& N : Montage->Notifies)
        {
            const int32 Lane = LaneOf(N.Track);
            if (Lane >= 0) { ++LaneCounts[Lane]; }
        }
        for (const SAnimMontageNotifyState& S : Montage->NotifyStates)
        {
            const int32 Lane = LaneOf(S.Track);
            if (Lane >= 0) { ++LaneCounts[Lane]; }
        }

        const bool bInRows = bCanvasHovered && Mouse.y >= TrackAreaY0;
        const bool bSectionRowHovered = bInRows && Mouse.y >= SectionY0 && Mouse.y < SlotsY0;
        int32 HoveredSlot = -1;
        int32 HoveredLane = -1;
        if (bInRows && Mouse.y >= SlotsY0 && Mouse.y < LanesY0)
        {
            HoveredSlot = Math::Clamp((int32)floorf((Mouse.y - SlotsY0) / SlotRowH), 0, NumSlots - 1);
        }
        else if (bInRows && Mouse.y >= LanesY0 && Mouse.y < LanesY0 + (float)NumLanes * NotifyRowH)
        {
            HoveredLane = Math::Clamp((int32)floorf((Mouse.y - LanesY0) / NotifyRowH), 0, NumLanes - 1);
        }

        auto AddNotifyAt = [&](int32 Lane, float Time, bool bState)
        {
            const FName Track = (Lane >= 0 && Lane < NumLanes) ? Montage->NotifyTracks[Lane] : FName("Notifies");
            if (bState)
            {
                SAnimMontageNotifyState& State = Montage->NotifyStates.emplace_back();
                State.Name      = "NewNotifyState";
                State.Track     = Track;
                State.StartTime = Time;
                State.EndTime   = Math::Min(Time + Math::Max(Duration * 0.1f, 0.1f), Duration);
                SelectedKind  = ESelectionKind::NotifyState;
                SelectedIndex = (int32)Montage->NotifyStates.size() - 1;
            }
            else
            {
                SAnimMontageNotify& Notify = Montage->Notifies.emplace_back();
                Notify.Name  = "NewNotify";
                Notify.Time  = Time;
                Notify.Track = Track;
                SelectedKind  = ESelectionKind::Notify;
                SelectedIndex = (int32)Montage->Notifies.size() - 1;
            }
            MarkMontageDirty();
        };

        auto AddSectionAt = [&](float Time)
        {
            SAnimMontageSection& Section = Montage->Sections.emplace_back();
            Section.Name = FName(Format("Section{}", (int)Montage->Sections.size()).c_str());
            Section.StartTime = Time;
            SelectedKind  = ESelectionKind::Section;
            SelectedIndex = (int32)Montage->Sections.size() - 1;
            MarkMontageDirty();
        };

        DrawList->AddRectFilled(CanvasPos, ImVec2(LaneX1, CanvasY1), Timeline::CanvasColor);
        DrawList->AddRectFilled(CanvasPos, ImVec2(LaneX0, CanvasY1), Timeline::HeaderColor);
        DrawList->AddRectFilled(ImVec2(CanvasPos.x, RulerY0), ImVec2(LaneX1, TrackAreaY0), Timeline::RulerColor);

        ImGui::SetCursorScreenPos(ImVec2(CanvasPos.x + 8.0f, RulerY0 + (Timeline::RulerHeight - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Button(LE_ICON_PLUS " Add"))
        {
            ImGui::OpenPopup("montage_add");
        }
        if (ImGui::BeginPopup("montage_add"))
        {
            const float AtTime = SnapTime(Playhead, Duration);
            if (ImGui::MenuItem(LE_ICON_ANIMATION " Slot Track"))
            {
                SAnimMontageSlotTrack& Track = Montage->SlotTracks.emplace_back();
                Track.SlotName = FName(Format("Slot {}", (int)Montage->SlotTracks.size()).c_str());
                MarkMontageDirty();
            }
            if (ImGui::MenuItem(LE_ICON_BOOKMARK " Section at Playhead"))
            {
                AddSectionAt(AtTime);
            }
            if (ImGui::MenuItem(LE_ICON_BELL_RING " Notify Track"))
            {
                Montage->NotifyTracks.push_back(FName(Format("Track {}", NumLanes).c_str()));
                MarkMontageDirty();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(LE_ICON_BELL " Notify at Playhead"))
            {
                AddNotifyAt(SelectedNotifyTrack, AtTime, false);
            }
            if (ImGui::MenuItem(LE_ICON_BELL_OUTLINE " Notify State at Playhead"))
            {
                AddNotifyAt(SelectedNotifyTrack, AtTime, true);
            }
            ImGui::EndPopup();
        }

        SortSections(Montage);

        ImGui::PushClipRect(ImVec2(CanvasPos.x, TrackAreaY0), ImVec2(LaneX1, CanvasY1), true);

        {
            const float RowY1 = SectionY0 + SectionRowH;
            DrawList->AddRectFilled(ImVec2(LaneX0, SectionY0), ImVec2(LaneX1, RowY1), Timeline::RowAltColor);
            if (bSectionRowHovered)
            {
                DrawList->AddRectFilled(ImVec2(LaneX0, SectionY0), ImVec2(LaneX1, RowY1), Timeline::RowHoverColor);
            }
            DrawList->AddLine(ImVec2(CanvasPos.x, RowY1 - 0.5f), ImVec2(LaneX1, RowY1 - 0.5f), Timeline::SeparatorColor);
            Timeline::DrawTrackHeader(DrawList, ImVec2(CanvasPos.x, SectionY0), ImVec2(LaneX0, RowY1), FVector4(0.62f, 0.5f, 0.9f, 1.0f),
                                      LE_ICON_BOOKMARK, "Sections", (int32)Montage->Sections.size(), false, bSectionRowHovered && Mouse.x < LaneX0);
        }

        for (int32 t = 0; t < NumSlots; ++t)
        {
            SAnimMontageSlotTrack& Track = Montage->SlotTracks[t];
            const float RowY0 = SlotsY0 + (float)t * SlotRowH;
            const float RowY1 = RowY0 + SlotRowH;
            if (RowY0 > CanvasY1) break;
            if (RowY1 <= TrackAreaY0) continue;

            const bool bPreview = (PreviewTrack == t);
            const bool bHovered = (HoveredSlot == t);
            const FVector4 SlotColor = Timeline::PaletteColor(t + 1);

            DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), (t & 1) ? Timeline::RowAltColor : Timeline::RowColor);
            if (bHovered)
            {
                DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), Timeline::RowHoverColor);
            }
            DrawList->AddLine(ImVec2(CanvasPos.x, RowY1 - 0.5f), ImVec2(LaneX1, RowY1 - 0.5f),
                              t == NumSlots - 1 ? Timeline::SeparatorColor : Timeline::RowLineColor);
            Timeline::DrawTrackHeader(DrawList, ImVec2(CanvasPos.x, RowY0), ImVec2(LaneX0, RowY1), SlotColor,
                                      bPreview ? LE_ICON_EYE : nullptr, Track.SlotName.c_str(), (int32)Track.Segments.size(), bPreview, bHovered && Mouse.x < LaneX0);

            ImGui::SetCursorScreenPos(ImVec2(CanvasPos.x, RowY0));
            ImGui::PushID(t);
            if (ImGui::InvisibleButton("##SlotHeader", ImVec2(Timeline::HeaderWidth, SlotRowH)))
            {
                PreviewTrack = t;
            }
            ImGuiX::TextTooltip("{}", bPreview ? "The preview plays this slot" : "Click to preview this slot. Right-click for options.");

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
                if (NumSlots > 1 && ImGui::MenuItem(LE_ICON_DELETE " Delete Slot"))
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

            const ImRect LaneRect(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1));
            if (ImGui::BeginDragDropTargetCustom(LaneRect, ImGui::GetID("##SlotDrop")))
            {
                if (CAnimation* Dropped = DragDrop::AcceptAsset<CAnimation>())
                {
                    AppendSegment(Montage, t, Dropped, SnapTime(View.XToTime(Mouse.x), Duration * 4.0f));
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }

        for (int32 n = 0; n < NumLanes; ++n)
        {
            const float RowY0 = LanesY0 + (float)n * NotifyRowH;
            const float RowY1 = RowY0 + NotifyRowH;
            if (RowY0 > CanvasY1) break;
            if (RowY1 <= TrackAreaY0) continue;

            const bool bSelectedLane = (SelectedNotifyTrack == n);
            const bool bHovered = (HoveredLane == n);
            const FVector4 LaneColor = Timeline::PaletteColor(n);

            DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), (n & 1) ? Timeline::RowAltColor : Timeline::RowColor);
            if (bSelectedLane)
            {
                DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), Timeline::WithAlpha(LaneColor, 0.05f));
            }
            else if (bHovered)
            {
                DrawList->AddRectFilled(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1), Timeline::RowHoverColor);
            }
            DrawList->AddLine(ImVec2(CanvasPos.x, RowY1 - 0.5f), ImVec2(LaneX1, RowY1 - 0.5f), Timeline::RowLineColor);
            Timeline::DrawTrackHeader(DrawList, ImVec2(CanvasPos.x, RowY0), ImVec2(LaneX0, RowY1), LaneColor, nullptr,
                                      Montage->NotifyTracks[n].c_str(), LaneCounts[n], bSelectedLane, bHovered && Mouse.x < LaneX0);

            ImGui::SetCursorScreenPos(ImVec2(CanvasPos.x, RowY0));
            ImGui::PushID(2000 + n);
            if (ImGui::InvisibleButton("##LaneHeader", ImVec2(Timeline::HeaderWidth, NotifyRowH)))
            {
                SelectedNotifyTrack = n;
            }

            if (ImGui::BeginPopupContextItem("lane_ctx"))
            {
                static char RenameBuf[128];
                if (ImGui::IsWindowAppearing())
                {
                    snprintf(RenameBuf, sizeof(RenameBuf), "%s", Montage->NotifyTracks[n].c_str());
                }
                ImGui::SetNextItemWidth(160);
                if (ImGui::InputText("Name", RenameBuf, sizeof(RenameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    const FName PreviousName = Montage->NotifyTracks[n];
                    const FName NewName(RenameBuf);
                    Montage->NotifyTracks[n] = NewName;
                    for (SAnimMontageNotify& N : Montage->Notifies)
                    {
                        if (N.Track == PreviousName) { N.Track = NewName; }
                    }
                    for (SAnimMontageNotifyState& S : Montage->NotifyStates)
                    {
                        if (S.Track == PreviousName) { S.Track = NewName; }
                    }
                    MarkMontageDirty();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Notify"))       { AddNotifyAt(n, SnapTime(Playhead, Duration), false); }
                if (ImGui::MenuItem(LE_ICON_PLUS " Add Notify State")) { AddNotifyAt(n, SnapTime(Playhead, Duration), true); }
                ImGui::Separator();
                if (NumLanes > 1 && ImGui::MenuItem(LE_ICON_DELETE " Delete Track"))
                {
                    const FName Removed = Montage->NotifyTracks[n];
                    Montage->NotifyTracks.erase(Montage->NotifyTracks.begin() + n);
                    Montage->Notifies.erase(Algo::RemoveIf(Montage->Notifies, [&](const SAnimMontageNotify& N) { return N.Track == Removed; }), Montage->Notifies.end());
                    Montage->NotifyStates.erase(Algo::RemoveIf(Montage->NotifyStates, [&](const SAnimMontageNotifyState& S) { return S.Track == Removed; }), Montage->NotifyStates.end());
                    SelectedNotifyTrack = 0;
                    ClearSelection();
                    MarkMontageDirty();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            const ImRect LaneRect(ImVec2(LaneX0, RowY0), ImVec2(LaneX1, RowY1));
            if (bLanesHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && LaneRect.Contains(Mouse))
            {
                LaneAddTrack = n;
                LaneAddTime = SnapTime(View.XToTime(Mouse.x), Duration);
                ImGui::OpenPopup("notify_lane_add");
            }
        }
        ImGui::PopClipRect();

        if (ImGui::BeginPopup("notify_lane_add"))
        {
            ImGui::TextDisabled("at %.3fs (frame %d)", LaneAddTime, (int)roundf(LaneAddTime * (float)FrameRate));
            ImGui::Separator();
            if (ImGui::MenuItem(LE_ICON_BELL " Add Notify"))             { AddNotifyAt(LaneAddTrack, LaneAddTime, false); }
            if (ImGui::MenuItem(LE_ICON_BELL_OUTLINE " Add Notify State")) { AddNotifyAt(LaneAddTrack, LaneAddTime, true); }
            ImGui::EndPopup();
        }

        Timeline::DrawRuler(DrawList, View, RulerY0, TrackAreaY0, Math::Min(CanvasY1, LanesY0 + (float)NumLanes * NotifyRowH), Duration, FrameRate);

        DrawList->PushClipRect(ImVec2(LaneX0, TrackAreaY0), ImVec2(LaneX1, CanvasY1), true);
        bool bItemHovered = false;

        for (int32 i = 0; i < (int32)Montage->Sections.size(); ++i)
        {
            const SAnimMontageSection& Section = Montage->Sections[i];
            const float X0 = View.TimeToX(Section.StartTime);
            const float X1 = Math::Max(View.TimeToX(Montage->GetSectionEndTime(i)), X0 + 4.0f);
            const Timeline::FRangeGeometry Bar = Timeline::GetRangeGeometry(X0, X1, SectionY0 - 3.0f, SectionY0 + SectionRowH + 3.0f, Section.Name.c_str());

            const bool bHover = bLanesHovered && Mouse.x >= X0 && Mouse.x <= X1 && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1;
            const bool bSel   = (SelectedKind == ESelectionKind::Section && SelectedIndex == i);
            bItemHovered |= bHover;

            FFixedString Detail;
            if (Section.NextSection.IsNone())
            {
                Detail = LE_ICON_FLAG_CHECKERED " End";
            }
            else if (Section.NextSection == Section.Name)
            {
                Detail = LE_ICON_REPEAT " Loop";
            }
            else
            {
                Detail = FormatAs<FFixedString>("{} {}", LE_ICON_ARROW_RIGHT, Section.NextSection.c_str());
            }
            Timeline::DrawRange(DrawList, Bar, X0, X1, Timeline::PaletteColor(i + 4), Section.Name.c_str(), Detail.c_str(), bSel, bHover, false, false);

            if (bHover && DragMode == EDragMode::None)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (DragMode == EDragMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && bHover)
            {
                SelectedKind  = ESelectionKind::Section;
                SelectedIndex = i;
                DragKind = ESelectionKind::Section;
                DragIndex = i;
                DragMode = EDragMode::MoveItem;
                DragGrabOffset = View.XToTime(Mouse.x) - Section.StartTime;
            }
            if (bHover && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedKind  = ESelectionKind::Section;
                SelectedIndex = i;
                ImGui::OpenPopup("section_ctx");
            }
        }

        for (int32 t = 0; t < NumSlots; ++t)
        {
            SAnimMontageSlotTrack& Track = Montage->SlotTracks[t];
            const float RowY0 = SlotsY0 + (float)t * SlotRowH;
            const float RowY1 = RowY0 + SlotRowH;
            if (RowY1 <= TrackAreaY0 || RowY0 >= CanvasY1) continue;

            for (int32 s = 0; s < (int32)Track.Segments.size(); ++s)
            {
                SAnimMontageSegment& Segment = Track.Segments[s];
                const char* ClipName = Segment.Animation.IsValid() ? Segment.Animation->GetName().c_str() : "<empty>";
                const float X0 = View.TimeToX(Segment.StartTime);
                const float X1 = Math::Max(View.TimeToX(Segment.GetEndTime()), X0 + 4.0f);
                const Timeline::FRangeGeometry Bar = Timeline::GetRangeGeometry(X0, X1, RowY0, RowY1, ClipName);

                const bool bOverRight = bLanesHovered && fabsf(Mouse.x - X1) <= 5.0f && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1;
                const bool bOverBody  = bLanesHovered && Mouse.x >= X0 && Mouse.x <= X1 && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1;
                const bool bHover     = bOverBody || bOverRight;
                const bool bSel       = (SelectedKind == ESelectionKind::Segment && SelectedTrack == t && SelectedIndex == s);
                const bool bDragged   = (DragKind == ESelectionKind::Segment && DragTrack == t && DragIndex == s);
                bItemHovered |= bHover;

                const int32 Loops = Math::Max(Segment.LoopCount, 1);
                FFixedString Detail;
                if (fabsf(Segment.PlayRate - 1.0f) > 0.001f)
                {
                    Detail = FormatAs<FFixedString>("x{:.2f}", Segment.PlayRate);
                }
                if (Loops > 1)
                {
                    Detail += FormatAs<FFixedString>("{}{} loops", Detail.empty() ? "" : "  ", Loops);
                }

                Timeline::DrawRange(DrawList, Bar, X0, X1, Timeline::PaletteColor(t + 1), ClipName, Detail.empty() ? nullptr : Detail.c_str(),
                                    bSel, bHover, false, bOverRight || (bDragged && DragMode == EDragMode::ResizeEnd));

                for (int32 L = 1; L < Loops; ++L)
                {
                    const float LX = X0 + (X1 - X0) * ((float)L / (float)Loops);
                    for (float Y = Bar.BarY0 + 3.0f; Y < Bar.BarY1 - 3.0f; Y += 6.0f)
                    {
                        DrawList->AddLine(ImVec2(LX, Y), ImVec2(LX, Math::Min(Y + 3.0f, Bar.BarY1 - 3.0f)), IM_COL32(255, 255, 255, 120), 1.0f);
                    }
                }

                if (DragMode == EDragMode::None && bOverRight)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
                else if (DragMode == EDragMode::None && bOverBody)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s\nStarts at %.2fs, runs %.2fs\nRate x%.2f, %d loop(s)", ClipName, Segment.StartTime,
                                      Segment.GetTimelineLength(), Segment.PlayRate, Loops);
                }

                if (DragMode == EDragMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && bHover)
                {
                    SelectedKind  = ESelectionKind::Segment;
                    SelectedTrack = t;
                    SelectedIndex = s;
                    DragKind  = ESelectionKind::Segment;
                    DragTrack = t;
                    DragIndex = s;
                    DragMode  = bOverRight ? EDragMode::ResizeEnd : EDragMode::MoveItem;
                    DragGrabOffset = View.XToTime(Mouse.x) - Segment.StartTime;
                }
                if (bOverBody && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    SelectedKind  = ESelectionKind::Segment;
                    SelectedTrack = t;
                    SelectedIndex = s;
                    ImGui::OpenPopup("segment_ctx");
                }
            }

            if (HoveredSlot == t && bLanesHovered && Track.Segments.empty() && DragMode == EDragMode::None)
            {
                const char* Hint = "Drop an animation from the Content Browser here";
                const ImVec2 HintSize = ImGui::CalcTextSize(Hint);
                DrawList->AddText(ImVec2(LaneX0 + (LaneX1 - LaneX0 - HintSize.x) * 0.5f, RowY0 + (SlotRowH - HintSize.y) * 0.5f), Timeline::MutedTextColor, Hint);
            }
        }

        TVector<TVector<ImVec2>> LaneLabels(NumLanes);
        TVector<TVector<ImVec2>> LaneBars(NumLanes);

        for (int32 i = 0; i < (int32)Montage->NotifyStates.size(); ++i)
        {
            SAnimMontageNotifyState& State = Montage->NotifyStates[i];
            const int32 Lane = LaneOf(State.Track);
            if (Lane < 0) continue;

            const float RowY0 = LanesY0 + (float)Lane * NotifyRowH;
            const float RowY1 = RowY0 + NotifyRowH;
            if (RowY1 <= TrackAreaY0 || RowY0 >= CanvasY1) continue;

            const float X0 = View.TimeToX(State.StartTime);
            const float X1 = Math::Max(View.TimeToX(State.EndTime), X0 + 4.0f);
            const Timeline::FRangeGeometry Bar = Timeline::GetRangeGeometry(X0, X1, RowY0, RowY1, State.Name.c_str());
            LaneLabels[Lane].push_back(Timeline::RangeLabelSpan(Bar, X0, X1, State.Name.c_str()));
            LaneBars[Lane].push_back(ImVec2(X0, X1));
            const bool bOverTag = !Bar.bNameInside && Mouse.x >= Bar.Tag0.x && Mouse.x <= Bar.Tag1.x && Mouse.y >= Bar.Tag0.y && Mouse.y <= Bar.Tag1.y;

            const bool bOverLeft  = bLanesHovered && fabsf(Mouse.x - X0) <= 5.0f && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1;
            const bool bOverRight = bLanesHovered && fabsf(Mouse.x - X1) <= 5.0f && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1;
            const bool bOverBody  = bLanesHovered && ((Mouse.x >= X0 && Mouse.x <= X1 && Mouse.y >= Bar.BarY0 && Mouse.y <= Bar.BarY1) || bOverTag);
            const bool bHover     = bOverBody || bOverLeft || bOverRight;
            const bool bSel       = (SelectedKind == ESelectionKind::NotifyState && SelectedIndex == i);
            const bool bDragged   = (DragKind == ESelectionKind::NotifyState && DragIndex == i);
            bItemHovered |= bHover;

            char DurationText[24];
            snprintf(DurationText, sizeof(DurationText), "%.2fs", State.EndTime - State.StartTime);
            Timeline::DrawRange(DrawList, Bar, X0, X1, Timeline::PaletteColor(Lane), State.Name.c_str(), DurationText, bSel, bHover,
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
                SelectedKind  = ESelectionKind::NotifyState;
                SelectedIndex = i;
                SelectedNotifyTrack = Lane;
                DragKind  = ESelectionKind::NotifyState;
                DragIndex = i;
                DragMode  = bOverLeft ? EDragMode::ResizeStart : (bOverRight ? EDragMode::ResizeEnd : EDragMode::MoveItem);
                DragGrabOffset = View.XToTime(Mouse.x) - State.StartTime;
            }
            if (bOverBody && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedKind  = ESelectionKind::NotifyState;
                SelectedIndex = i;
                ImGui::OpenPopup("notify_item_ctx");
            }
        }

        for (int32 i = 0; i < (int32)Montage->Notifies.size(); ++i)
        {
            SAnimMontageNotify& Notify = Montage->Notifies[i];
            const int32 Lane = LaneOf(Notify.Track);
            if (Lane < 0) continue;

            const float RowY0 = LanesY0 + (float)Lane * NotifyRowH;
            const float RowY1 = RowY0 + NotifyRowH;
            if (RowY1 <= TrackAreaY0 || RowY0 >= CanvasY1) continue;

            const float X = View.TimeToX(Notify.Time);
            const Timeline::FPointGeometry Point = Timeline::GetPointGeometry(X, RowY0, RowY1, Notify.Name.c_str(), Timeline::Overlaps(LaneBars[Lane], X - 1.0f, X + 1.0f));
            const bool bSel  = (SelectedKind == ESelectionKind::Notify && SelectedIndex == i);
            const bool bShowTag = bSel || !Timeline::Overlaps(LaneLabels[Lane], Point.Tag0.x, Point.Tag1.x);
            if (bShowTag)
            {
                LaneLabels[Lane].push_back(ImVec2(Point.Tag0.x, Point.Tag1.x));
            }
            const bool bOver = bLanesHovered && Timeline::HitPoint(Point, X, Mouse, bShowTag);
            bItemHovered |= bOver;

            Timeline::DrawPoint(DrawList, Point, X, RowY0, RowY1, Timeline::PaletteColor(Lane), Notify.Name.c_str(), bSel, bOver, 0.0f, bShowTag);

            if (bOver && DragMode == EDragMode::None)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s  %.3fs (frame %d)", Notify.Name.c_str(), Notify.Time, (int)roundf(Notify.Time * (float)FrameRate));
            }
            if (DragMode == EDragMode::None && bOver && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedKind  = ESelectionKind::Notify;
                SelectedIndex = i;
                SelectedNotifyTrack = Lane;
                DragKind  = ESelectionKind::Notify;
                DragIndex = i;
                DragMode  = EDragMode::MoveItem;
                DragGrabOffset = View.XToTime(Mouse.x) - Notify.Time;
            }
            if (bOver && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                SelectedKind  = ESelectionKind::Notify;
                SelectedIndex = i;
                ImGui::OpenPopup("notify_item_ctx");
            }
        }

        if (HoveredLane >= 0 && bLanesHovered && !bItemHovered && DragMode == EDragMode::None && LaneCounts[HoveredLane] == 0)
        {
            const char* Hint = "Double-click to add a notify, right-click for more";
            const ImVec2 HintSize = ImGui::CalcTextSize(Hint);
            const float RowY0 = LanesY0 + (float)HoveredLane * NotifyRowH;
            DrawList->AddText(ImVec2(LaneX0 + (LaneX1 - LaneX0 - HintSize.x) * 0.5f, RowY0 + (NotifyRowH - HintSize.y) * 0.5f), Timeline::MutedTextColor, Hint);
        }

        DrawList->PopClipRect();

        Timeline::DrawPlayhead(DrawList, View, RulerY0, TrackAreaY0, Math::Min(CanvasY1, LanesY0 + (float)NumLanes * NotifyRowH), Playhead, FrameRate);

        DrawList->AddLine(ImVec2(CanvasPos.x, TrackAreaY0 - 0.5f), ImVec2(LaneX1, TrackAreaY0 - 0.5f), Timeline::SeparatorColor);
        DrawList->AddLine(ImVec2(LaneX0 - 0.5f, RulerY0), ImVec2(LaneX0 - 0.5f, CanvasY1), Timeline::SeparatorColor);

        if (MaxScroll > 0.0f)
        {
            const float AreaH  = CanvasY1 - TrackAreaY0;
            const float ThumbH = Math::Max(AreaH * AreaH / ContentH, 16.0f);
            const float ThumbY = TrackAreaY0 + (TrackScroll / MaxScroll) * (AreaH - ThumbH);
            DrawList->AddRectFilled(ImVec2(LaneX1 - 6.0f, ThumbY + 2.0f), ImVec2(LaneX1 - 2.0f, ThumbY + ThumbH - 2.0f), IM_COL32(255, 255, 255, 55), 2.0f);
        }

        if (ImGui::BeginPopup("section_ctx"))
        {
            if (SelectedKind == ESelectionKind::Section && SelectedIndex >= 0 && SelectedIndex < (int32)Montage->Sections.size())
            {
                SAnimMontageSection& Section = Montage->Sections[SelectedIndex];
                if (ImGui::BeginMenu(LE_ICON_ARROW_RIGHT " Next Section"))
                {
                    if (ImGui::MenuItem(LE_ICON_FLAG_CHECKERED " End montage", nullptr, Section.NextSection.IsNone()))
                    {
                        Section.NextSection = FName();
                        MarkMontageDirty();
                    }
                    for (const SAnimMontageSection& Other : Montage->Sections)
                    {
                        if (ImGui::MenuItem(Other.Name.c_str(), nullptr, Other.Name == Section.NextSection))
                        {
                            Section.NextSection = Other.Name;
                            MarkMontageDirty();
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(LE_ICON_MAP_MARKER " Move to playhead"))
                {
                    Section.StartTime = SnapTime(Playhead, Duration);
                    MarkMontageDirty();
                }
                if (ImGui::MenuItem(LE_ICON_DELETE " Delete"))
                {
                    DeleteSelected(Montage);
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("segment_ctx"))
        {
            if (ImGui::MenuItem(LE_ICON_ARROW_COLLAPSE_LEFT " Reflow Slot"))
            {
                ReflowTrack(Montage, SelectedTrack);
            }
            if (ImGui::MenuItem(LE_ICON_DELETE " Delete"))
            {
                DeleteSelected(Montage);
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("notify_item_ctx"))
        {
            if (ImGui::MenuItem(LE_ICON_MAP_MARKER " Move to playhead"))
            {
                const float AtTime = SnapTime(Playhead, Duration);
                if (SelectedKind == ESelectionKind::Notify && SelectedIndex >= 0 && SelectedIndex < (int32)Montage->Notifies.size())
                {
                    Montage->Notifies[SelectedIndex].Time = AtTime;
                }
                else if (SelectedKind == ESelectionKind::NotifyState && SelectedIndex >= 0 && SelectedIndex < (int32)Montage->NotifyStates.size())
                {
                    SAnimMontageNotifyState& State = Montage->NotifyStates[SelectedIndex];
                    const float Len = State.EndTime - State.StartTime;
                    State.StartTime = Math::Clamp(AtTime, 0.0f, Math::Max(Duration - Len, 0.0f));
                    State.EndTime = State.StartTime + Len;
                }
                MarkMontageDirty();
            }
            if (ImGui::MenuItem(LE_ICON_DELETE " Delete"))
            {
                DeleteSelected(Montage);
            }
            ImGui::EndPopup();
        }

        const ImRect RulerRect(ImVec2(LaneX0, RulerY0), ImVec2(LaneX1, TrackAreaY0));
        if (DragMode == EDragMode::None && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && RulerRect.Contains(Mouse))
        {
            DragMode = EDragMode::Playhead;
        }

        // Every item and the ruler set a DragMode, so an unclaimed press landed on empty canvas.
        if (DragMode == EDragMode::None && bCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        {
            ClearSelection();
            if (HoveredLane >= 0)
            {
                SelectedNotifyTrack = HoveredLane;
            }
        }

        if (DragMode == EDragMode::None && bLanesHovered && !bItemHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if (HoveredLane >= 0)
            {
                SelectedNotifyTrack = HoveredLane;
                AddNotifyAt(HoveredLane, SnapTime(View.XToTime(Mouse.x), Duration), false);
            }
            else if (bSectionRowHovered)
            {
                AddSectionAt(SnapTime(View.XToTime(Mouse.x), Duration));
            }
        }

        if (DragMode != EDragMode::None)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const float RawT = View.XToTime(Mouse.x);
                const bool bMoved = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
                if (DragMode == EDragMode::ResizeStart || DragMode == EDragMode::ResizeEnd)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }

                if (DragMode == EDragMode::Playhead)
                {
                    bIsPlaying = false;
                    Playhead = SnapTime(RawT, Duration);
                }
                else if (bMoved && DragKind == ESelectionKind::Segment &&
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
                            ImGui::SetTooltip("Rate x%.2f, runs %.2fs", Segment.PlayRate, Segment.GetTimelineLength());
                        }
                        else
                        {
                            Segment.StartTime = Math::Max(SnapTime(RawT - DragGrabOffset, Duration * 4.0f), 0.0f);
                            ImGui::SetTooltip("Starts at %.3fs", Segment.StartTime);
                        }
                        MarkMontageDirty();
                    }
                }
                else if (bMoved && DragKind == ESelectionKind::Section && DragIndex >= 0 && DragIndex < (int32)Montage->Sections.size())
                {
                    Montage->Sections[DragIndex].StartTime = SnapTime(RawT - DragGrabOffset, Duration);
                    MarkMontageDirty();
                    ImGui::SetTooltip("%.3fs (frame %d)", Montage->Sections[DragIndex].StartTime, (int)roundf(Montage->Sections[DragIndex].StartTime * (float)FrameRate));
                }
                else if (bMoved && DragKind == ESelectionKind::Notify && DragIndex >= 0 && DragIndex < (int32)Montage->Notifies.size())
                {
                    SAnimMontageNotify& Notify = Montage->Notifies[DragIndex];
                    Notify.Time = SnapTime(RawT - DragGrabOffset, Duration);
                    MarkMontageDirty();
                    ImGui::SetTooltip("%.3fs (frame %d)", Notify.Time, (int)roundf(Notify.Time * (float)FrameRate));
                }
                else if (bMoved && DragKind == ESelectionKind::NotifyState && DragIndex >= 0 && DragIndex < (int32)Montage->NotifyStates.size())
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
                    ImGui::SetTooltip("%.3fs to %.3fs (%.3fs)", State.StartTime, State.EndTime, State.EndTime - State.StartTime);
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

        const int32 Count = (int32)Montage->Sections.size();
        TVector<int32> Order(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Order[Index] = Index;
        }
        Algo::StableSort(Order, [Montage](int32 A, int32 B) { return Montage->Sections[A].StartTime < Montage->Sections[B].StartTime; });

        bool bReordered = false;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            bReordered |= Order[Index] != Index;
        }
        if (!bReordered)
        {
            return;
        }

        TVector<SAnimMontageSection> Sorted;
        Sorted.reserve(Count);
        int32 NewSelection = SelectedIndex;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Sorted.push_back(Montage->Sections[Order[Index]]);
            if (Order[Index] == SelectedIndex)
            {
                NewSelection = Index;
            }
        }
        Montage->Sections = Move(Sorted);
        if (SelectedKind == ESelectionKind::Section)
        {
            SelectedIndex = NewSelection;
        }
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
            if (ImGui::InputText("Name", NameBuf, sizeof(NameBuf)))
            {
                const FName OldName = Section.Name;
                const FName NewName(NameBuf);
                for (SAnimMontageSection& Other : Montage->Sections)
                {
                    if (Other.NextSection == OldName)
                    {
                        Other.NextSection = NewName;
                    }
                }
                Section.Name = NewName;
                MarkMontageDirty();
            }

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
            if (ImGui::DragFloat("Start##NS", &State.StartTime, 0.01f, 0.0f, State.EndTime - 0.001f, "%.3fs")) { MarkMontageDirty(); }
            ImGui::SetNextItemWidth(FieldWidth);
            if (ImGui::DragFloat("End##NS", &State.EndTime, 0.01f, State.StartTime + 0.001f, Duration, "%.3fs")) { MarkMontageDirty(); }

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

    void FAnimationMontageEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();

        // The selection is an index into arrays a restore may have shortened.
        ClearSelection();
    }
}
