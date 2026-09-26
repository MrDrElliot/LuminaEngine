#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Math/SIMD/VFloat8.h"
#include "Platform/GenericPlatform.h"

// Uniform XZ grid over agent positions in CSR layout, rebuilt every solve and queried read-only.

namespace Lumina::Avoidance
{
    struct FAvoidanceGrid
    {
        float OriginX    = 0.0f;
        float OriginZ    = 0.0f;
        float InvCellSize = 1.0f;
        int32 DimX       = 0;
        int32 DimZ       = 0;

        TVector<int32> CellStart;
        TVector<int32> SortedIndices;

        // Positions reordered into cell order, so a cell's members load contiguously.
        TVector<float> SortedPosX;
        TVector<float> SortedPosZ;

        TVector<int32> Cursor;

        NODISCARD FORCEINLINE int32 NumCells() const { return DimX * DimZ; }

        NODISCARD FORCEINLINE int32 ClampCellX(float X) const
        {
            const int32 C = (int32)((X - OriginX) * InvCellSize);
            return C < 0 ? 0 : (C >= DimX ? DimX - 1 : C);
        }

        NODISCARD FORCEINLINE int32 ClampCellZ(float Z) const
        {
            const int32 C = (int32)((Z - OriginZ) * InvCellSize);
            return C < 0 ? 0 : (C >= DimZ ? DimZ - 1 : C);
        }

        void Build(const float* PosX, const float* PosZ, int32 Count, float CellSize)
        {
            if (Count <= 0)
            {
                DimX = DimZ = 0;
                CellStart.clear();
                SortedIndices.clear();
                SortedPosX.clear();
                SortedPosZ.clear();
                return;
            }

            const float Size = CellSize > 0.01f ? CellSize : 0.01f;
            InvCellSize = 1.0f / Size;

            float MinX = PosX[0], MaxX = PosX[0];
            float MinZ = PosZ[0], MaxZ = PosZ[0];
            for (int32 i = 1; i < Count; ++i)
            {
                MinX = Math::Min(MinX, PosX[i]);
                MaxX = Math::Max(MaxX, PosX[i]);
                MinZ = Math::Min(MinZ, PosZ[i]);
                MaxZ = Math::Max(MaxZ, PosZ[i]);
            }

            OriginX = MinX;
            OriginZ = MinZ;
            DimX = Math::Max((int32)((MaxX - MinX) * InvCellSize) + 1, 1);
            DimZ = Math::Max((int32)((MaxZ - MinZ) * InvCellSize) + 1, 1);

            const int32 Cells = DimX * DimZ;
            CellStart.assign((size_t)Cells + 1, 0);

            for (int32 i = 0; i < Count; ++i)
            {
                const int32 Cell = ClampCellZ(PosZ[i]) * DimX + ClampCellX(PosX[i]);
                ++CellStart[(size_t)Cell + 1];
            }
            for (int32 c = 1; c <= Cells; ++c)
            {
                CellStart[c] += CellStart[c - 1];
            }

            SortedIndices.resize((size_t)Count);
            SortedPosX.resize((size_t)Count);
            SortedPosZ.resize((size_t)Count);
            Cursor.assign(CellStart.begin(), CellStart.end());

            for (int32 i = 0; i < Count; ++i)
            {
                const int32 Cell = ClampCellZ(PosZ[i]) * DimX + ClampCellX(PosX[i]);
                const int32 Slot = Cursor[(size_t)Cell]++;
                SortedIndices[(size_t)Slot] = i;
                SortedPosX[(size_t)Slot]    = PosX[i];
                SortedPosZ[(size_t)Slot]    = PosZ[i];
            }
        }

        // Cells in one row are consecutive indices, so a row's span of the CSR arrays is one run.
        template <typename Fn>
        FORCEINLINE void ForEachRowSpan(float CenterX, float CenterZ, float Radius, Fn&& Func) const
        {
            if (DimX <= 0 || DimZ <= 0)
            {
                return;
            }

            const int32 MinCX = ClampCellX(CenterX - Radius);
            const int32 MaxCX = ClampCellX(CenterX + Radius);
            const int32 MinCZ = ClampCellZ(CenterZ - Radius);
            const int32 MaxCZ = ClampCellZ(CenterZ + Radius);

            for (int32 cz = MinCZ; cz <= MaxCZ; ++cz)
            {
                const int32 RowBase = cz * DimX;
                const int32 Begin   = CellStart[(size_t)(RowBase + MinCX)];
                const int32 End     = CellStart[(size_t)(RowBase + MaxCX) + 1];
                if (End > Begin)
                {
                    Func(Begin, End);
                }
            }
        }
    };

    struct FNeighborHit
    {
        float DistSq = 0.0f;
        int32 Index  = -1;
    };

    // Keeps the K nearest by insertion, and returns the radius to cull against once the list is full.
    FORCEINLINE float InsertNeighbor(FNeighborHit* Heap, int32& Count, int32 MaxCount, float DistSq, int32 Index,
                                     float CurrentRangeSq)
    {
        if (Count == MaxCount && DistSq >= Heap[MaxCount - 1].DistSq)
        {
            return CurrentRangeSq;
        }

        int32 Slot = Count < MaxCount ? Count : MaxCount - 1;
        while (Slot > 0 && Heap[Slot - 1].DistSq > DistSq)
        {
            Heap[Slot] = Heap[Slot - 1];
            --Slot;
        }
        Heap[Slot].DistSq = DistSq;
        Heap[Slot].Index  = Index;

        if (Count < MaxCount)
        {
            ++Count;
        }

        return Count == MaxCount ? Heap[MaxCount - 1].DistSq : CurrentRangeSq;
    }

    // Eight-wide distance cull over the grid's contiguous row spans, scalar only on the lanes that pass.
    inline int32 GatherNeighbors(const FAvoidanceGrid& Grid, int32 SelfIndex, float SelfX, float SelfZ,
                                 float Range, int32 MaxNeighbors, FNeighborHit* OutHeap)
    {
        int32 Count   = 0;
        float RangeSq = Range * Range;

        Grid.ForEachRowSpan(SelfX, SelfZ, Range, [&](int32 Begin, int32 End)
        {
            const float* PX = Grid.SortedPosX.data();
            const float* PZ = Grid.SortedPosZ.data();

            const SIMD::VFloat8 SX(SelfX);
            const SIMD::VFloat8 SZ(SelfZ);

            int32 i = Begin;
            for (; i + 8 <= End; i += 8)
            {
                const SIMD::VFloat8 DX = SIMD::VFloat8::Load(PX + i) - SX;
                const SIMD::VFloat8 DZ = SIMD::VFloat8::Load(PZ + i) - SZ;
                const SIMD::VFloat8 D2 = SIMD::MulAdd(DX, DX, DZ * DZ);

                int Mask = SIMD::MoveMask(SIMD::CmpLe(D2, SIMD::VFloat8(RangeSq)));
                if (Mask == 0)
                {
                    continue;
                }

                // Stored once rather than per lane, since VFloat8::operator[] spills the register each read.
                LUMINA_SIMD_ALIGN32 float Lanes[8];
                D2.StoreAligned(Lanes);

                while (Mask != 0)
                {
                    const int32 Lane = (int32)Math::CountTrailingZeros64((uint64)(uint32)Mask);
                    Mask &= Mask - 1;

                    const int32 Candidate = Grid.SortedIndices[(size_t)(i + Lane)];
                    if (Candidate != SelfIndex)
                    {
                        RangeSq = InsertNeighbor(OutHeap, Count, MaxNeighbors, Lanes[Lane], Candidate, RangeSq);
                    }
                }
            }

            for (; i < End; ++i)
            {
                const float DX = PX[i] - SelfX;
                const float DZ = PZ[i] - SelfZ;
                const float D2 = DX * DX + DZ * DZ;
                if (D2 > RangeSq)
                {
                    continue;
                }

                const int32 Candidate = Grid.SortedIndices[(size_t)i];
                if (Candidate != SelfIndex)
                {
                    RangeSq = InsertNeighbor(OutHeap, Count, MaxNeighbors, D2, Candidate, RangeSq);
                }
            }
        });

        return Count;
    }
}
