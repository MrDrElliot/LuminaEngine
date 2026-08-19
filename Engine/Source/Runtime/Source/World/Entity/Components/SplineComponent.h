#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "SplineComponent.generated.h"

namespace Lumina
{
    /** How a control point's tangents are produced. */
    REFLECT()
    enum class ESplineTangentMode : uint8
    {
        /** Catmull-Rom: tangents derived from the neighboring points, giving a smooth curve. */
        Auto,

        /** Straight segments: tangents point at the neighbors with no overshoot. */
        Linear,

        /** ArriveTangent / LeaveTangent are authored by hand and never recomputed. */
        User,
    };

    /** One control point of an SSplineComponent, in the owning entity's local space. */
    REFLECT()
    struct RUNTIME_API SSplinePoint
    {
        GENERATED_BODY()

        /** Local-space position of this control point. */
        PROPERTY(Editable, Units = "m")
        FVector3 Location = FVector3(0.0f);

        /** Incoming tangent (the curve's derivative as it arrives here). Recomputed unless TangentMode is User. */
        PROPERTY(Editable)
        FVector3 ArriveTangent = FVector3(0.0f);

        /** Outgoing tangent (the curve's derivative as it leaves here). Recomputed unless TangentMode is User. */
        PROPERTY(Editable)
        FVector3 LeaveTangent = FVector3(0.0f);

        /** Per-point scale, interpolated along the spline. Carried to the GPU for anything that wants to size along the curve. */
        PROPERTY(Editable)
        FVector3 Scale = FVector3(1.0f);

        /** Twist about the spline's own tangent, in degrees. Interpolated along the spline. */
        PROPERTY(Editable, Units = "deg")
        float Roll = 0.0f;

        /** Which tangent rule applies to this point. */
        PROPERTY(Editable)
        ESplineTangentMode TangentMode = ESplineTangentMode::Auto;
    };

    /**
     * An editable cubic-Hermite curve through a list of control points.
     *
     * The curve is parameterized by a "key" in [0, NumSegments]: the integer part selects the segment and
     * the fraction is the position within it. Key space is NOT arc length -- a constant key rate moves at a
     * varying speed. For constant-speed / distance-along queries use the arc-length table
     * (BuildSamples / SampleAtDistance), which is also what gets uploaded when bSendToGPU is set.
     *
     * Control points live in the entity's local space; the render extract bakes the entity transform in, so
     * everything the GPU sees is world space.
     */
    REFLECT(Component, Category = "Spline")
    struct RUNTIME_API SSplineComponent
    {
        GENERATED_BODY()

        /** Control points, in order along the curve. */
        PROPERTY(Editable, Category = "Spline")
        TVector<SSplinePoint> Points;

        /** When set, a closing segment joins the last point back to the first. */
        PROPERTY(Editable, Category = "Spline")
        bool bClosedLoop = false;

        /** Reference up vector; the per-sample up is this vector made perpendicular to the tangent, then rolled. */
        PROPERTY(Editable, Category = "Spline")
        FVector3 DefaultUpVector = FVector3(0.0f, 1.0f, 0.0f);

        /**
         * Upload this spline's points and its arc-length table to the GPU so compute/material shaders can
         * sample it. Off by default: a spline used purely as authoring data costs nothing.
         */
        PROPERTY(Editable, Category = "Spline|GPU")
        bool bSendToGPU = false;

        /**
         * Arc-length table density, in samples per segment. The table is resampled to be uniform in
         * DISTANCE, so a shader looks a sample up by dividing distance by the step -- no search. Higher is
         * a more faithful curve at more memory; the cost is (Segments * this) * 64 bytes.
         */
        PROPERTY(Editable, Category = "Spline|GPU", ClampMin = 2, ClampMax = 256)
        int32 SamplesPerSegment = 16;

        /** Draw the curve in the editor viewport even when the entity is not selected. */
        PROPERTY(Editable, Category = "Spline|Display")
        bool bDrawDebug = true;


        /** Number of curve segments; one fewer than the point count, plus the closing segment when looped. */
        int32 GetNumSegments() const;

        /** Upper bound of key space. Equals GetNumSegments(). */
        float GetKeyRange() const { return static_cast<float>(GetNumSegments()); }

        /**
         * Recompute ArriveTangent / LeaveTangent for every point whose TangentMode is not User.
         * Call after any edit to Points, bClosedLoop, or a point's TangentMode.
         */
        void UpdateTangents();

        /** Position on the curve at the given key (clamped to key space, or wrapped when looped). */
        FVector3 EvaluatePosition(float Key) const;

        /** Unnormalized derivative at the given key. */
        FVector3 EvaluateTangent(float Key) const;

        /** Interpolated per-point scale at the given key. */
        FVector3 EvaluateScale(float Key) const;

        /** Interpolated roll, in degrees, at the given key. */
        float EvaluateRoll(float Key) const;

        /** Up vector at the given key: DefaultUpVector orthogonalized against the tangent, then rolled. */
        FVector3 EvaluateUpVector(float Key) const;
    };

    /** One entry of a spline's arc-length table. Mirrors FGPUSplineSample. */
    struct FSplineSample
    {
        FVector3 Position       = FVector3(0.0f);
        float    DistanceAlong  = 0.0f;
        FVector3 Tangent        = FVector3(0.0f);   // normalized
        float    Key            = 0.0f;             // curve key this sample landed on
        FVector3 Up             = FVector3(0.0f, 1.0f, 0.0f);
        float    Roll           = 0.0f;             // degrees
        FVector3 Scale          = FVector3(1.0f);
        float    _Pad           = 0.0f;
    };

    /**
     * Build the arc-length table for a spline, optionally baking a transform into every sample.
     *
     * Two passes: a dense walk in key space accumulates chord length, then that polyline is resampled at a
     * uniform distance step. The result is uniform in DISTANCE, which is what makes a GPU lookup a divide
     * instead of a search. Returns the total length; OutSamples is sized (Segments * SamplesPerSegment) + 1.
     */
    RUNTIME_API float BuildSplineSamples(const SSplineComponent& Spline,
                                         const FMatrix4& LocalToWorld,
                                         TVector<FSplineSample>& OutSamples);

    /** Interpolate an arc-length table at a distance along the curve. Clamps at both ends. */
    RUNTIME_API FSplineSample SampleSplineAtDistance(const TVector<FSplineSample>& Samples,
                                                     float TotalLength,
                                                     float Distance);
}
