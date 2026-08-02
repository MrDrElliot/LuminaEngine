#pragma once

#include "Containers/Array.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "CurveAsset.generated.h"

namespace Lumina
{
    /** How the segment leaving a key is interpolated. Cubic recomputes its tangents from the
     *  neighbours (Catmull-Rom); CubicUser keeps whatever the curve editor authored. */
    REFLECT()
    enum class RUNTIME_API ECurveInterpMode : uint8
    {
        Constant,
        Linear,
        Cubic,
        CubicUser,
    };

    /** Behavior outside the keyed range, evaluated separately for each side. */
    REFLECT()
    enum class RUNTIME_API ECurveExtrapolation : uint8
    {
        Clamp,
        Cycle,
        CycleWithOffset,
        Oscillate,
        Linear,
    };

    /** One control point. Tangents are slopes in value/time units, so they survive time rescaling. */
    REFLECT()
    struct RUNTIME_API SCurveKey
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Key")
        float Time = 0.0f;

        PROPERTY(Editable, Category = "Key")
        float Value = 0.0f;

        PROPERTY(Editable, Category = "Key")
        ECurveInterpMode InterpMode = ECurveInterpMode::Linear;

        /** Slope of the segment arriving at this key. Cubic modes only. */
        PROPERTY(Editable, Category = "Key")
        float ArriveTangent = 0.0f;

        /** Slope of the segment leaving this key. Cubic modes only. */
        PROPERTY(Editable, Category = "Key")
        float LeaveTangent = 0.0f;

        /** When false the two tangents mirror each other; when true they are edited independently. */
        PROPERTY(Editable, Category = "Key")
        bool bTangentsBroken = false;

        bool IsCubic() const { return InterpMode == ECurveInterpMode::Cubic || InterpMode == ECurveInterpMode::CubicUser; }
    };

    /** One key pair baked to a cubic polynomial. Value(T) = A + B*U + C*U^2 + D*U^3 with
     *  U = (T - StartTime) / Duration. Plain data for codegen consumers, so it isn't reflected. */
    struct FCurveSegment
    {
        float StartTime = 0.0f;
        float Duration = 0.0f;
        float A = 0.0f;
        float B = 0.0f;
        float C = 0.0f;
        float D = 0.0f;
    };

    /** A float curve defined by time-sorted keys. Evaluation is allocation-free and safe on an
     *  empty curve (returns 0) or a single key (returns that key's value). */
    REFLECT()
    struct RUNTIME_API SKeyedCurve
    {
        GENERATED_BODY()

        float Evaluate(float InTime) const;

        /** Inserts a key at its sorted position and returns the index it landed on. */
        int32 AddKey(float InTime, float InValue);
        void RemoveKey(int32 Index);
        void SortKeys();

        /** Refreshes the tangents of every auto (Cubic) key. CubicUser keys are left alone. */
        void ComputeAutoTangents();

        int32 NumKeys() const { return (int32)Keys.size(); }
        bool IsEmpty() const { return Keys.empty(); }

        void GetTimeRange(float& OutMin, float& OutMax) const;
        void GetValueRange(float& OutMin, float& OutMax) const;

        /** Evaluate assuming InTime sits inside the keyed range. Public so the editor can sample
         *  the interpolated shape without paying for the extrapolation branch. */
        float EvaluateInRange(float InTime) const;

        /** Bakes one polynomial segment per key pair. Reproduces EvaluateInRange inside the keyed
         *  range, so consumers that cannot call Evaluate (shader codegen) stay exact. */
        void BakeSegments(TVector<FCurveSegment>& OutSegments) const;

        /** End slopes used by Linear extrapolation, matching Extrapolate(). */
        void GetExtrapolationSlopes(float& OutPreSlope, float& OutPostSlope) const;

        PROPERTY(Editable, Category = "Curve")
        TVector<SCurveKey> Keys;

        PROPERTY(Editable, Category = "Curve")
        ECurveExtrapolation PreExtrapolation = ECurveExtrapolation::Clamp;

        PROPERTY(Editable, Category = "Curve")
        ECurveExtrapolation PostExtrapolation = ECurveExtrapolation::Clamp;

    private:

        float Extrapolate(float InTime, ECurveExtrapolation Mode, bool bBefore) const;
    };

    /** Authorable float curve asset. Gameplay and animation systems sample it by time. */
    REFLECT()
    class RUNTIME_API CCurveAsset : public CObject
    {
        GENERATED_BODY()

    public:

        CCurveAsset();

        bool IsAsset() const override { return true; }

        float Evaluate(float InTime) const { return Curve.Evaluate(InTime); }

        PROPERTY(Editable, Category = "Curve")
        SKeyedCurve Curve;
    };
}
