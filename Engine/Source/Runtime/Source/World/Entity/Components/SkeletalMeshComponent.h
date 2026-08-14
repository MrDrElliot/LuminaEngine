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
        // Evaluate the pose every frame regardless of visibility (use for gameplay-critical
        // skeletons whose pose must stay current even off-screen).
        AlwaysTickPose,

        // Skip pose evaluation while the mesh hasn't been rendered recently (the pose freezes
        // and resumes when it comes back on-screen). Default -- saves CPU on off-screen crowds.
        TickWhenRendered,
    };

    // CACHE_ALIGN: the gather writes LastRenderedTime / LastDistanceOverRadius / the render-bone cache from
    // worker threads, so unaligned components would false-share at parallel range boundaries.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API CACHE_ALIGN SSkeletalMeshComponent : SMeshComponent
    {
        GENERATED_BODY()

        CMaterialInterface* GetMaterialForSlot(size_t Slot) const;

        FUNCTION(Script)
        FAABB GetAABB() const;

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

        // Screen-size proxy (camera distance / bounding radius) stamped by the render gather; drives
        // the update-rate interval. 0 (never rendered) evaluates at full rate. Transient.
        float LastDistanceOverRadius = 0.0f;

        // Animation time accrued while update-rate optimization skipped evaluation; consumed as one
        // larger step on the next evaluated frame. Transient.
        float PendingAnimTime = 0.0f;

        // Update-rate countdown: frames until the next evaluation. -1 seeds the entity-id phase
        // stagger on the next reduced-rate frame (self-contained per component, so it works no
        // matter how many worlds tick per frame). Transient.
        int16 AnimSkipCounter = -1;

        // World time the render gather last kept this mesh through culling; animation systems read it
        // to gate off-screen pose evaluation. -1 until first rendered. Transient, never serialized.
        double LastRenderedTime = -1.0;
        
        /** Mirrors SStaticMeshComponent::SetStaticMesh. Assigning the field directly skips
         *  InvalidateRenderResolve, and the retained render scene only re-reads primitives that report a
         *  change -- so a direct write is simply never picked up. */
        FUNCTION(Script)
        void SetSkeletalMesh(CSkeletalMesh* InMesh) { SkeletalMesh = InMesh; InvalidateRenderResolve(); }

        FUNCTION(Script)
        CSkeletalMesh* GetSkeletalMesh() const { return SkeletalMesh; }

        /** The skeletal mesh asset to render and animate for this component. */
        PROPERTY(Editable, Replicated, Category = "Mesh")
        TObjectPtr<CSkeletalMesh> SkeletalMesh;

        // Sized to the skeleton's bone count by the animation system (0 when unused). The render scene
        // uploads exactly this many matrices; FGPUInstance.BoneOffset references this instance's slice.
        TVector<FMatrix4> BoneTransforms;

        // This frame's recorded pose recipe: the animation system's update phase fills it, the
        // execution phase consumes it into BoneTransforms. Transient, capacity reused across frames.
        FAnimTaskList AnimTasks;

        // BoneTransforms packed into the GPU 3x4-row layout (3 FVector4 rows per bone, aliases
        // FBoneTransform); the render gather bulk-copies this instead of converting per bone per
        // frame. Repacked only when the pose changes: writers of BoneTransforms either call
        // SkeletalUtils::PackRenderBones or set bRenderBonesDirty so the gather repacks lazily.
        TVector<FVector4> RenderBones;
        bool bRenderBonesDirty = false;
    };
}
