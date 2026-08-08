#include "BlendSpaceEditorTool.h"

#include "Animation/TaskSystem/AnimTaskExecutor.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/Transactions/EditorTransaction.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "imgui.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    static const char* BlendSpaceGridWindowName = "Blend Grid";
    static const char* BlendSpaceDetailsWindowName   = "Details";

    static constexpr float SampleRadius = 7.0f;
    static constexpr float GrabRadius   = 11.0f;

    static const ImU32 GridLineColor    = IM_COL32(255, 255, 255, 18);
    static const ImU32 GridAxisColor    = IM_COL32(255, 255, 255, 55);
    static const ImU32 TriangleColor    = IM_COL32(90, 170, 255, 70);
    static const ImU32 SampleColor      = IM_COL32(90, 180, 255, 255);
    static const ImU32 SampleSelColor   = IM_COL32(255, 200, 60, 255);
    static const ImU32 SampleEmptyColor = IM_COL32(150, 90, 90, 255);
    static const ImU32 CursorColor      = IM_COL32(120, 255, 150, 255);

    FBlendSpaceEditorTool::FBlendSpaceEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    CSkeleton* FBlendSpaceEditorTool::GetSkeleton()
    {
        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();
        return BlendSpace != nullptr ? BlendSpace->Skeleton.Get() : nullptr;
    }

    FSkeletonResource* FBlendSpaceEditorTool::GetSkeletonResource()
    {
        CSkeleton* Skeleton = GetSkeleton();
        return Skeleton != nullptr ? Skeleton->GetSkeletonResource() : nullptr;
    }

    void FBlendSpaceEditorTool::OnInitialize()
    {
        SampleTable = MakeUnique<FPropertyTable>();
        SampleTable->SetPostEditCallback([this](const FPropertyChangedEvent&)
        {
            // A clip or position edit changes the topology the grid draws from.
            GetAsset<CBlendSpace>()->RebuildTopology();
            NotifyAssetDataChanged();
        });

        CreateToolWindow(BlendSpaceGridWindowName, [this](bool) { DrawGridWindow(); });
        CreateToolWindow(BlendSpaceDetailsWindowName, [this](bool) { DrawDetailsWindow(); });

        CachedSkeleton = GetSkeleton();
        GetAsset<CBlendSpace>()->RebuildTopology();
    }

    void FBlendSpaceEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        CreateFloorPlane();

        LightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(LightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(LightEntity);
        World->EmplaceComponent<SSkyLightComponent>(LightEntity);

        CameraState.Speed = 5.0f;

        RefreshPreviewMesh();
    }

    void FBlendSpaceEditorTool::RefreshPreviewMesh()
    {
        CachedSkeleton = GetSkeleton();

        if (!World.IsValid())
        {
            return;
        }

        if (MeshEntity != entt::null)
        {
            World->DestroyEntity(MeshEntity);
            MeshEntity = entt::null;
        }

        CSkeleton* Skeleton = GetSkeleton();
        if (Skeleton == nullptr || !Skeleton->PreviewMesh.IsValid())
        {
            return;
        }

        MeshEntity = World->ConstructEntity("PreviewMesh");
        SSkeletalMeshComponent& MeshComponent = World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity);
        MeshComponent.SetSkeletalMesh(Skeleton->PreviewMesh);
        Skeleton->ComputeBindPoseSkinningMatrices(MeshComponent.BoneTransforms);
        MeshComponent.bRenderBonesDirty = true;

        STransformComponent& MeshTransform = World->GetComponent<STransformComponent>(MeshEntity);
        STransformComponent& EditorTransform = World->GetComponent<STransformComponent>(EditorEntity);

        const FQuat LookAt = Math::FindLookAtRotation(MeshTransform.GetWorldLocation() + FVector3(0.0f, 0.9f, 0.0f), EditorTransform.GetLocation());
        EditorTransform.SetRotation(LookAt);
    }

    void FBlendSpaceEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();
        GetAsset<CBlendSpace>()->RebuildTopology();
        RefreshPreviewMesh();
    }

    void FBlendSpaceEditorTool::OnPostUndoRedo()
    {
        FAssetEditorTool::OnPostUndoRedo();

        // A restore rewrites Samples wholesale, so the details pointer into an element is stale and the
        // selection may now be past the end.
        SampleTarget = nullptr;

        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();
        if (SelectedSample >= (int32)BlendSpace->Samples.size())
        {
            SelectedSample = INDEX_NONE;
        }

        BlendSpace->RebuildTopology();
        SyncSampleTable();
    }

    void FBlendSpaceEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (!World.IsValid())
        {
            return;
        }

        if (CachedSkeleton.Get() != GetSkeleton())
        {
            RefreshPreviewMesh();
        }

        if (World->GetRenderer() != nullptr)
        {
            World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;
        }

        SyncSampleTable();
        EvaluatePreviewPose((float)UpdateContext.GetDeltaTime());
    }

    void FBlendSpaceEditorTool::EvaluatePreviewPose(float DeltaTime)
    {
        FSkeletonResource* Resource = GetSkeletonResource();
        if (Resource == nullptr || MeshEntity == entt::null)
        {
            return;
        }

        SSkeletalMeshComponent* MeshComponent = World->TryGetComponent<SSkeletalMeshComponent>(MeshEntity);
        if (MeshComponent == nullptr)
        {
            return;
        }

        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();

        FBlendSpaceWeights Weights;
        BlendSpace->Evaluate(PreviewPosition, Weights);

        if (Weights.Count == 0)
        {
            return;
        }

        const float BlendedDuration = BlendSpace->GetBlendedDuration(Weights);
        if (bPlaying && BlendedDuration > 1e-4f)
        {
            PreviewPhase += (DeltaTime * PlayRate) / BlendedDuration;
            PreviewPhase -= Math::Floor(PreviewPhase);
        }

        FAnimTaskList Tasks;
        Tasks.Skeleton = Resource;

        int16 Accumulated = FAnimTask::NoTask;
        float AccumulatedWeight = 0.0f;

        for (int32 i = 0; i < Weights.Count; ++i)
        {
            const SBlendSpaceSample& Sample = BlendSpace->Samples[Weights.SampleIndices[i]];

            FAnimTask Task;
            if (Sample.Animation.IsValid())
            {
                Task.Type = EAnimTaskType::SampleClip;
                Task.Clip = Sample.Animation.Get();
                Task.Time = PreviewPhase * Sample.Animation->GetDuration();
            }
            else
            {
                Task.Type = EAnimTaskType::ReferencePose;
            }

            const int16 SampleTask = Tasks.Add(Task);

            if (Accumulated == FAnimTask::NoTask)
            {
                Accumulated = SampleTask;
                AccumulatedWeight = Weights.Weights[i];
                continue;
            }

            AccumulatedWeight += Weights.Weights[i];

            FAnimTask BlendTask;
            BlendTask.Type = EAnimTaskType::Blend;
            BlendTask.DepA = Accumulated;
            BlendTask.DepB = SampleTask;
            BlendTask.Alpha = AccumulatedWeight > 1e-5f ? (Weights.Weights[i] / AccumulatedWeight) : 0.0f;

            Accumulated = Tasks.Add(BlendTask);
        }

        Tasks.OutputTask = Accumulated;

        Anim::ExecuteTaskList(Tasks, MeshComponent->BoneTransforms);
        MeshComponent->bRenderBonesDirty = true;
    }

    ImVec2 FBlendSpaceEditorTool::AxisToCanvas(const FVector2& AxisPosition) const
    {
        const CBlendSpace* BlendSpace = const_cast<FBlendSpaceEditorTool*>(this)->GetAsset<CBlendSpace>();

        const float U = BlendSpace->AxisX.Normalize(AxisPosition.x);
        const float V = (BlendSpace->AxisCount == EBlendSpaceAxes::Two)
            ? BlendSpace->AxisY.Normalize(AxisPosition.y) : 0.5f;

        // Canvas Y grows downward; axis Y grows upward, so V is flipped.
        return ImVec2(CanvasMin.x + U * CanvasSize.x, CanvasMin.y + (1.0f - V) * CanvasSize.y);
    }

    FVector2 FBlendSpaceEditorTool::CanvasToAxis(const ImVec2& CanvasPosition) const
    {
        CBlendSpace* BlendSpace = const_cast<FBlendSpaceEditorTool*>(this)->GetAsset<CBlendSpace>();

        const float U = Math::Clamp((CanvasPosition.x - CanvasMin.x) / Math::Max(CanvasSize.x, 1.0f), 0.0f, 1.0f);
        const float V = 1.0f - Math::Clamp((CanvasPosition.y - CanvasMin.y) / Math::Max(CanvasSize.y, 1.0f), 0.0f, 1.0f);

        const float X = Math::Mix(BlendSpace->AxisX.Min, BlendSpace->AxisX.Max, U);
        const float Y = (BlendSpace->AxisCount == EBlendSpaceAxes::Two)
            ? Math::Mix(BlendSpace->AxisY.Min, BlendSpace->AxisY.Max, V) : 0.0f;

        return FVector2(X, Y);
    }

    bool FBlendSpaceEditorTool::IsSnapActive() const
    {
        return (bool)bSnapEnabled != ImGui::GetIO().KeyCtrl;
    }

    FVector2 FBlendSpaceEditorTool::SnapToGrid(const FVector2& AxisPosition) const
    {
        if (!IsSnapActive() || SnapDivisions <= 0)
        {
            return AxisPosition;
        }

        CBlendSpace* BlendSpace = const_cast<FBlendSpaceEditorTool*>(this)->GetAsset<CBlendSpace>();

        const auto SnapAxis = [this](float Value, const SBlendSpaceAxis& Axis)
        {
            const float Step = (Axis.Max - Axis.Min) / (float)SnapDivisions;
            if (Math::Abs(Step) < 1e-6f)
            {
                return Value;
            }

            return Axis.Min + Math::Floor((Value - Axis.Min) / Step + 0.5f) * Step;
        };

        FVector2 Result = AxisPosition;
        Result.x = SnapAxis(AxisPosition.x, BlendSpace->AxisX);

        if (BlendSpace->AxisCount == EBlendSpaceAxes::Two)
        {
            Result.y = SnapAxis(AxisPosition.y, BlendSpace->AxisY);
        }

        return Result;
    }

    int32 FBlendSpaceEditorTool::FindSampleAtCanvasPos(const ImVec2& CanvasPosition) const
    {
        CBlendSpace* BlendSpace = const_cast<FBlendSpaceEditorTool*>(this)->GetAsset<CBlendSpace>();

        int32 Best = INDEX_NONE;
        float BestDistanceSq = GrabRadius * GrabRadius;

        for (int32 i = 0; i < (int32)BlendSpace->Samples.size(); ++i)
        {
            const ImVec2 Screen = AxisToCanvas(BlendSpace->Samples[i].Position);
            const float DX = CanvasPosition.x - Screen.x;
            const float DY = CanvasPosition.y - Screen.y;
            const float DistanceSq = DX * DX + DY * DY;

            if (DistanceSq <= BestDistanceSq)
            {
                BestDistanceSq = DistanceSq;
                Best = i;
            }
        }

        return Best;
    }

    void FBlendSpaceEditorTool::AddSampleAt(const FVector2& AxisPosition)
    {
        BeginAssetTransaction("Add Blend Sample");

        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();

        SBlendSpaceSample& Sample = BlendSpace->Samples.emplace_back();
        Sample.Position = AxisPosition;

        SelectedSample = (int32)BlendSpace->Samples.size() - 1;

        BlendSpace->RebuildTopology();
        NotifyAssetDataChanged();
        EndAssetTransaction();
    }

    void FBlendSpaceEditorTool::RemoveSampleAt(int32 SampleIndex)
    {
        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();
        if (SampleIndex < 0 || SampleIndex >= (int32)BlendSpace->Samples.size())
        {
            return;
        }

        BeginAssetTransaction("Remove Blend Sample");

        BlendSpace->Samples.erase(BlendSpace->Samples.begin() + SampleIndex);
        SelectedSample = INDEX_NONE;

        BlendSpace->RebuildTopology();
        NotifyAssetDataChanged();
        EndAssetTransaction();
    }

    void FBlendSpaceEditorTool::SyncSampleTable()
    {
        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();

        void* Target = nullptr;
        if (SelectedSample >= 0 && SelectedSample < (int32)BlendSpace->Samples.size())
        {
            Target = &BlendSpace->Samples[SelectedSample];
        }

        // Comparing the address catches a vector reallocation as well as a selection change.
        if (Target != SampleTarget)
        {
            SampleTarget = Target;
            if (Target != nullptr)
            {
                SampleTable->SetObject(Target, SBlendSpaceSample::StaticStruct());
                SampleTable->MarkDirty();
            }
        }
    }

    void FBlendSpaceEditorTool::BeginAssetTransaction(FName Name)
    {
        FTransactionManager& Manager = GetTransactionManager();
        Manager.BeginTransaction(Name);
        Manager.Record(MakeUnique<FObjectSnapshotCommand>(Asset.Get(), Name));
    }

    void FBlendSpaceEditorTool::EndAssetTransaction()
    {
        GetTransactionManager().CommitTransaction();
    }

    void FBlendSpaceEditorTool::DrawGridWindow()
    {
        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();

        bool bPlay = bPlaying;
        if (ImGui::Checkbox(bPlay ? LE_ICON_PAUSE "##Play" : LE_ICON_PLAY "##Play", &bPlay))
        {
            bPlaying = bPlay;
        }
        ImGuiX::TextTooltip("Advance the preview clock. Off holds the pose at the current phase.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("Rate", &PlayRate, 0.0f, 2.0f, "%.2fx");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Phase", &PreviewPhase, 0.0f, 1.0f, "%.2f");

        ImGui::SameLine();
        bool bTri = bShowTriangulation;
        if (ImGui::Checkbox("Mesh", &bTri))
        {
            bShowTriangulation = bTri;
        }
        ImGuiX::TextTooltip("Draw the triangulation the 2D weights come from.");

        ImGui::SameLine();
        bool bWeights = bShowWeights;
        if (ImGui::Checkbox("Weights", &bWeights))
        {
            bShowWeights = bWeights;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Same numbers the on-canvas readout shows, but editable, so an exact position can be dialled in
        // rather than nudged toward.
        {
            const FString AxisXName = BlendSpace->AxisX.Name.ToString();

            char XLabel[80];
            snprintf(XLabel, sizeof(XLabel), "%s##PreviewX", AxisXName.c_str());

            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::DragFloat(XLabel, &PreviewPosition.x, (BlendSpace->AxisX.Max - BlendSpace->AxisX.Min) * 0.005f,
                                 BlendSpace->AxisX.Min, BlendSpace->AxisX.Max, "%.1f"))
            {
                PreviewPosition.x = Math::Clamp(PreviewPosition.x, BlendSpace->AxisX.Min, BlendSpace->AxisX.Max);
            }

            if (BlendSpace->AxisCount == EBlendSpaceAxes::Two)
            {
                const FString AxisYName = BlendSpace->AxisY.Name.ToString();

                char YLabel[80];
                snprintf(YLabel, sizeof(YLabel), "%s##PreviewY", AxisYName.c_str());

                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::DragFloat(YLabel, &PreviewPosition.y, (BlendSpace->AxisY.Max - BlendSpace->AxisY.Min) * 0.005f,
                                     BlendSpace->AxisY.Min, BlendSpace->AxisY.Max, "%.1f"))
                {
                    PreviewPosition.y = Math::Clamp(PreviewPosition.y, BlendSpace->AxisY.Min, BlendSpace->AxisY.Max);
                }
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        bool bSnap = bSnapEnabled;
        if (ImGui::Checkbox("Snap", &bSnap))
        {
            bSnapEnabled = bSnap;
        }
        ImGuiX::TextTooltip("Snap samples to the gridlines. Hold Ctrl to invert: it snaps while the toggle is off, and frees the drag while it is on.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::DragInt("Divisions", &SnapDivisions, 0.2f, 1, 50))
        {
            SnapDivisions = Math::Clamp(SnapDivisions, 1, 50);
        }
        ImGuiX::TextTooltip("How many increments each axis is divided into. Drives the gridlines as well as the snap step.");

        ImGui::SameLine();
        ImGui::TextDisabled("%d samples", (int)BlendSpace->Samples.size());

        ImGui::Separator();

        DrawGridCanvas();
    }

    void FBlendSpaceEditorTool::DrawGridCanvas()
    {
        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();

        const ImVec2 Available = ImGui::GetContentRegionAvail();
        const float Margin = 46.0f;

        CanvasMin = ImVec2(ImGui::GetCursorScreenPos().x + Margin, ImGui::GetCursorScreenPos().y + 10.0f);
        CanvasSize = ImVec2(Math::Max(Available.x - Margin - 16.0f, 32.0f), Math::Max(Available.y - Margin, 32.0f));

        // Both axes need a positive size: InvisibleButton asserts on a zero extent, and the content region
        // is legitimately zero on the frame the window is first docked or while it is collapsed.
        ImGui::InvisibleButton("##BlendCanvas",
            ImVec2(Math::Max(Available.x, 32.0f), Math::Max(Available.y - 4.0f, 32.0f)));

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const ImVec2 CanvasMax = ImVec2(CanvasMin.x + CanvasSize.x, CanvasMin.y + CanvasSize.y);

        DrawList->AddRectFilled(CanvasMin, CanvasMax, IM_COL32(16, 17, 21, 255), 4.0f);
        DrawList->AddRect(CanvasMin, CanvasMax, IM_COL32(255, 255, 255, 40), 4.0f);

        const bool bTwoAxis = BlendSpace->AxisCount == EBlendSpaceAxes::Two;

        // Gridlines mark the snap increments, so what samples land on is what you can see. Brightened
        // while snapping is live to make the modifier's effect obvious before you commit to a drag.
        const bool bSnapping = IsSnapActive();
        const ImU32 MinorLineColor = bSnapping ? IM_COL32(255, 255, 255, 40) : GridLineColor;

        const int32 Divisions = Math::Max(SnapDivisions, 1);
        for (int32 i = 0; i <= Divisions; ++i)
        {
            const float T = (float)i / (float)Divisions;
            const float X = CanvasMin.x + T * CanvasSize.x;
            const float Y = CanvasMin.y + T * CanvasSize.y;

            const bool bEdge = (i == 0 || i == Divisions);

            DrawList->AddLine(ImVec2(X, CanvasMin.y), ImVec2(X, CanvasMax.y), bEdge ? GridAxisColor : MinorLineColor);
            if (bTwoAxis)
            {
                DrawList->AddLine(ImVec2(CanvasMin.x, Y), ImVec2(CanvasMax.x, Y), bEdge ? GridAxisColor : MinorLineColor);
            }
        }

        if (!bTwoAxis)
        {
            const float MidY = CanvasMin.y + CanvasSize.y * 0.5f;
            DrawList->AddLine(ImVec2(CanvasMin.x, MidY), ImVec2(CanvasMax.x, MidY), GridAxisColor, 2.0f);
        }

        char Label[64];
        snprintf(Label, sizeof(Label), "%s  %.0f", BlendSpace->AxisX.Name.c_str(), BlendSpace->AxisX.Min);
        DrawList->AddText(ImVec2(CanvasMin.x, CanvasMax.y + 6.0f), IM_COL32(190, 190, 200, 200), Label);

        snprintf(Label, sizeof(Label), "%.0f", BlendSpace->AxisX.Max);
        DrawList->AddText(ImVec2(CanvasMax.x - 30.0f, CanvasMax.y + 6.0f), IM_COL32(190, 190, 200, 200), Label);

        if (bTwoAxis)
        {
            snprintf(Label, sizeof(Label), "%s  %.0f", BlendSpace->AxisY.Name.c_str(), BlendSpace->AxisY.Max);
            DrawList->AddText(ImVec2(CanvasMin.x - Margin + 4.0f, CanvasMin.y), IM_COL32(190, 190, 200, 200), Label);

            snprintf(Label, sizeof(Label), "%.0f", BlendSpace->AxisY.Min);
            DrawList->AddText(ImVec2(CanvasMin.x - Margin + 4.0f, CanvasMax.y - 14.0f), IM_COL32(190, 190, 200, 200), Label);
        }

        if (bShowTriangulation && bTwoAxis)
        {
            for (const FBlendSpaceTriangle& Tri : BlendSpace->GetTriangles())
            {
                const ImVec2 A = AxisToCanvas(BlendSpace->Samples[Tri.A].Position);
                const ImVec2 B = AxisToCanvas(BlendSpace->Samples[Tri.B].Position);
                const ImVec2 C = AxisToCanvas(BlendSpace->Samples[Tri.C].Position);

                DrawList->AddTriangle(A, B, C, TriangleColor, 1.5f);
            }
        }

        // Contribution lines from the cursor, so it is obvious which clips are feeding the pose.
        FBlendSpaceWeights Weights;
        BlendSpace->Evaluate(PreviewPosition, Weights);

        const ImVec2 CursorScreen = AxisToCanvas(PreviewPosition);

        for (int32 i = 0; i < Weights.Count; ++i)
        {
            const ImVec2 SampleScreen = AxisToCanvas(BlendSpace->Samples[Weights.SampleIndices[i]].Position);
            const int32 Alpha = (int32)(40.0f + 180.0f * Weights.Weights[i]);
            DrawList->AddLine(CursorScreen, SampleScreen, IM_COL32(120, 255, 150, Alpha), 1.0f + 2.0f * Weights.Weights[i]);
        }

        for (int32 i = 0; i < (int32)BlendSpace->Samples.size(); ++i)
        {
            const SBlendSpaceSample& Sample = BlendSpace->Samples[i];
            const ImVec2 Screen = AxisToCanvas(Sample.Position);

            const bool bSelected = (i == SelectedSample);
            const ImU32 Color = !Sample.Animation.IsValid() ? SampleEmptyColor : (bSelected ? SampleSelColor : SampleColor);

            DrawList->AddCircleFilled(Screen, bSelected ? SampleRadius + 1.5f : SampleRadius, Color);
            DrawList->AddCircle(Screen, bSelected ? SampleRadius + 1.5f : SampleRadius, IM_COL32(10, 10, 14, 220), 0, 1.5f);

            const char* Name = Sample.Animation.IsValid() ? Sample.Animation->GetName().c_str() : "(no clip)";
            DrawList->AddText(ImVec2(Screen.x + 11.0f, Screen.y - 7.0f), IM_COL32(225, 228, 235, 220), Name);

            if (bShowWeights)
            {
                for (int32 w = 0; w < Weights.Count; ++w)
                {
                    if (Weights.SampleIndices[w] != i || Weights.Weights[w] <= 0.001f)
                    {
                        continue;
                    }

                    snprintf(Label, sizeof(Label), "%.0f%%", Weights.Weights[w] * 100.0f);
                    DrawList->AddText(ImVec2(Screen.x + 11.0f, Screen.y + 6.0f), IM_COL32(120, 255, 150, 230), Label);
                    break;
                }
            }
        }

        // Cursor last so it is never buried under a sample.
        DrawList->AddCircleFilled(CursorScreen, 5.0f, CursorColor);
        DrawList->AddCircle(CursorScreen, 10.0f, CursorColor, 0, 2.0f);

        // Crosshair out to the axes: reading the value off the gridlines alone is guesswork.
        DrawList->AddLine(ImVec2(CursorScreen.x, CanvasMin.y), ImVec2(CursorScreen.x, CanvasMax.y), IM_COL32(120, 255, 150, 45));
        if (bTwoAxis)
        {
            DrawList->AddLine(ImVec2(CanvasMin.x, CursorScreen.y), ImVec2(CanvasMax.x, CursorScreen.y), IM_COL32(120, 255, 150, 45));
        }

        // ToString rather than c_str: both names are alive at once here, and a numbered FName's c_str
        // comes from a short-lived rotating buffer.
        const FString AxisXName = BlendSpace->AxisX.Name.ToString();
        const FString AxisYName = BlendSpace->AxisY.Name.ToString();

        char CursorLabel[128];
        if (bTwoAxis)
        {
            snprintf(CursorLabel, sizeof(CursorLabel), "%s %.1f    %s %.1f",
                AxisXName.c_str(), PreviewPosition.x, AxisYName.c_str(), PreviewPosition.y);
        }
        else
        {
            snprintf(CursorLabel, sizeof(CursorLabel), "%s %.1f", AxisXName.c_str(), PreviewPosition.x);
        }

        const ImVec2 LabelSize = ImGui::CalcTextSize(CursorLabel);

        // Flip to the other side of the cursor near the edges so the readout never clips off-canvas.
        ImVec2 LabelPos(CursorScreen.x + 14.0f, CursorScreen.y - LabelSize.y - 12.0f);
        if (LabelPos.x + LabelSize.x + 8.0f > CanvasMax.x)
        {
            LabelPos.x = CursorScreen.x - LabelSize.x - 14.0f;
        }
        if (LabelPos.y < CanvasMin.y)
        {
            LabelPos.y = CursorScreen.y + 14.0f;
        }

        DrawList->AddRectFilled(ImVec2(LabelPos.x - 5.0f, LabelPos.y - 3.0f),
                                ImVec2(LabelPos.x + LabelSize.x + 5.0f, LabelPos.y + LabelSize.y + 3.0f),
                                IM_COL32(12, 14, 18, 220), 3.0f);
        DrawList->AddText(LabelPos, CursorColor, CursorLabel);

        HandleGridInput();
    }

    void FBlendSpaceEditorTool::HandleGridInput()
    {
        CBlendSpace* BlendSpace = GetAsset<CBlendSpace>();

        const bool bHovered = ImGui::IsItemHovered();
        const ImVec2 MousePos = ImGui::GetMousePos();

        if (DraggedSample != INDEX_NONE)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                DraggedSample = INDEX_NONE;
                if (bDragTransactionOpen)
                {
                    bDragTransactionOpen = false;
                    EndAssetTransaction();
                }
            }
            else if (DraggedSample < (int32)BlendSpace->Samples.size())
            {
                BlendSpace->Samples[DraggedSample].Position = SnapToGrid(CanvasToAxis(MousePos));
                BlendSpace->RebuildTopology();
                NotifyAssetDataChanged();
            }
            return;
        }

        if (!bHovered)
        {
            return;
        }

        // Double-click on empty space drops a new sample there; the clip is assigned in Details.
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && FindSampleAtCanvasPos(MousePos) == INDEX_NONE)
        {
            AddSampleAt(SnapToGrid(CanvasToAxis(MousePos)));
            return;
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const int32 Hit = FindSampleAtCanvasPos(MousePos);
            if (Hit != INDEX_NONE)
            {
                SelectedSample = Hit;
                DraggedSample = Hit;
                BeginAssetTransaction("Move Blend Sample");
                bDragTransactionOpen = true;
            }
            else
            {
                // Empty space moves the preview cursor: the whole point of the grid.
                PreviewPosition = CanvasToAxis(MousePos);
            }
        }
        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && DraggedSample == INDEX_NONE)
        {
            PreviewPosition = CanvasToAxis(MousePos);
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            const int32 Hit = FindSampleAtCanvasPos(MousePos);
            if (Hit != INDEX_NONE)
            {
                SelectedSample = Hit;
                ImGui::OpenPopup("##SampleContext");
            }
        }

        if (ImGui::BeginPopup("##SampleContext"))
        {
            if (ImGui::MenuItem(LE_ICON_DELETE " Remove Sample"))
            {
                RemoveSampleAt(SelectedSample);
            }
            ImGui::EndPopup();
        }
    }

    void FBlendSpaceEditorTool::DrawDetailsWindow()
    {
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Sample");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        if (SampleTarget != nullptr)
        {
            SampleTable->DrawTree();
        }
        else
        {
            ImGui::TextDisabled("Double-click the grid to add a sample, or click one to select it.");
        }

        ImGui::Spacing();
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::SeparatorText("Blend Space");
        ImGuiX::Font::PopFont();
        ImGui::Spacing();

        PropertyTable.DrawTree();
    }

    void FBlendSpaceEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);

        if (ImGui::BeginMenu(LE_ICON_CHART_SCATTER_PLOT " Blend Space"))
        {
            bool bTri = bShowTriangulation;
            if (ImGui::MenuItem("Show Triangulation", nullptr, &bTri))
            {
                bShowTriangulation = bTri;
            }

            bool bWeights = bShowWeights;
            if (ImGui::MenuItem("Show Weights", nullptr, &bWeights))
            {
                bShowWeights = bWeights;
            }

            ImGui::Separator();

            bool bSnap = bSnapEnabled;
            if (ImGui::MenuItem("Snap To Grid", "Ctrl inverts", &bSnap))
            {
                bSnapEnabled = bSnap;
            }

            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::DragInt("Snap Divisions", &SnapDivisions, 0.2f, 1, 50))
            {
                SnapDivisions = Math::Clamp(SnapDivisions, 1, 50);
            }

            ImGui::Separator();

            if (ImGui::MenuItem(LE_ICON_REFRESH " Rebuild Triangulation"))
            {
                GetAsset<CBlendSpace>()->RebuildTopology();
            }

            ImGui::EndMenu();
        }
    }

    void FBlendSpaceEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Samples",
            "Double-click empty grid space to add a sample, then assign its clip in Details. Drag a sample "
            "to move it; right-click to remove it.");
        DrawHelpTextRow("Preview",
            "Click or drag empty space to move the green cursor. The mesh plays exactly what the runtime "
            "would produce at that position, including the blend weights shown on each sample.");
        DrawHelpTextRow("Sync",
            "Every contributing clip is sampled at the same fraction of its own duration, against a phase "
            "driven by the weighted blend of their lengths. That is what keeps a walk/run blend on one stride.");
        DrawHelpTextRow("Snapping",
            "Samples snap to the gridlines, and the Divisions count drives both. Ctrl inverts the toggle "
            "rather than forcing it on, so the same key snaps a free drag and frees a snapped one. The "
            "preview cursor is never snapped, since scrubbing between samples is the point of it.");
        DrawHelpTextRow("Axes",
            "Axis names and ranges are on the asset. Positions are stored in axis units, so widening a range "
            "moves samples on the grid without changing their values.");
    }

    void FBlendSpaceEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID RightID = 0, CenterID = 0, BottomID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.26f, &RightID, &CenterID);
        ImGui::DockBuilderSplitNode(CenterID, ImGuiDir_Down, 0.45f, &BottomID, &CenterID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), CenterID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(BlendSpaceGridWindowName).c_str(), BottomID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(BlendSpaceDetailsWindowName).c_str(), RightID);
    }
}
