#pragma once

#include "SceneRenderTypes.h"
#include "Containers/Span.h"

#include <limits>

namespace Lumina::SunShadow
{
    // Unreal's split, where each cascade spans Exponent times the one before. Slots past the near cascades hold the far cascade or the far end.
    inline void ComputeCascadeSplits(float Near, float Far, int32 NearCount, float Exponent, bool bFarCascade,
                                     float FarCascadeDistance, float (&OutSplits)[NumCascades])
    {
        NearCount = Math::Clamp(NearCount, 1, NumCascades);

        float TotalScale = 0.0f;
        for (int32 i = 0; i < NearCount; ++i)
        {
            TotalScale += Math::Pow(Exponent, (float)i);
        }

        float AccumulatedScale = 0.0f;
        for (int32 i = 0; i < NumCascades; ++i)
        {
            if (i < NearCount)
            {
                AccumulatedScale += Math::Pow(Exponent, (float)i);
            }
            OutSplits[i] = (bFarCascade && i >= NearCount)
                ? FarCascadeDistance
                : Near + (Far - Near) * (AccumulatedScale / TotalScale);
        }
    }

    // A square grid in the plane facing the sun. A ray toward the sun keeps its position in that plane, so one cell holds every caster it can meet.
    struct FSunGrid
    {
        FVector3 Right    = FVector3(1.0f, 0.0f, 0.0f);
        FVector3 Up       = FVector3(0.0f, 0.0f, 1.0f);
        FVector2 Origin   = FVector2(0.0f);
        float    CellSize = 1.0f;

        FVector2 Project(const FVector3& P) const
        {
            return FVector2(Math::Dot(P, Right), Math::Dot(P, Up));
        }

        FIntVector2 CellOf(const FVector3& P) const
        {
            const FVector2 Cell = (Project(P) - Origin) / CellSize;
            return FIntVector2((int32)Math::Floor(Cell.x), (int32)Math::Floor(Cell.y));
        }
    };

    // Sized so every point lands inside a Dimension by Dimension grid.
    inline FSunGrid FitSunGrid(const FVector3& ToSun, TSpan<const FVector3> Points, uint32 Dimension)
    {
        FSunGrid Grid;
        const FVector3 Reference = Math::Abs(ToSun.y) > 0.99f ? FVector3(1.0f, 0.0f, 0.0f) : FVector3(0.0f, 1.0f, 0.0f);
        Grid.Right = Math::Normalize(Math::Cross(Reference, ToSun));
        Grid.Up    = Math::Cross(ToSun, Grid.Right);

        FVector2 Min(std::numeric_limits<float>::max());
        FVector2 Max(-std::numeric_limits<float>::max());
        for (const FVector3& Point : Points)
        {
            const FVector2 XY = Grid.Project(Point);
            Min = Math::Min(Min, XY);
            Max = Math::Max(Max, XY);
        }

        // A sliver of slack keeps the far edge inside the last cell rather than on the boundary past it.
        const float Extent = Math::Max(Max.x - Min.x, Max.y - Min.y);
        Grid.CellSize = Math::Max(Extent * 1.001f / (float)Math::Max(Dimension, 1u), 0.01f);
        Grid.Origin   = Min;
        return Grid;
    }
}
