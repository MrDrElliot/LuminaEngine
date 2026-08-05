#include "RuntimePCH.h"
#include "Gradient.h"

#include "Core/Math/Math.h"
#include "EASTL/sort.h"

namespace Lumina
{
    FVector4 SGradient::Evaluate(float InTime) const
    {
        if (Keys.empty())
        {
            return FVector4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        if (Keys.size() == 1)
        {
            return Keys[0].Color;
        }

        // Clamp outside the keyed range. Extrapolating colour has no useful meaning (it leaves gamut
        // almost immediately), so unlike SKeyedCurve there is no per-side extrapolation mode.
        if (InTime <= Keys.front().Time)
        {
            return Keys.front().Color;
        }
        if (InTime >= Keys.back().Time)
        {
            return Keys.back().Color;
        }

        for (SIZE_T i = 1; i < Keys.size(); ++i)
        {
            const SGradientKey& Hi = Keys[i];
            if (InTime > Hi.Time)
            {
                continue;
            }

            const SGradientKey& Lo = Keys[i - 1];
            const float Span = Hi.Time - Lo.Time;
            // Coincident stops are a legal way to author a hard edge; take the later colour rather than
            // dividing by zero.
            if (Span <= 1e-6f)
            {
                return Hi.Color;
            }

            // Component-wise rather than Math::Lerp: the vector overloads are ambiguous for FVector4, and
            // spelling it out keeps alpha interpolating linearly with the colour rather than picking up
            // whatever a vector specialization does.
            const float Alpha = (InTime - Lo.Time) / Span;
            return FVector4(
                Lo.Color.x + (Hi.Color.x - Lo.Color.x) * Alpha,
                Lo.Color.y + (Hi.Color.y - Lo.Color.y) * Alpha,
                Lo.Color.z + (Hi.Color.z - Lo.Color.z) * Alpha,
                Lo.Color.w + (Hi.Color.w - Lo.Color.w) * Alpha);
        }

        return Keys.back().Color;
    }

    int32 SGradient::AddKey(float InTime, const FVector4& InColor)
    {
        SGradientKey Key;
        Key.Time  = InTime;
        Key.Color = InColor;

        // Insert in place rather than push-then-sort: Evaluate relies on the ordering, and a caller that
        // forgot to sort would read a silently wrong ramp instead of failing.
        SIZE_T Index = 0;
        while (Index < Keys.size() && Keys[Index].Time <= InTime)
        {
            ++Index;
        }
        Keys.insert(Keys.begin() + Index, Key);
        return (int32)Index;
    }

    void SGradient::RemoveKey(int32 Index)
    {
        if (Index >= 0 && Index < (int32)Keys.size())
        {
            Keys.erase(Keys.begin() + Index);
        }
    }

    void SGradient::SortKeys()
    {
        eastl::stable_sort(Keys.begin(), Keys.end(), [](const SGradientKey& A, const SGradientKey& B)
        {
            return A.Time < B.Time;
        });
    }

    void SGradient::GetTimeRange(float& OutMin, float& OutMax) const
    {
        if (Keys.empty())
        {
            OutMin = 0.0f;
            OutMax = 1.0f;
            return;
        }
        OutMin = Keys.front().Time;
        OutMax = Keys.back().Time;
    }
}
