#include "RuntimePCH.h"
#include "ImpostorPointSet.h"
#include "Containers/Algorithm.h"
#include "Memory/Memcpy.h"

namespace Lumina
{
    namespace ImpostorPointSetInternal
    {
        constexpr uint32 kBlockSize = IMPOSTOR_POINT_GROUP_SIZE;
        // Reach of a block holding a type with no draw distance, far past any view.
        constexpr float  kUnlimitedReach = 1.0e30f;
    }

    FImpostorPointSet::FRange FImpostorPointSet::Allocate(TVector<FUIntVector2>& FreeRanges, uint32& Extent, uint32 Count)
    {
        FRange Out;
        Out.Count = Count;
        if (Count == 0u)
        {
            return Out;
        }

        // Best fit, since streamed tiles hold similar counts and an exact hole keeps the arrays dense.
        int32 Best = Constants::kIndexNone;
        for (int32 i = 0; i < FreeRanges.Num(); ++i)
        {
            if (FreeRanges[i].y >= Count && (Best == Constants::kIndexNone || FreeRanges[i].y < FreeRanges[Best].y))
            {
                Best = i;
            }
        }
        if (Best != Constants::kIndexNone)
        {
            Out.Base = FreeRanges[Best].x;
            FreeRanges[Best].x += Count;
            FreeRanges[Best].y -= Count;
            if (FreeRanges[Best].y == 0u)
            {
                FreeRanges.RemoveAt(Best);
            }
            return Out;
        }

        Out.Base = Extent;
        Extent  += Count;
        return Out;
    }

    void FImpostorPointSet::Free(TVector<FUIntVector2>& FreeRanges, uint32& Extent, FRange Range)
    {
        if (Range.Count == 0u)
        {
            return;
        }

        const auto Insert = Algo::LowerBound(FreeRanges, Range.Base, [](const FUIntVector2& Hole, uint32 Base) { return Hole.x < Base; });
        int32 Index = (int32)(Insert - FreeRanges.begin());
        FreeRanges.Insert(FUIntVector2(Range.Base, Range.Count), Index);

        if (Index + 1 < FreeRanges.Num() && FreeRanges[Index].x + FreeRanges[Index].y == FreeRanges[Index + 1].x)
        {
            FreeRanges[Index].y += FreeRanges[Index + 1].y;
            FreeRanges.RemoveAt(Index + 1);
        }
        if (Index > 0 && FreeRanges[Index - 1].x + FreeRanges[Index - 1].y == FreeRanges[Index].x)
        {
            FreeRanges[Index - 1].y += FreeRanges[Index].y;
            FreeRanges.RemoveAt(Index);
            --Index;
        }
        // A hole at the end gives its room back, so a forest that streams out shrinks the arrays.
        if (FreeRanges[Index].x + FreeRanges[Index].y == Extent)
        {
            Extent = FreeRanges[Index].x;
            FreeRanges.RemoveAt(Index);
        }
    }

    void FImpostorPointSet::AddRun(TVector<FUIntVector2>& Runs, uint32 Index)
    {
        if (!Runs.IsEmpty() && Runs.Last().x + Runs.Last().y == Index)
        {
            ++Runs.Last().y;
            return;
        }
        Runs.Emplace(Index, 1u);
    }

    FImpostorPointSet::FRange FImpostorPointSet::AllocatePoints(uint32 Count)
    {
        const FRange Range = Allocate(FreePointRanges, PointExtent, Count);
        if (Points.Num() < (int32)PointExtent)
        {
            Points.SetNum(PointExtent);
        }
        LivePoints += Count;
        return Range;
    }

    void FImpostorPointSet::FreePoints(FRange& Range)
    {
        if (Range.Count == 0u)
        {
            return;
        }
        TouchRange(Range);
        FImpostorPoint Hidden = {};
        Hidden.Type = IMPOSTOR_TYPE_NONE;
        for (uint32 i = Range.Base; i < Range.Base + Range.Count; ++i)
        {
            if (Points[i].Type != IMPOSTOR_TYPE_NONE)
            {
                SetPoint(i, Hidden);
            }
        }
        LivePoints -= Range.Count;
        Free(FreePointRanges, PointExtent, Range);
        // The tail past the extent is dead, and the renderer uploads only up to it.
        Points.SetNum(PointExtent);
        Range = {};
    }

    FImpostorPointSet::FRange FImpostorPointSet::AllocateTypes(uint32 Count)
    {
        const FRange Range = Allocate(FreeTypeRanges, TypeExtent, Count);
        if (Types.Num() < (int32)TypeExtent)
        {
            Types.SetNum(TypeExtent);
        }
        return Range;
    }

    void FImpostorPointSet::FreeTypes(FRange& Range)
    {
        for (uint32 i = Range.Base; i < Range.Base + Range.Count; ++i)
        {
            SetType(i, FImpostorType{});
        }
        Free(FreeTypeRanges, TypeExtent, Range);
        Types.SetNum(TypeExtent);
        Range = {};
    }

    float FImpostorPointSet::GetPointRadius(const FImpostorPoint& Point) const
    {
        if (Point.Type >= (uint32)Types.Num())
        {
            return 0.0f;
        }
        const FImpostorType& Type = Types[Point.Type];
        return (Math::Length(Type.Proxy.LocalCenter) + Math::Abs(Type.Proxy.SplatHalfSize)) * Point.Scale;
    }

    void FImpostorPointSet::TouchRange(const FRange& Range)
    {
        FVector3 Min(FLT_MAX);
        FVector3 Max(-FLT_MAX);
        for (uint32 i = Range.Base; i < Range.Base + Range.Count; ++i)
        {
            const FImpostorPoint& Point = Points[i];
            if (Point.Type != IMPOSTOR_TYPE_NONE)
            {
                const float Radius = GetPointRadius(Point);
                Min = Math::Min(Min, Point.Position - FVector3(Radius));
                Max = Math::Max(Max, Point.Position + FVector3(Radius));
            }
        }
        if (Max.x >= Min.x)
        {
            const FVector3 Center = (Min + Max) * 0.5f;
            Touch(FVector4(Center, Math::Length(Max - Center)));
        }
    }

    void FImpostorPointSet::SetPoint(uint32 Index, const FImpostorPoint& Point)
    {
        Points[Index] = Point;
        AddRun(DirtyPoints, Index);
        const uint32 Block = Index / ImpostorPointSetInternal::kBlockSize;
        if (DirtyBlocks.IsEmpty() || DirtyBlocks.Last() != Block)
        {
            DirtyBlocks.Add(Block);
        }
    }

    void FImpostorPointSet::SetPointType(uint32 Index, uint32 Type)
    {
        if (Index >= (uint32)Points.Num() || Points[Index].Type == Type)
        {
            return;
        }
        FImpostorPoint Point = Points[Index];
        const uint32 Shown = Type != IMPOSTOR_TYPE_NONE ? Type : Point.Type;
        Point.Type = Shown;
        Touch(FVector4(Point.Position, GetPointRadius(Point)));
        Point.Type = Type;
        SetPoint(Index, Point);
    }

    void FImpostorPointSet::SetType(uint32 Index, const FImpostorType& Type)
    {
        if (Memory::Memcmp(&Types[Index], &Type, sizeof(FImpostorType)) == 0)
        {
            return;
        }
        Types[Index] = Type;
        AddRun(DirtyTypes, Index);
    }

    void FImpostorPointSet::RefreshBlockBounds()
    {
        using namespace ImpostorPointSetInternal;

        const uint32 BlockCount = RenderUtils::GetGroupCount(PointExtent, kBlockSize);
        Blocks.SetNum(BlockCount);
        Algo::Sort(DirtyBlocks);
        DirtyBlocks.SetNum((int32)(Algo::Unique(DirtyBlocks) - DirtyBlocks.begin()));
        DirtyBlocks.RemoveAll([BlockCount](uint32 Block) { return Block >= BlockCount; });

        for (uint32 Block : DirtyBlocks)
        {
            FInstanceBlockBounds Bounds = {};
            Bounds.Min   = FVector3(FLT_MAX);
            Bounds.Max   = FVector3(-FLT_MAX);
            Bounds.Reach = -1.0f;
            const uint32 End = Math::Min((Block + 1u) * kBlockSize, PointExtent);
            for (uint32 i = Block * kBlockSize; i < End; ++i)
            {
                const FImpostorPoint& Point = Points[i];
                if (Point.Type >= (uint32)Types.Num())
                {
                    continue;
                }
                const FImpostorType& Type = Types[Point.Type];
                const float Radius = (Math::Length(Type.Proxy.LocalCenter) + Math::Abs(Type.Proxy.SplatHalfSize)) * Point.Scale;
                Bounds.Min   = Math::Min(Bounds.Min, Point.Position - FVector3(Radius));
                Bounds.Max   = Math::Max(Bounds.Max, Point.Position + FVector3(Radius));
                Bounds.Reach = Math::Max(Bounds.Reach, Type.MaxDrawDistance > 0.0f ? Type.MaxDrawDistance + Radius : kUnlimitedReach);
            }
            if (Bounds.Reach < 0.0f)
            {
                Bounds.Min = Bounds.Max = FVector3(0.0f);
            }
            Blocks[Block] = Bounds;
        }
    }

    void FImpostorPointSet::TakeDirty(TVector<FUIntVector2>& OutPoints, TVector<FUIntVector2>& OutTypes, TVector<FUIntVector2>& OutBlocks)
    {
        OutPoints = Move(DirtyPoints);
        OutTypes  = Move(DirtyTypes);
        OutBlocks.Reset();
        for (uint32 Block : DirtyBlocks)
        {
            if (Block < (uint32)Blocks.Num())
            {
                AddRun(OutBlocks, Block);
            }
        }
        DirtyPoints.Reset();
        DirtyTypes.Reset();
        DirtyBlocks.Reset();

        // Runs past the live arrays were freed after being written, and the renderer has nothing there to send.
        const auto Clip = [](TVector<FUIntVector2>& Runs, uint32 Limit)
        {
            for (FUIntVector2& Run : Runs)
            {
                Run.y = Run.x >= Limit ? 0u : Math::Min(Run.y, Limit - Run.x);
            }
            Runs.RemoveAll([](const FUIntVector2& Run) { return Run.y == 0u; });
        };
        Clip(OutPoints, (uint32)Points.Num());
        Clip(OutTypes, (uint32)Types.Num());
    }

    void FImpostorPointSet::TakeTouches(TVector<FVector4>& OutSpheres)
    {
        OutSpheres.Append(Touches);
        Touches.Reset();
    }

    void FImpostorPointSet::Reset()
    {
        Points.Reset();
        Types.Reset();
        Blocks.Reset();
        FreePointRanges.Reset();
        FreeTypeRanges.Reset();
        PointExtent = 0;
        TypeExtent  = 0;
        LivePoints  = 0;
        DirtyPoints.Reset();
        DirtyTypes.Reset();
        DirtyBlocks.Reset();
        Touches.Reset();
    }
}
