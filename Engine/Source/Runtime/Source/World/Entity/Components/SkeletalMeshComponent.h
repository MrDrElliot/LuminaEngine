#pragma once
#include "MeshComponent.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Core/Math/AABB.h"
#include "Core/Object/ObjectMacros.h"
#include "SkeletalMeshComponent.generated.h"


namespace Lumina
{
    class CMaterialInterface;
    class CSkeletalMesh;

    /** Controls whether the animation pose is evaluated when the mesh isn't being rendered. */
    REFLECT()
    enum class EAnimUpdateMode : uint8
    {
        // Evaluates the pose every frame regardless of visibility.
        AlwaysTickPose,

        // Default, freezes the pose while the mesh has not been rendered recently.
        TickWhenRendered,
    };

    // CACHE_ALIGN because the gather writes the render-bone cache from workers and would false-share at range boundaries.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API CACHE_ALIGN SSkeletalMeshComponent : SMeshComponent
    {
        GENERATED_BODY()

        FUNCTION()
        CMaterialInterface* GetMaterialForSlot(uint32 Slot) const;

        /** Installs a transient instance over this slot so its parameters diverge for this component only. */
        FUNCTION()
        CMaterialInstance* CreateDynamicMaterialInstance(uint32 Slot);

        FUNCTION()
        SAABB GetAABB() const;

        /** How the animation pose updates relative to visibility (see EAnimUpdateMode). */
        PROPERTY(Editable, Category = "Animation")
        EAnimUpdateMode VisibilityBasedAnimTick = EAnimUpdateMode::TickWhenRendered;

        /**
         * Re-evaluate distant meshes' poses every 2-4 frames instead of every frame (staggered across
         * entities, skipped time accumulated so playback speed is unaffected). Big crowd CPU win;
         * disable for hero characters that must stay frame-exact at any distance.
         */
        PROPERTY(Editable, Category = "Animation")
        bool bUpdateRateOptimization = true;

        // Camera distance over bounding radius stamped by the render gather, and 0 evaluates at full rate. Transient.
        float LastDistanceOverRadius = 0.0f;

        // Mesh-local error the camera tolerates at this mesh, stamped by the render gather; 0 animates every bone. Transient.
        float LastBoneLODTolerance = 0.0f;

        // Animation time accrued while evaluation was skipped, consumed as one step on the next evaluated frame. Transient.
        float PendingAnimTime = 0.0f;

        // Frames until the next evaluation, and -1 seeds the entity-id phase stagger. Transient.
        int16 AnimSkipCounter = -1;

        // World time the render gather last kept this mesh through culling, and -1 until first rendered. Transient.
        double LastRenderedTime = -1.0;

        // Latest LastRenderedTime among the meshes following this one, so a hidden leader still animates them. Transient.
        double FollowerRenderedTime = -1.0;
        
        /** Mirrors SStaticMeshComponent::SetStaticMesh. Assigning the field directly skips
         *  InvalidateRenderResolve, and the retained render scene only re-reads primitives that report a
         *  change -- so a direct write is simply never picked up. */
        FUNCTION()
        void SetSkeletalMesh(CSkeletalMesh* InMesh) { SkeletalMesh = InMesh; InvalidateRenderResolve(); }

        FUNCTION()
        CSkeletalMesh* GetSkeletalMesh() const { return SkeletalMesh.Get(); }

        /** The skeletal mesh asset to render and animate for this component. */
        PROPERTY(Editable, Replicated, Category = "Mesh")
        TStrongObjectPtr<CSkeletalMesh> SkeletalMesh;

        // Sized to the skeleton's bone count by the animation system, and the render scene uploads exactly this many matrices.
        TVector<FMatrix4> BoneTransforms;

        // This frame's pose recipe, filled by the update phase and consumed by the execution phase. Transient.
        FAnimTaskList AnimTasks;

        // Raised by anything that writes BoneTransforms without bumping PoseSerial, and the gather folds it in.
        bool bRenderBonesDirty = false;

        // Bumped whenever BoneTransforms is rewritten, so the gather packs and uploads only a moved pose.
        uint32 PoseSerial = 0;

        // BoneTransforms in the GPU bone layout at two vectors a bone, packed while the pose was still in cache; current only while PackedPoseSerial equals PoseSerial.
        TVector<FVector4> PackedBones;
        uint32 PackedPoseSerial = 0;

        // Hash of the last pose the animation system wrote, so an unchanged pose is caught without reading the cold matrices back.
        uint64 PoseHash = 0;

        // The recipe BoneTransforms was last built from, so an identical recipe skips the whole execute.
        FAnimTaskList LastRecipe;
        bool bLastRecipeValid = false;
    };
}
