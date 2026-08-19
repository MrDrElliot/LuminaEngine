#include "RuntimePCH.h"
#include "TerrainSculptSystem.h"
#include "Memory/Allocators/Allocator.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/TerrainComponent.h"

namespace Lumina
{
    namespace
    {
        // Matches DefaultSceneRenderer OriginXZ so brush hits the same samples the renderer sees.
        FVector2 TerrainOriginXZ(const STerrainComponent& Terrain, const FVector3& TerrainOrigin)
        {
            const float HalfSize = Terrain.TileWorldSize * 0.5f;
            return FVector2(TerrainOrigin.x - HalfSize, TerrainOrigin.z - HalfSize);
        }

        FIntVector4 ComputeSampleRect(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, const FVector3& WorldPos, float Radius)
        {
            const float Stride = Terrain.TileWorldSize / float(Terrain.Resolution - 1);
            const FVector2 OriginXZ = TerrainOriginXZ(Terrain, TerrainOrigin);
            const float LocalX = WorldPos.x - OriginXZ.x;
            const float LocalZ = WorldPos.z - OriginXZ.y;

            int MinX = int(std::floor((LocalX - Radius) / Stride));
            int MaxX = int(std::ceil ((LocalX + Radius) / Stride));
            int MinY = int(std::floor((LocalZ - Radius) / Stride));
            int MaxY = int(std::ceil ((LocalZ + Radius) / Stride));

            MinX = Math::Max(MinX, 0);
            MinY = Math::Max(MinY, 0);
            MaxX = Math::Min(MaxX, Terrain.Resolution - 1);
            MaxY = Math::Min(MaxY, Terrain.Resolution - 1);

            if (MaxX < MinX || MaxY < MinY)
            {
                return FIntVector4(0, 0, -1, -1);
            }

            return FIntVector4(MinX, MinY, MaxX, MaxY);
        }

        // Smoothstep-shaped disk falloff. Returns 0 outside the radius.
        float BrushWeight(float DistSq, float Radius, float Falloff)
        {
            const float R  = Radius;
            const float R2 = R * R;
            if (DistSq >= R2)
            {
                return 0.0f;
            }

            float T = 1.0f - std::sqrt(DistSq) / R;           // 1 at center, 0 at edge
            float H = Math::Mix(1.0f, T * T * (3.0f - 2.0f * T), Falloff);
            return H;
        }
    }

    FIntVector2 FTerrainSculptSystem::WorldToSample(const STerrainComponent& Terrain, const FVector3& WorldPos)
    {
        const float Stride = Terrain.TileWorldSize / float(Terrain.Resolution - 1);
        return FIntVector2(int(WorldPos.x / Stride), int(WorldPos.z / Stride));
    }

    bool FTerrainSculptSystem::SampleHeight(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ, float& OutHeight)
    {
        const int32 Res = Terrain.Resolution;
        if (Res < 2 || Terrain.Heightmap.size() < size_t(Res) * size_t(Res))
        {
            return false;
        }

        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, TerrainOrigin);
        const float Stride = Terrain.TileWorldSize / float(Res - 1);

        const float Lx = (WorldX - OriginXZ.x) / Stride;
        const float Lz = (WorldZ - OriginXZ.y) / Stride;
        if (Lx < 0.0f || Lz < 0.0f || Lx > float(Res - 1) || Lz > float(Res - 1))
        {
            return false;
        }

        const int X0 = int(Lx), Z0 = int(Lz);
        const int X1 = Math::Min(X0 + 1, Res - 1), Z1 = Math::Min(Z0 + 1, Res - 1);
        const float Tx = Lx - float(X0), Tz = Lz - float(Z0);
        const float H00 = Terrain.Heightmap[size_t(Z0) * Res + X0];
        const float H10 = Terrain.Heightmap[size_t(Z0) * Res + X1];
        const float H01 = Terrain.Heightmap[size_t(Z1) * Res + X0];
        const float H11 = Terrain.Heightmap[size_t(Z1) * Res + X1];
        const float H   = Math::Mix(Math::Mix(H00, H10, Tx), Math::Mix(H01, H11, Tx), Tz);

        OutHeight = TerrainOrigin.y + H * Terrain.MaxHeight;
        return true;
    }

    FVector3 FTerrainSculptSystem::SampleNormal(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ)
    {
        const float Stride = Terrain.TileWorldSize / float(Math::Max(Terrain.Resolution - 1, 1));
        const float Eps    = Math::Max(Stride, 0.5f);

        float HxL, HxR, HzL, HzR;
        if (!SampleHeight(Terrain, TerrainOrigin, WorldX - Eps, WorldZ, HxL)) { return FVector3(0.0f, 1.0f, 0.0f); }
        if (!SampleHeight(Terrain, TerrainOrigin, WorldX + Eps, WorldZ, HxR)) { return FVector3(0.0f, 1.0f, 0.0f); }
        if (!SampleHeight(Terrain, TerrainOrigin, WorldX, WorldZ - Eps, HzL)) { return FVector3(0.0f, 1.0f, 0.0f); }
        if (!SampleHeight(Terrain, TerrainOrigin, WorldX, WorldZ + Eps, HzR)) { return FVector3(0.0f, 1.0f, 0.0f); }

        const float dHdx = (HxR - HxL) / (2.0f * Eps);
        const float dHdz = (HzR - HzL) / (2.0f * Eps);
        return Math::Normalize(FVector3(-dHdx, 1.0f, -dHdz));
    }

    bool FTerrainSculptSystem::Raycast(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, const FVector3& RayOrigin, const FVector3& RayDir, FVector3& OutHit)
    {
        const int32 Res     = Terrain.Resolution;
        const float MaxDist = Terrain.TileWorldSize * 4.0f;
        const float Step    = Math::Max(0.5f, Terrain.TileWorldSize / float(Res) * 0.5f);

        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, TerrainOrigin);
        const float BaseY  = TerrainOrigin.y;
        const float Stride = Terrain.TileWorldSize / float(Res - 1);

        auto InBounds = [&](const FVector3& P)
        {
            const float Lx = (P.x - OriginXZ.x) / Stride;
            const float Lz = (P.z - OriginXZ.y) / Stride;
            return Lx >= 0.0f && Lz >= 0.0f && Lx <= float(Res - 1) && Lz <= float(Res - 1);
        };

        // Bilinear surface height at a world XZ (clamped to the tile), so the hit
        // tracks the rendered surface smoothly instead of snapping to texels.
        auto SurfaceY = [&](float Wx, float Wz)
        {
            const float Fx = Math::Clamp((Wx - OriginXZ.x) / Stride, 0.0f, float(Res - 1));
            const float Fz = Math::Clamp((Wz - OriginXZ.y) / Stride, 0.0f, float(Res - 1));
            const int X0 = int(Fx), Z0 = int(Fz);
            const int X1 = Math::Min(X0 + 1, Res - 1), Z1 = Math::Min(Z0 + 1, Res - 1);
            const float Tx = Fx - float(X0), Tz = Fz - float(Z0);
            const float H00 = Terrain.Heightmap[size_t(Z0) * Res + X0];
            const float H10 = Terrain.Heightmap[size_t(Z0) * Res + X1];
            const float H01 = Terrain.Heightmap[size_t(Z1) * Res + X0];
            const float H11 = Terrain.Heightmap[size_t(Z1) * Res + X1];
            const float H   = Math::Mix(Math::Mix(H00, H10, Tx), Math::Mix(H01, H11, Tx), Tz);
            return BaseY + H * Terrain.MaxHeight;
        };

        FVector3 Prev      = RayOrigin;
        bool      bPrevIn   = InBounds(Prev);
        float     PrevDiff  = bPrevIn ? (Prev.y - SurfaceY(Prev.x, Prev.z)) : 0.0f;

        for (float T = Step; T < MaxDist; T += Step)
        {
            const FVector3 Pos = RayOrigin + RayDir * T;
            const bool bIn = InBounds(Pos);
            if (bIn)
            {
                const float Diff = Pos.y - SurfaceY(Pos.x, Pos.z);
                // Downward crossing (above -> on/below) between Prev and Pos: bisect for a tight hit.
                if (bPrevIn && PrevDiff > 0.0f && Diff <= 0.0f)
                {
                    FVector3 A = Prev, B = Pos;
                    for (int i = 0; i < 8; ++i)
                    {
                        const FVector3 M = (A + B) * 0.5f;
                        if (M.y - SurfaceY(M.x, M.z) > 0.0f) A = M; else B = M;
                    }
                    const FVector3 H = (A + B) * 0.5f;
                    OutHit = FVector3(H.x, SurfaceY(H.x, H.z), H.z);
                    return true;
                }
                PrevDiff = Diff;
            }
            Prev    = Pos;
            bPrevIn = bIn;
        }
        return false;
    }

    void FTerrainSculptSystem::ApplyDab(STerrainComponent& Terrain, const FTerrainSculptDab& Dab)
    {
        if (Terrain.Resolution <= 0 || Terrain.Heightmap.empty())
        {
            return;
        }

        FIntVector4 Rect;
        if (Dab.Mode == ETerrainBrushMode::Ramp)
        {
                const FIntVector4 R1 = ComputeSampleRect(Terrain, Dab.TerrainOrigin, Dab.RampStart, Dab.RampHalfWidth);
            const FIntVector4 R2 = ComputeSampleRect(Terrain, Dab.TerrainOrigin, Dab.RampEnd,   Dab.RampHalfWidth);
            const bool V1 = !(R1.z < R1.x || R1.w < R1.y);
            const bool V2 = !(R2.z < R2.x || R2.w < R2.y);
            if (!V1 && !V2)
            {
                return;
            }
            if (V1 && V2)
            {
                Rect = FIntVector4(Math::Min(R1.x, R2.x), Math::Min(R1.y, R2.y), Math::Max(R1.z, R2.z), Math::Max(R1.w, R2.w));
            }
            else
            {
                Rect = V1 ? R1 : R2;
            }
        }
        else
        {
            Rect = ComputeSampleRect(Terrain, Dab.TerrainOrigin, Dab.WorldPosition, Dab.Radius);
        }
        if (Rect.z < Rect.x || Rect.w < Rect.y)
        {
            return;
        }

        switch (Dab.Mode)
        {
        case ETerrainBrushMode::Sculpt:  ApplySculpt (Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Flatten: ApplyFlatten(Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Smooth:  ApplySmooth (Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Noise:   ApplyNoise  (Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Ramp:    ApplyRamp   (Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Paint:   ApplyPaint  (Terrain, Dab, Rect); break;
        }

        if (Dab.Mode == ETerrainBrushMode::Paint)
        {
            const int32 Layer = Dab.ActiveLayer;
            if (Layer >= 0 && Layer < (int32)Terrain.Layers.size())
            {
                Terrain.MarkWeightsDirty((uint32)Layer,
                                         FIntVector2(Rect.x, Rect.y),
                                         FIntVector2(Rect.z, Rect.w));
            }
        }
        else
        {
            Terrain.MarkHeightDirty(FIntVector2(Rect.x, Rect.y), FIntVector2(Rect.z, Rect.w));
        }
    }
    
    void FTerrainSculptSystem::ApplySculpt(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const float Stride    = Terrain.TileWorldSize / float(Terrain.Resolution - 1);
        const float Radius    = Dab.Radius;
        
        // Strength is expressed in normalized height units per second (i.e. 1.0 == full
        const float DeltaVal = (Dab.Strength * Dab.DeltaSeconds * static_cast<float>(Dab.SculptSign)) / Terrain.MaxHeight;
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);

        const int32 RowCount = Rect.w - Rect.y + 1;
        Task::ParallelFor((uint32)RowCount, [&, Rect, Radius, DeltaVal, Stride, OriginXZ](const Task::FParallelRange& Range)
        {
            for (uint32 RowIdx = Range.Start; RowIdx < Range.End; ++RowIdx)
            {
                int Y = Rect.y + static_cast<int>(RowIdx);
                float WorldZ = OriginXZ.y + static_cast<float>(Y) * Stride;
                float DZ = WorldZ - Dab.WorldPosition.z;
                for (int X = Rect.x; X <= Rect.z; ++X)
                {
                    float WorldX = OriginXZ.x + static_cast<float>(X) * Stride;
                    float DX = WorldX - Dab.WorldPosition.x;
                    float DistSq = DX * DX + DZ * DZ;
                    float W = BrushWeight(DistSq, Radius, Dab.Falloff);
                    if (W <= 0.0f)
                    {
                        continue;
                    }

                    float& H = Terrain.Heightmap[Y * Terrain.Resolution + X];
                    H = Math::Clamp(H + DeltaVal * W, 0.0f, 1.0f);
                }
            }
        }, 64u);
    }

    void FTerrainSculptSystem::ApplyFlatten(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const float Stride = Terrain.TileWorldSize / float(Terrain.Resolution - 1);
        const float Target = Terrain.MaxHeight > 0.0f ? Math::Clamp(Dab.FlattenHeight / Terrain.MaxHeight, 0.0f, 1.0f) : 0.0f;
        const float Speed  = Math::Clamp(Dab.Strength * Dab.DeltaSeconds, 0.0f, 1.0f);
        const float Radius = Dab.Radius;
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);

        const int32 RowCount = Rect.w - Rect.y + 1;
        Task::ParallelFor((uint32)RowCount, [&, Rect, Radius, Target, Speed, Stride, OriginXZ](const Task::FParallelRange& Range)
        {
            for (uint32 RowIdx = Range.Start; RowIdx < Range.End; ++RowIdx)
            {
                int Y = Rect.y + int(RowIdx);
                float WorldZ = OriginXZ.y + float(Y) * Stride;
                float DZ = WorldZ - Dab.WorldPosition.z;
                for (int X = Rect.x; X <= Rect.z; ++X)
                {
                    float WorldX = OriginXZ.x + float(X) * Stride;
                    float DX = WorldX - Dab.WorldPosition.x;
                    float W = BrushWeight(DX * DX + DZ * DZ, Radius, Dab.Falloff);
                    if (W <= 0.0f)
                    {
                        continue;
                    }

                    float& H = Terrain.Heightmap[Y * Terrain.Resolution + X];
                    H = Math::Mix(H, Target, Speed * W);
                }
            }
        }, 64u);
    }

    void FTerrainSculptSystem::ApplySmooth(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        // Snapshot only the affected rect+1px apron; copying the full map at 1025^2 dominated per-dab cost.
        const int32 Res    = Terrain.Resolution;
        const float Stride = Terrain.TileWorldSize / float(Res - 1);
        const float Speed  = Math::Clamp(Dab.Strength * Dab.DeltaSeconds, 0.0f, 1.0f);
        const float Radius = Dab.Radius;
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);

        const int32 SnapMinX = Math::Max(Rect.x - 1, 0);
        const int32 SnapMinY = Math::Max(Rect.y - 1, 0);
        const int32 SnapMaxX = Math::Min(Rect.z + 1, Res - 1);
        const int32 SnapMaxY = Math::Min(Rect.w + 1, Res - 1);
        const int32 SnapW    = SnapMaxX - SnapMinX + 1;
        const int32 SnapH    = SnapMaxY - SnapMinY + 1;

        // Scratch-backed so a held smooth stroke doesn't churn the heap every frame.
        FMemMark Mark;
        TScratchVector<float> Snapshot;
        Snapshot.resize(size_t(SnapW) * size_t(SnapH));
        for (int32 Y = 0; Y < SnapH; ++Y)
        {
            const float* SrcRow = &Terrain.Heightmap[size_t(SnapMinY + Y) * size_t(Res) + size_t(SnapMinX)];
            float*       DstRow = &Snapshot[size_t(Y) * size_t(SnapW)];
            std::memcpy(DstRow, SrcRow, size_t(SnapW) * sizeof(float));
        }

        const int32 RowCount = Rect.w - Rect.y + 1;
        Task::ParallelFor((uint32)RowCount, [&, Rect, Radius, Speed, Stride, OriginXZ, SnapMinX, SnapMinY, SnapW](const Task::FParallelRange& Range)
        {
            for (uint32 RowIdx = Range.Start; RowIdx < Range.End; ++RowIdx)
            {
                int Y = Rect.y + int(RowIdx);
                float WorldZ = OriginXZ.y + float(Y) * Stride;
                float DZ = WorldZ - Dab.WorldPosition.z;
                for (int X = Rect.x; X <= Rect.z; ++X)
                {
                    float WorldX = OriginXZ.x + float(X) * Stride;
                    float DX = WorldX - Dab.WorldPosition.x;
                    float W = BrushWeight(DX * DX + DZ * DZ, Radius, Dab.Falloff);
                    if (W <= 0.0f)
                    {
                        continue;
                    }

                    float Sum = 0.0f;
                    int Count = 0;
                    for (int YY = Math::Max(Y - 1, 0); YY <= Math::Min(Y + 1, Res - 1); ++YY)
                    {
                        for (int XX = Math::Max(X - 1, 0); XX <= Math::Min(X + 1, Res - 1); ++XX)
                        {
                            Sum += Snapshot[size_t(YY - SnapMinY) * size_t(SnapW) + size_t(XX - SnapMinX)];
                            ++Count;
                        }
                    }
                    float Avg = Sum / float(Count);

                    float& H = Terrain.Heightmap[Y * Res + X];
                    H = Math::Mix(H, Avg, Speed * W);
                }
            }
        }, 64u);
    }

    namespace
    {
        inline float Hash2D(int X, int Y)
        {
            uint32 N = uint32(X) * 374761393u + uint32(Y) * 668265263u;
            N = (N ^ (N >> 13u)) * 1274126177u;
            N = N ^ (N >> 16u);
            return float(N & 0x00FFFFFFu) / float(0x00FFFFFF);
        }

        inline float ValueNoise2D(float X, float Y)
        {
            const int Xi = int(std::floor(X));
            const int Yi = int(std::floor(Y));
            const float Fx = X - float(Xi);
            const float Fy = Y - float(Yi);
            const float Ux = Fx * Fx * (3.0f - 2.0f * Fx);
            const float Uy = Fy * Fy * (3.0f - 2.0f * Fy);
            const float V00 = Hash2D(Xi,     Yi);
            const float V10 = Hash2D(Xi + 1, Yi);
            const float V01 = Hash2D(Xi,     Yi + 1);
            const float V11 = Hash2D(Xi + 1, Yi + 1);
            const float A = Math::Mix(V00, V10, Ux);
            const float B = Math::Mix(V01, V11, Ux);
            return Math::Mix(A, B, Uy) * 2.0f - 1.0f;   // [-1, 1]
        }

        inline float Fbm2D(float X, float Y, int Octaves)
        {
            float Sum = 0.0f;
            float Amp = 1.0f;
            float Freq = 1.0f;
            float Norm = 0.0f;
            for (int i = 0; i < Octaves; ++i)
            {
                Sum  += ValueNoise2D(X * Freq, Y * Freq) * Amp;
                Norm += Amp;
                Amp  *= 0.5f;
                Freq *= 2.0f;
            }
            return (Norm > 0.0f) ? (Sum / Norm) : 0.0f;
        }
    }

    void FTerrainSculptSystem::ApplyNoise(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const int32 Res    = Terrain.Resolution;
        const float Stride = Terrain.TileWorldSize / float(Res - 1);
        const float Radius = Dab.Radius;
        const float DeltaPerSecond = (Dab.Strength * Dab.DeltaSeconds * float(Dab.SculptSign)) / Terrain.MaxHeight;
        const float Freq = Math::Max(Dab.NoiseFrequency, 1e-6f);
        const int   Octaves = Math::Clamp(Dab.NoiseOctaves, 1, 8);
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);

        const int32 RowCount = Rect.w - Rect.y + 1;
        Task::ParallelFor((uint32)RowCount, [&, Rect, Radius, DeltaPerSecond, Stride, OriginXZ, Freq, Octaves](const Task::FParallelRange& Range)
        {
            for (uint32 RowIdx = Range.Start; RowIdx < Range.End; ++RowIdx)
            {
                int Y = Rect.y + int(RowIdx);
                float WorldZ = OriginXZ.y + float(Y) * Stride;
                float DZ = WorldZ - Dab.WorldPosition.z;
                for (int X = Rect.x; X <= Rect.z; ++X)
                {
                    float WorldX = OriginXZ.x + float(X) * Stride;
                    float DX = WorldX - Dab.WorldPosition.x;
                    float W = BrushWeight(DX * DX + DZ * DZ, Radius, Dab.Falloff);
                    if (W <= 0.0f)
                    {
                        continue;
                    }

                    const float N = Fbm2D(WorldX * Freq, WorldZ * Freq, Octaves);
                    float& H = Terrain.Heightmap[Y * Res + X];
                    H = Math::Clamp(H + DeltaPerSecond * W * N, 0.0f, 1.0f);
                }
            }
        }, 64u);
    }

    void FTerrainSculptSystem::ApplyRamp(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const int32 Res    = Terrain.Resolution;
        const float Stride = Terrain.TileWorldSize / float(Res - 1);
        const float Speed  = Math::Clamp(Dab.Strength * Dab.DeltaSeconds, 0.0f, 1.0f);
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);

        const FVector2 A   = FVector2(Dab.RampStart.x, Dab.RampStart.z);
        const FVector2 B   = FVector2(Dab.RampEnd.x,   Dab.RampEnd.z);
        const FVector2 AB  = B - A;
        const float ABLen2  = Math::Dot(AB, AB);
        if (ABLen2 < 1e-3f)
        {
            return;
        }

        // Endpoint heights are relative to the terrain origin (as sampled everywhere else); absolute
        // world Y would break ramps once the terrain entity moves off Y=0.
        const float MaxH = Math::Max(Terrain.MaxHeight, 1e-3f);
        const float StartN = Dab.RampUseExplicitHeights
            ? Math::Clamp(Dab.RampStartHeight / MaxH, 0.0f, 1.0f)
            : Math::Clamp((Dab.RampStart.y - Dab.TerrainOrigin.y) / MaxH, 0.0f, 1.0f);
        const float EndN = Dab.RampUseExplicitHeights
            ? Math::Clamp(Dab.RampEndHeight / MaxH, 0.0f, 1.0f)
            : Math::Clamp((Dab.RampEnd.y - Dab.TerrainOrigin.y) / MaxH, 0.0f, 1.0f);

        const float HalfWidth = Math::Max(Dab.RampHalfWidth, Stride);
        const float InvLen    = 1.0f / std::sqrt(ABLen2);
        const float Feather   = Math::Clamp(Dab.Falloff, 0.0f, 1.0f);

        const int32 RowCount = Rect.w - Rect.y + 1;
        Task::ParallelFor((uint32)RowCount, [&, Rect, Speed, Stride, OriginXZ, A, AB, ABLen2, StartN, EndN, HalfWidth, InvLen, Feather](const Task::FParallelRange& Range)
        {
            for (uint32 RowIdx = Range.Start; RowIdx < Range.End; ++RowIdx)
            {
                int Y = Rect.y + int(RowIdx);
                float WorldZ = OriginXZ.y + float(Y) * Stride;
                for (int X = Rect.x; X <= Rect.z; ++X)
                {
                    float WorldX = OriginXZ.x + float(X) * Stride;

                    const FVector2 P     = FVector2(WorldX, WorldZ) - A;
                    const float Along     = Math::Dot(P, AB) / ABLen2;   // unclamped position along the ramp
                    const float T         = Math::Clamp(Along, 0.0f, 1.0f);
                    const FVector2 C     = AB * T;
                    const float Perp      = std::sqrt(Math::Dot(P - C, P - C));
                    if (Perp > HalfWidth)
                    {
                        continue;
                    }

                    // Lateral feather: full strength in an inner core, smoothstepped to 0 at the edge so
                    // the ramp blends in instead of a vertical wall. Falloff sets the feathered band width.
                    const float Edge = Perp / HalfWidth;       // 0 center, 1 edge
                    float Lat = 1.0f;
                    if (Feather > 1e-4f)
                    {
                        const float U = Math::Clamp((Edge - (1.0f - Feather)) / Feather, 0.0f, 1.0f);
                        Lat = 1.0f - U * U * (3.0f - 2.0f * U);
                    }

                    // Longitudinal feather: fade the rounded end-caps over one half-width so the
                    // ramp tapers into the terrain at each end rather than stamping a flat disc.
                    float Long = 1.0f;
                    const float CapU = HalfWidth * InvLen;
                    if (Along < 0.0f)
                    {
                        Long = Math::Clamp(1.0f + Along / CapU, 0.0f, 1.0f);
                    }
                    else if (Along > 1.0f)
                    {
                        Long = Math::Clamp(1.0f - (Along - 1.0f) / CapU, 0.0f, 1.0f);
                    }
                    Long = Long * Long * (3.0f - 2.0f * Long);

                    const float Target = Math::Mix(StartN, EndN, T);
                    float& H = Terrain.Heightmap[Y * Res + X];
                    H = Math::Mix(H, Target, Speed * Lat * Long);
                }
            }
        }, 64u);
    }

    void FTerrainSculptSystem::ApplyPaint(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const int32 Layer = Dab.ActiveLayer;
        if (Layer < 0 || Layer >= (int32)Terrain.Layers.size())
            return;

        const int32 Res         = Terrain.Resolution;
        const int32 LayerCount  = (int32)Terrain.Layers.size();
        const int32 LayerStride = Res * Res;
        const size_t Needed = size_t(LayerCount) * size_t(LayerStride);
        if (Terrain.LayerWeights.size() < Needed)
        {
            Terrain.LayerWeights.resize(Needed, 0u);
        }

        const float Stride = Terrain.TileWorldSize / float(Terrain.Resolution - 1);
        const float Speed  = Math::Clamp(Dab.Strength * Dab.DeltaSeconds, 0.0f, 1.0f);
        const float Radius = Dab.Radius;
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);

        const int32 RowCount = Rect.w - Rect.y + 1;
        Task::ParallelFor((uint32)RowCount, [&, Rect, Radius, Speed, Stride, OriginXZ, Layer, LayerStride, Res](const Task::FParallelRange& Range)
        {
            for (uint32 RowIdx = Range.Start; RowIdx < Range.End; ++RowIdx)
            {
                int Y = Rect.y + int(RowIdx);
                float WorldZ = OriginXZ.y + float(Y) * Stride;
                float DZ = WorldZ - Dab.WorldPosition.z;
                for (int X = Rect.x; X <= Rect.z; ++X)
                {
                    float WorldX = OriginXZ.x + float(X) * Stride;
                    float DX = WorldX - Dab.WorldPosition.x;
                    float W = BrushWeight(DX * DX + DZ * DZ, Radius, Dab.Falloff);
                    if (W <= 0.0f)
                    {
                        continue;
                    }

                    const size_t Pixel = size_t(Y) * size_t(Res) + size_t(X);
                    uint8& Target  = Terrain.LayerWeights[size_t(Layer) * LayerStride + Pixel];
                    float  Current = float(Target) / 255.0f;
                    float  Goal    = (Dab.SculptSign >= 0) ? 1.0f : 0.0f;
                    float  New     = Math::Mix(Current, Goal, Speed * W);
                    Target = uint8(Math::Clamp(New * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            }
        }, 64u);
    }
}
