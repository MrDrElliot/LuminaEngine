#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include "ImGuizmo.h"
#include "Containers/String.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    class CWorld;
    struct SNameComponent;
    struct STransformComponent;
}

// Shared helpers for FWorldEditorTool and FPrefabEditorTool (editor-only tag set,
// transform/gizmo math, outliner row formatting) so the two tools agree by construction.
namespace Lumina::EditorEntityUtils
{
    /** True for tags/components that are tool-internal bookkeeping and must NOT be
     *  duplicated when copying an entity, nor serialized into a prefab. */
    bool IsEditorOnlyComponent(const entt::type_info& Type);
    bool IsEditorOnlyComponent(entt::id_type TypeHash);

    /** Standard filter for CWorld::DuplicateEntity, drops the editor-only set above
     *  so duplicates don't carry the source's selection / clipboard / dirty flags. */
    bool DefaultDuplicateFilter(const entt::type_info& Type);

    /** Translate → Rotate → Scale → Translate. */
    void CycleGizmoOp(ImGuizmo::OPERATION& InOutOp);

    /** WORLD ↔ LOCAL. */
    void ToggleGizmoMode(ImGuizmo::MODE& InOutMode);

    /** Decompose a world matrix into the entity's local transform (parent-aware); writes
     *  via the transform setters so the dirty flag is raised. Safe with no parent. */
    void ApplyWorldMatrixToTransform(FEntityRegistry& Registry, entt::entity Entity, const FMatrix4& WorldMatrix);

    /** Standard formatting for an entity's outliner row: "<icon> <name> - (<id>)". */
    FFixedString MakeOutlinerDisplayName(const SNameComponent* Name, entt::entity Entity, const char* Icon = LE_ICON_CUBE);

    /** Returns world-space center + radius covering Entity and descendants (mesh AABBs/
     *  transforms); false if nothing focusable. */
    bool ComputeFocusBoundsForEntity(FEntityRegistry& Registry, entt::entity Entity, FVector3& OutCenter, float& OutRadius);

    /** Single source of truth for selection/hover boxes: AABB+20% for meshes, else a unit box
     *  scaled by the transform. Returns false if the entity has no transform. */
    bool GetEntityDrawBox(FEntityRegistry& Registry, entt::entity Entity, FVector3& OutCenter, FVector3& OutHalfExtents, FQuat& OutRotation);

    /** Hover-style wireframe bounds box around any entity (via GetEntityDrawBox). No-op if unbounded. */
    void DrawEntityBounds(CWorld* World, entt::entity Entity, const FVector4& Color, float Thickness = 3.0f, bool bDepthTest = true);

    /** Selection-style corner-bracket box around any entity (via GetEntityDrawBox). No-op if unbounded. */
    void DrawEntitySelectionBox(CWorld* World, entt::entity Entity, const FVector4& Color, float CornerFraction = 0.2f, float Thickness = 5.0f, bool bDepthTest = true);

    /** Fills an empty world with the starting scene: environment, directional light, sky light, a static
     *  floor slab, and a dynamic sphere resting above it.
     *
     *  Shared by CWorldFactory (a newly created world asset) and the transient world the editor opens
     *  with when no project world is set, so the two cannot drift apart.
     */
    void PopulateDefaultScene(CWorld* World);

    /** Where the editor viewport camera should sit to frame PopulateDefaultScene, and the point it
     *  should look at. Kept next to the scene it frames -- moving the sphere without moving the camera
     *  is how a "default scene" ends up pointing at empty space. */
    void GetDefaultScenePreviewPose(FVector3& OutLocation, FVector3& OutTarget);
}
