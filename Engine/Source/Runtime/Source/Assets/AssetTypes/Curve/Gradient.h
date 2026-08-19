#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "Gradient.generated.h"

namespace Lumina
{
    /** One color stop. Time is the ramp position, conventionally 0..1 but not clamped, so a gradient can
     *  be authored over any domain (particle age in seconds, a distance, a temperature). */
    REFLECT()
    struct RUNTIME_API SGradientKey
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Gradient")
        float Time = 0.0f;

        PROPERTY(Editable, Category = "Gradient")
        FVector4 Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    };

    /** An authorable color ramp. Sibling to SCurve: same role for color that SCurve fills for scalars,
     *  and deliberately the same shape so both read the same way at a use site.
     *
     *  Inline only for now -- there is no CGradientAsset, because unlike curves there is no existing
     *  gradient asset to share. Adding one later means giving this the same bUseAsset/Asset pair SCurve
     *  has; consumers that go through Evaluate() would not change.
     *
     *  Keys are kept time-sorted by AddKey/SortKeys; Evaluate assumes that ordering. */
    REFLECT()
    struct RUNTIME_API SGradient
    {
        GENERATED_BODY()

        /** Color at InTime. Interpolates linearly in RGBA between the bracketing stops and clamps to the
         *  end stops outside the keyed range. An empty gradient evaluates to opaque white, which is the
         *  identity for the multiply most consumers do with it. */
        FVector4 Evaluate(float InTime) const;

        /** Inserts a stop at its sorted position and returns the index it landed on. */
        int32 AddKey(float InTime, const FVector4& InColor);
        void RemoveKey(int32 Index);
        void SortKeys();

        int32 NumKeys() const { return (int32)Keys.size(); }
        bool IsEmpty() const { return Keys.empty(); }

        void GetTimeRange(float& OutMin, float& OutMax) const;

        PROPERTY(Editable, Category = "Gradient")
        TVector<SGradientKey> Keys;
    };
}
