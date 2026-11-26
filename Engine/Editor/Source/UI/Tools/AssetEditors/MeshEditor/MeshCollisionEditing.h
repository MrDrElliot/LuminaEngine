#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"

#include "Assets/AssetTypes/Mesh/MeshCollision.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Physics/CollisionGen.h"
#include "UI/Tools/AssetEditors/ShapeHandles.h"

namespace Lumina
{
    class CStaticMesh;
    class CWorld;
    class FPropertyTable;
    class FTransactionManager;

    // Authoring of a static mesh's own collision for the mesh editor's Collision tab and viewport.
    class FMeshCollisionEditing : public IShapeHandleHost
    {
    public:

        explicit FMeshCollisionEditing(FTransactionManager& InTransactions);
        ~FMeshCollisionEditing() override;

        FMeshCollisionEditing(const FMeshCollisionEditing&) = delete;
        FMeshCollisionEditing& operator=(const FMeshCollisionEditing&) = delete;

        static const char* GetGeneratorLabel(Physics::CollisionGen::ECollisionGenerator Generator);
        static const char* GetGeneratorTooltip(Physics::CollisionGen::ECollisionGenerator Generator);

        void SetMesh(CStaticMesh* InMesh);

        // Adds what Generator makes from the mesh to the shapes already there; an Exact Mesh replaces them, since it cannot mix.
        void AddGenerated(Physics::CollisionGen::ECollisionGenerator Generator);
        void AddPrimitive(const SCollisionPrimitive& Primitive);
        void RemovePrimitiveAt(int32 Index);
        void ClearAll();

        // Tiles for every generator; click one to add it, or drag it onto the viewport or the shape list.
        void DrawPalette();
        void DrawShapeList();
        void DrawPrimitiveDetails();
        void DrawSettings();

        // Keeps the details panel on the selected piece and draws the collision into World.
        void Tick(CWorld* World);
        void TickViewport(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize, bool bViewportHovered, bool bSuppressClicks);
        void DrawViewMenuItems();

        void OnUndoRedo();
        void CloseOpenDragTransaction();
        void RebuildHullWireframes();

        bool HasSelection() const;

        bool bDrawShapes = true;

        // Fills each shape in as well as outlining it, which reads better where shapes overlap the mesh or each other.
        bool bDrawSolid = false;

    private:

        SMeshCollision* GetCollision() const;
        FMatrix4 GetPrimitiveMatrix(const SCollisionPrimitive& Primitive) const;
        void DrawPrimitives(CWorld* World);
        void DrawPrimitive(CWorld* World, const SCollisionPrimitive& Primitive, int32 Index, const FVector4& Color, float Thickness);
        int32 PickPrimitive(const FVector3& RayOrigin, const FVector3& RayDirection);
        void SyncDetailsTable();
        void MarkChanged();

        // A box, sphere or capsule dragged from the palette, sized to part of the mesh and centered under the cursor.
        bool MakeDroppedPrimitive(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize,
                                  Physics::CollisionGen::ECollisionGenerator Generator, SCollisionPrimitive& OutPrimitive) const;
        void TickViewportDrop(CWorld* World, const ImVec2& ViewportOrigin, const ImVec2& ViewportSize);

        void BeginTransaction(FName Name);
        void EndTransaction();

        //~ IShapeHandleHost
        FMatrix4 GetGizmoMatrix() override;
        void  ApplyGizmoMatrix(const FMatrix4& Matrix) override;
        void  GatherShapeHandles(TVector<FShapeHandle>& OutHandles) override;
        void  ApplyShapeHandleDrag(const FShapeHandle& Handle, const FVector3& RayOrigin, const FVector3& RayDirection) override;
        void  PickShapeAtRay(const FVector3& RayOrigin, const FVector3& RayDirection) override;
        FName GetMoveTransactionName() const override   { return "Move Collision Shape"; }
        FName GetRotateTransactionName() const override { return "Rotate Collision Shape"; }
        FName GetResizeTransactionName() const override { return "Resize Collision Shape"; }
        void  BeginShapeTransaction(FName Name) override { BeginTransaction(Name); }
        void  EndShapeTransaction() override             { EndTransaction(); }

        // Rebuilt from the points rather than serialized, so an edited hull is never drawn against a stale outline.
        struct FHullDrawData
        {
            TVector<FVector3> Vertices;
            TVector<uint32>   Edges;
            TVector<FVector3> SolidVertices;
            TVector<uint32>   SolidIndices;
        };

        FTransactionManager*           Transactions;
        TWeakObjectPtr<CStaticMesh>    Mesh;
        TUniquePtr<FPropertyTable>     DetailsTable;
        TUniquePtr<FPropertyTable>     SettingsTable;
        void*                          DetailsTarget = nullptr;
        TVector<FHullDrawData>         HullDrawData;
        int32                          SelectedPrimitive = Constants::kIndexNone;
        FShapeHandleState              HandleState;
        ImGuizmo::OPERATION            GizmoOp = ImGuizmo::TRANSLATE;
    };
}
