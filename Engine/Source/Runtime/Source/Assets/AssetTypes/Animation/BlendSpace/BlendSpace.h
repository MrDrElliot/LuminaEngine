#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "BlendSpace.generated.h"

namespace Lumina
{
    class CAnimation;
    class CSkeleton;

    REFLECT()
    enum class EBlendSpaceAxes : uint8
    {
        // Samples live on a line; the value between the two bracketing samples is a straight lerp.
        One,
        // Samples live on a plane; the enclosing triangle's barycentric coordinates are the weights.
        Two,
    };

    REFLECT()
    enum class EBlendSpaceSmoothing : uint8
    {
        // The raw input drives the blend, so it moves the instant the input does.
        None,
        // Constant speed, covering the whole axis range in SmoothingTime.
        Linear,
        // Exponential approach, within a few percent of the input after SmoothingTime.
        ExponentialDecay,
        // Damped spring, so DampingRatio decides whether it settles in or overshoots and rings.
        SpringDamper,
    };

    REFLECT()
    struct RUNTIME_API SBlendSpaceAxis
    {
        GENERATED_BODY()

        /** Label shown on the editor grid; purely cosmetic. */
        PROPERTY(Editable, Category = "Axis")
        FName Name = "Speed";

        PROPERTY(Editable, Category = "Axis")
        float Min = 0.0f;

        PROPERTY(Editable, Category = "Axis")
        float Max = 1.0f;

        PROPERTY(Editable, Category = "Smoothing",
                 ToolTip = "How the input follows the value driving this axis, so a stick flick ramps instead of popping.")
        EBlendSpaceSmoothing Smoothing = EBlendSpaceSmoothing::None;

        PROPERTY(Editable, Category = "Smoothing", ClampMin = 0.0f, Delta = 0.005f, Units = "s",
                 EditCondition = "Smoothing != None", EditConditionHides,
                 ToolTip = "Seconds for the smoothed value to settle on a new input.")
        float SmoothingTime = 0.1f;

        PROPERTY(Editable, Category = "Smoothing", ClampMin = 0.0f, Delta = 0.01f,
                 EditCondition = "Smoothing == SpringDamper", EditConditionHides,
                 ToolTip = "Below 1 overshoots and rings, 1 settles without overshoot, above 1 crawls in.")
        float DampingRatio = 1.0f;

        PROPERTY(Editable, Category = "Smoothing", ClampMin = 0.0f,
                 EditCondition = "Smoothing != None", EditConditionHides,
                 ToolTip = "Ceiling on how fast the smoothed value travels, in axis units per second. 0 is unlimited.")
        float MaxSpeed = 0.0f;

        /** Normalized 0..1 position of Value within the axis range, clamped at both ends. */
        float Normalize(float Value) const;

        // Advances a follower one frame toward Target and returns it. Value and Velocity are per-instance.
        float Smooth(float Target, float DeltaTime, float& InOutValue, float& InOutVelocity) const;

        // Drops the follower onto Target, so a fresh instance starts where it is instead of easing up from zero.
        void SnapSmoothing(float Target, float& OutValue, float& OutVelocity) const;

        bool IsSmoothed() const { return Smoothing != EBlendSpaceSmoothing::None && SmoothingTime > 1e-4f; }
    };

    REFLECT()
    struct RUNTIME_API SBlendSpaceSample
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Sample")
        TObjectPtr<CAnimation> Animation;

        /** Where this sample sits in axis space. Y is ignored on a one-axis blend space. */
        PROPERTY(Editable, Category = "Sample")
        FVector2 Position = FVector2(0.0f, 0.0f);
    };

    // Indices into CBlendSpace::Samples; rebuilt from the samples rather than serialized.
    struct FBlendSpaceTriangle
    {
        int32 A = 0;
        int32 B = 0;
        int32 C = 0;
    };

    // The result of sampling a blend space: which samples contribute and how much. At most three, so
    // this stays a fixed-size POD the VM can put on the stack every frame.
    struct FBlendSpaceWeights
    {
        static constexpr int32 MaxContributions = 3;

        int32 SampleIndices[MaxContributions] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
        float Weights[MaxContributions] = { 0.0f, 0.0f, 0.0f };
        int32 Count = 0;

        void Add(int32 SampleIndex, float Weight);
    };

    // A set of animation clips placed in a 1D or 2D value space, blended by where an input lands
    // between them. The usual case is locomotion: speed on X, strafe on Y.
    REFLECT()
    class RUNTIME_API CBlendSpace : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }
        void PostLoad() override;

        /** Skeleton every sample clip is authored against. */
        PROPERTY(Editable, Category = "Blend Space")
        TObjectPtr<CSkeleton> Skeleton;

        PROPERTY(Editable, Category = "Blend Space")
        EBlendSpaceAxes AxisCount = EBlendSpaceAxes::Two;

        PROPERTY(Editable, Category = "Blend Space")
        SBlendSpaceAxis AxisX;

        PROPERTY(Editable, Category = "Blend Space")
        SBlendSpaceAxis AxisY;

        PROPERTY(Editable, Category = "Blend Space")
        TVector<SBlendSpaceSample> Samples;

        /** Which samples an input position blends between, and by how much. Weights always sum to 1
         *  when there is at least one usable sample; Count is 0 when the space is empty. */
        void Evaluate(const FVector2& Input, FBlendSpaceWeights& OutWeights) const;

        /** Weighted mean of the contributing clips' durations. This is what a synced phase advances
         *  against, so a walk/run blend keeps one stride rather than two competing ones. */
        float GetBlendedDuration(const FBlendSpaceWeights& Weights) const;

        /** Rebuilds the 2D triangulation. Call after editing sample positions; PostLoad does it once. */
        void RebuildTopology();

        const TVector<FBlendSpaceTriangle>& GetTriangles() const { return Triangles; }

        /** Sample indices sorted along X, for the one-axis path and for the editor's line view. */
        const TVector<int32>& GetSortedOrder() const { return SortedOrder; }

        /** False once Samples has been resized without a matching RebuildTopology. */
        bool HasValidTopology() const { return TopologySampleCount == (int32)Samples.size(); }

    private:

        void EvaluateOneAxis(float X, FBlendSpaceWeights& OutWeights) const;
        void EvaluateTwoAxis(const FVector2& Input, FBlendSpaceWeights& OutWeights) const;

        // Derived from Samples, never serialized: a stale triangulation loaded from disk would be worse
        // than no triangulation, and rebuilding is cheap at these counts.
        TVector<FBlendSpaceTriangle> Triangles;
        TVector<int32>               SortedOrder;
        int32                        TopologySampleCount = 0;
    };
}
