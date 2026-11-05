#include "RuntimePCH.h"
#include "Gradient.h"

#include "Core/Math/Math.h"

namespace Lumina
{
    FVector4 SGradient::Evaluate(float InTime) const
    {
        if (Keys.IsEmpty())
        {
            return FVector4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        if (Keys.Num() == 1)
        {
            return Keys[0].Color;
        }

        // Extrapolating color leaves gamut almost immediately, so there is no per-side mode here.
        if (InTime <= Keys[0].Time)
        {
            return Keys[0].Color;
        }
        if (InTime >= Keys.Last().Time)
        {
            return Keys.Last().Color;
        }

        for (int32 i = 1; i < Keys.Num(); ++i)
        {
            const SGradientKey& Hi = Keys[i];
            if (InTime > Hi.Time)
            {
                continue;
            }

            const SGradientKey& Lo = Keys[i - 1];
            const float Span = Hi.Time - Lo.Time;
            // Coincident stops are a legal hard edge, so take the later color rather than divide by zero.
            if (Span <= 1e-6f)
            {
                return Hi.Color;
            }

            // Spelled out so alpha interpolates linearly rather than picking up a vector specialization.
            const float Alpha = (InTime - Lo.Time) / Span;
            return FVector4(
                Lo.Color.x + (Hi.Color.x - Lo.Color.x) * Alpha,
                Lo.Color.y + (Hi.Color.y - Lo.Color.y) * Alpha,
                Lo.Color.z + (Hi.Color.z - Lo.Color.z) * Alpha,
                Lo.Color.w + (Hi.Color.w - Lo.Color.w) * Alpha);
        }

        return Keys.Last().Color;
    }

    int32 SGradient::AddKey(float InTime, const FVector4& InColor)
    {
        SGradientKey Key;
        Key.Time  = InTime;
        Key.Color = InColor;

        // Evaluate relies on the ordering, so a caller that forgot to sort would read a wrong ramp.
        int32 Index = 0;
        while (Index < Keys.Num() && Keys[Index].Time <= InTime)
        {
            ++Index;
        }
        Keys.Insert(Key, Index);
        return (int32)Index;
    }

    void SGradient::RemoveKey(int32 Index)
    {
        if (Index >= 0 && Index < Keys.Num())
        {
            Keys.RemoveAt(Index);
        }
    }

    void SGradient::SortKeys()
    {
        Algo::StableSort(Keys, [](const SGradientKey& A, const SGradientKey& B)
        {
            return A.Time < B.Time;
        });
    }

    void SGradient::GetTimeRange(float& OutMin, float& OutMax) const
    {
        if (Keys.IsEmpty())
        {
            OutMin = 0.0f;
            OutMax = 1.0f;
            return;
        }
        OutMin = Keys[0].Time;
        OutMax = Keys.Last().Time;
    }
}
