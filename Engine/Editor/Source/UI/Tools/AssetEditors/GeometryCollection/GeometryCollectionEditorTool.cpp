#include "GeometryCollectionEditorTool.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/GeometryCollection/GeometryCollection.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/World.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "imgui.h"

namespace Lumina
{
    FGeometryCollectionEditorTool::FGeometryCollectionEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset, NewObject<CWorld>())
    {
    }

    void FGeometryCollectionEditorTool::OnInitialize()
    {
        CreateToolWindow(FractureWindowName.data(), [this](bool /*bFocused*/)
        {
            DrawFractureWindow();
        });
    }

    void FGeometryCollectionEditorTool::SetupWorldForTool()
    {
        FEditorTool::SetupWorldForTool();

        World->GetRenderer()->GetSceneRenderSettings().bDrawBillboards = false;

        LightEntity = World->ConstructEntity("Directional Light");
        World->EmplaceComponent<SDirectionalLightComponent>(LightEntity);
        World->EmplaceComponent<SEnvironmentComponent>(LightEntity);
        World->EmplaceComponent<SSkyLightComponent>(LightEntity);

        CameraState.Speed = 5.0f;

        RebuildPreview();

        CGeometryCollection* Collection = Cast<CGeometryCollection>(Asset.Get());
        const FAABB Bounds = Collection ? Collection->GetFractureData().SourceBounds : FAABB(FVector3(-1.0f), FVector3(1.0f));
        const float Radius = Math::Max(Math::Length(Bounds.GetSize() * 0.5f), 0.5f);
        SetOrbitTarget(Bounds.GetCenter(), Radius * 3.0f);
        SetCameraMode(EEditorCameraMode::Orbit);
    }

    void FGeometryCollectionEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        if (!World.IsValid())
        {
            return;
        }

        // Spawning/destroying entities is deferred out of the ImGui draw into Update.
        if (bPreviewDirty)
        {
            RebuildPreview();
        }
    }

    void FGeometryCollectionEditorTool::ClearPreviewEntities()
    {
        if (World.IsValid())
        {
            for (ECS::FEntity Entity : PieceEntities)
            {
                if (Entity != ECS::NullEntity && World->IsValidEntity(Entity))
                {
                    World->DestroyEntity(Entity);
                }
            }
        }
        PieceEntities.clear();
        PieceMeshes.clear();
    }

    void FGeometryCollectionEditorTool::RebuildPreview()
    {
        bPreviewDirty = false;

        ClearPreviewEntities();

        CGeometryCollection* Collection = Cast<CGeometryCollection>(Asset.Get());
        if (Collection == nullptr || !World.IsValid())
        {
            return;
        }

        const int32 NumPieces = Collection->GetNumPieces();
        PieceMeshes.reserve(NumPieces);
        PieceEntities.reserve(NumPieces);

        for (int32 i = 0; i < NumPieces; ++i)
        {
            CStaticMesh* PieceMesh = Fracture::BuildPieceMesh(Collection->GetPiece(i), Collection->Materials, "PreviewPiece");
            PieceMeshes.push_back(PieceMesh);

            if (PieceMesh == nullptr)
            {
                PieceEntities.push_back(ECS::NullEntity);
                continue;
            }

            ECS::FEntity Entity = World->ConstructEntity("Piece");
            World->EmplaceComponent<SStaticMeshComponent>(Entity).SetStaticMesh(PieceMesh);
            PieceEntities.push_back(Entity);
        }

        ApplyExplode();
    }

    void FGeometryCollectionEditorTool::ApplyExplode()
    {
        CGeometryCollection* Collection = Cast<CGeometryCollection>(Asset.Get());
        if (Collection == nullptr || !World.IsValid())
        {
            return;
        }

        const FVector3 Center = Collection->GetFractureData().SourceBounds.GetCenter();
        const int32 Count = Math::Min((int32)PieceEntities.size(), Collection->GetNumPieces());

        for (int32 i = 0; i < Count; ++i)
        {
            const ECS::FEntity Entity = PieceEntities[i];
            if (Entity == ECS::NullEntity || !World->IsValidEntity(Entity))
            {
                continue;
            }

            // Piece meshes are recentered in BuildPieceMesh, so placing at the centroid reassembles the source.
            const FVector3 PieceCenter = Collection->GetPiece(i).Center;
            const FVector3 Delta = PieceCenter - Center;
            const float Length = Math::Length(Delta);
            const FVector3 Offset = (Length > 1e-4f ? Delta / Length : FVector3(0.0f)) * ExplodeAmount;
            World->GetComponent<STransformComponent>(Entity).SetLocation(PieceCenter + Offset);
        }
    }

    void FGeometryCollectionEditorTool::DrawFractureWindow()
    {
        CGeometryCollection* Collection = Cast<CGeometryCollection>(Asset.Get());
        if (Collection == nullptr)
        {
            return;
        }

        ImGui::SeparatorText("Fracture");
        ImGui::Spacing();

        uint32 TotalVerts = 0;
        uint32 TotalTris  = 0;
        for (int32 i = 0; i < Collection->GetNumPieces(); ++i)
        {
            TotalVerts += (uint32)Collection->GetPiece(i).Vertices.size();
            TotalTris  += (uint32)Collection->GetPiece(i).Indices.size() / 3;
        }

        ImGui::Text("Pieces: %d", Collection->GetNumPieces());
        ImGui::Text("Vertices: %u   Triangles: %u", TotalVerts, TotalTris);
        ImGui::Spacing();

        if (ImGui::Button("Generate Fracture", ImVec2(-FLT_MIN, 0.0f)))
        {
            Collection->Rebuild();
            Asset->GetPackage()->MarkDirty();
            bPreviewDirty = true;
        }
        ImGuiX::TextTooltip("Re-bake convex chunks from the source mesh using the settings below.");

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##Explode", &ExplodeAmount, 0.0f, 3.0f, "Explode: %.2f"))
        {
            ApplyExplode();
        }
        ImGuiX::TextTooltip("Preview only: pushes each chunk outward from the center so you can inspect the break.");

        ImGui::Spacing();
        ImGui::SeparatorText("Settings");
        ImGui::Spacing();
        PropertyTable.DrawTree();
    }

    void FGeometryCollectionEditorTool::DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize)
    {
        DrawCameraModeSelector();
    }

    void FGeometryCollectionEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &RightDockID, &LeftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(ViewportWindowName).c_str(), LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(FractureWindowName.data()).c_str(), RightDockID);
    }
}
