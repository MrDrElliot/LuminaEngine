#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Physics/Ray/RayCast.h"
#include "FootPlacementComponent.generated.h"

namespace Lumina
{
    // One foot traced against the ground, and the graph parameters its result is written to.
    REFLECT()
    struct RUNTIME_API SFootPlacementFoot
    {
        GENERATED_BODY()

        /** Foot bone traced under. Its component-space position is where the trace starts. */
        PROPERTY(Editable, Category = "Foot", Picker = "Bone")
        FName FootBone;

        /** Height the foot bone sits above the sole in the bind pose, kept out of the ground. */
        PROPERTY(Editable, Category = "Foot")
        float SoleHeight = 0.0f;

        /** Graph parameters the component-space offset is written to. Wire these to a Foot IK node. */
        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName OffsetXParameter;

        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName OffsetYParameter;

        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName OffsetZParameter;

        /** Graph parameters the component-space ground normal is written to. */
        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName NormalXParameter;

        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName NormalYParameter;

        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName NormalZParameter;

        // Smoothed result, so a step onto a kerb eases in rather than snapping. Transient.
        FVector3 SmoothedOffset = FVector3(0.0f);
        FVector3 SmoothedNormal = FVector3(0.0f, 1.0f, 0.0f);

        // Resolved once against the graph's parameter struct. Transient.
        int32 BoneIndex = INDEX_NONE;

        // Inverse of the bone's inverse bind, which recovers a component-space transform. Transient.
        FMatrix4 BindMatrix = FMatrix4(1.0f);
    };

    // Traces under each foot and publishes the results as graph parameters for the IK nodes to consume.
    REFLECT(Component, Category = "Animation")
    struct RUNTIME_API SFootPlacementComponent
    {
        GENERATED_BODY()

        /** Feet traced for. Two for a biped, but nothing here assumes that. */
        PROPERTY(Editable, Category = "Foot Placement")
        TVector<SFootPlacementFoot> Feet;

        /** Off stops tracing and eases every offset back to zero, so it can be toggled per state. */
        PROPERTY(Editable, Category = "Foot Placement")
        bool bEnabled = true;

        /** World up, which the trace runs along. Matches the physics gravity direction by default. */
        PROPERTY(Editable, Category = "Foot Placement")
        FVector3 UpAxis = FVector3(0.0f, 1.0f, 0.0f);

        /** How far above the foot the trace starts, covering ground that rises ahead of the step. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Centimeters")
        float TraceUpDistance = 50.0f;

        /** How far below the foot the trace reaches before the foot is treated as airborne. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Centimeters")
        float TraceDownDistance = 100.0f;

        /** Ground the trace collides with. */
        PROPERTY(Editable, Category = "Trace")
        ECollisionProfiles TraceLayerMask = AllCollisionProfiles;

        /** Largest displacement any foot may take, so a bad trace cannot tear the leg apart. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Centimeters")
        float MaxOffset = 60.0f;

        /** Seconds for an offset to cover half the distance to its traced target. 0 snaps. */
        PROPERTY(Editable, Category = "Trace", ClampMin = 0.0f, Units = "Seconds")
        float SmoothingHalfLife = 0.08f;

        /** Graph parameters the pelvis offset is written to. Wire these to a Translate Bone node. */
        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName PelvisOffsetXParameter;

        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName PelvisOffsetYParameter;

        PROPERTY(Editable, Category = "Output", Picker = "Parameter")
        FName PelvisOffsetZParameter;

        // Smoothed pelvis drop, the deepest foot offset of the frame. Transient.
        FVector3 SmoothedPelvisOffset = FVector3(0.0f);

        // Parameter bindings resolved against the graph below; rebuilt when it changes. Transient.
        TVector<FAnimGraphParamBinding> Bindings;
        const void* BoundGraph = nullptr;
        const void* BoundSkeleton = nullptr;
    };
}
