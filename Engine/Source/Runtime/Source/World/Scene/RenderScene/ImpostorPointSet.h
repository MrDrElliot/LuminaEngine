#pragma once

#include "Containers/Vector.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    // Far plants kept as bare impostor points and the per-type data they share, with what changed since the renderer last uploaded.
    class RUNTIME_API FImpostorPointSet
    {
    public:

        struct FRange
        {
            uint32 Base  = 0;
            uint32 Count = 0;
        };

        FRange AllocatePoints(uint32 Count);
        // Hides the range's points and clears the shadow pages under them.
        void   FreePoints(FRange& Range);
        FRange AllocateTypes(uint32 Count);
        void   FreeTypes(FRange& Range);

        void SetPoint(uint32 Index, const FImpostorPoint& Point);
        void SetType(uint32 Index, const FImpostorType& Type);
        // The point's type alone, for a plant hidden or shown at runtime.
        void SetPointType(uint32 Index, uint32 Type);
        const FImpostorType& GetType(uint32 Index) const { return Types[Index]; }

        // A sphere whose cached sun shadow pages must draw again.
        void Touch(const FVector4& Sphere) { Touches.Add(Sphere); }
        // One sphere around every shown point of the range, so a streamed tile clears its shadow in one go.
        void TouchRange(const FRange& Range);
        // How far the point's impostor reaches from its pivot, 0 for a point with no type.
        float GetPointRadius(const FImpostorPoint& Point) const;

        void Reset();

        // Rebuilds the bounds of every block a write reached since the last call.
        void RefreshBlockBounds();

        uint32 GetPointCount() const { return (uint32)Points.Num(); }
        uint32 GetTypeCount() const  { return (uint32)Types.Num(); }
        uint32 GetLivePointCount() const { return LivePoints; }
        const FImpostorPoint*       GetPoints() const      { return Points.GetData(); }
        const FImpostorType*        GetTypes() const       { return Types.GetData(); }
        const FInstanceBlockBounds* GetBlockBounds() const { return Blocks.GetData(); }
        uint32 GetBlockCount() const { return (uint32)Blocks.Num(); }

        // Runs of (first, count) written since the last take, in point, type and block units.
        void TakeDirty(TVector<FUIntVector2>& OutPoints, TVector<FUIntVector2>& OutTypes, TVector<FUIntVector2>& OutBlocks);
        void TakeTouches(TVector<FVector4>& OutSpheres);

    private:

        static FRange Allocate(TVector<FUIntVector2>& FreeRanges, uint32& Extent, uint32 Count);
        static void   Free(TVector<FUIntVector2>& FreeRanges, uint32& Extent, FRange Range);
        static void   AddRun(TVector<FUIntVector2>& Runs, uint32 Index);

        TVector<FImpostorPoint>       Points;
        TVector<FImpostorType>        Types;
        TVector<FInstanceBlockBounds> Blocks;

        // Sorted (base, count) holes, merged on free.
        TVector<FUIntVector2> FreePointRanges;
        TVector<FUIntVector2> FreeTypeRanges;
        uint32                PointExtent = 0;
        uint32                TypeExtent  = 0;
        uint32                LivePoints  = 0;

        TVector<FUIntVector2> DirtyPoints;
        TVector<FUIntVector2> DirtyTypes;
        TVector<uint32>       DirtyBlocks;
        TVector<FVector4>     Touches;
    };
}
