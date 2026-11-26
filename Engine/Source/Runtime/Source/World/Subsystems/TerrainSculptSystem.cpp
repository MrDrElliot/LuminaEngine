#include "RuntimePCH.h"
#include "TerrainSculptSystem.h"
#include "Memory/Memcpy.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/TerrainComponent.h"

namespace Lumina
{
    namespace
    {
        // At full strength a held sculpt moves the center by the smaller of the brush radius and the terrain's height range per second.
        constexpr float kSculptRate = 1.0f;
        // Blend modes close this many e-folds of the gap to their target per second at full strength.
        constexpr float kBlendRate = 6.0f;
        // The smooth blur's reach as a share of the brush radius, so a wide brush relaxes wide features and not only the finest grain.
        constexpr float kSmoothKernelShare = 0.15f;
        constexpr int32 kMaxSmoothKernel   = 24;
        // Erode passes per dab, each sliding material at most one sample downhill.
        constexpr int32 kErodePasses = 4;
        constexpr float kErodeRate   = 30.0f;

        // Matches DefaultSceneRenderer OriginXZ so brush hits the same samples the renderer sees.
        FVector2 TerrainOriginXZ(const STerrainComponent& Terrain, const FVector3& TerrainOrigin)
        {
            const float HalfSize = Terrain.TileWorldSize * 0.5f;
            return FVector2(TerrainOrigin.x - HalfSize, TerrainOrigin.z - HalfSize);
        }

        float SampleStride(const STerrainComponent& Terrain)
        {
            return Terrain.TileWorldSize / float(Terrain.Resolution - 1);
        }

        FIntVector4 ComputeSampleRect(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, const FVector3& WorldPos, float Radius)
        {
            const float Stride = SampleStride(Terrain);
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

        // Normalized height a full-weight sample of a Sculpt or Noise dab moves by.
        float SculptDelta(const STerrainComponent& Terrain, const FTerrainSculptDab& Dab)
        {
            const float MaxHeight = Math::Max(Terrain.MaxHeight, 1e-3f);
            const float Rate      = Dab.Strength * kSculptRate * Math::Min(Dab.Radius, MaxHeight);
            return Rate * Dab.DeltaSeconds * float(Dab.SculptSign) / MaxHeight;
        }

        // Share of the gap to its target a full-weight sample of a blend dab closes, the same over a second at any frame rate.
        float BlendAlpha(const FTerrainSculptDab& Dab)
        {
            return 1.0f - Math::Exp(-Math::Max(Dab.Strength, 0.0f) * kBlendRate * Dab.DeltaSeconds);
        }

        // Runs Op(X, Y, Weight) for every sample of Rect the brush reaches, with rows spread over the workers.
        template<typename TOp>
        void ForEachBrushSample(const STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect, TOp&& Op)
        {
            const float    Stride   = SampleStride(Terrain);
            const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);
            Jobs::ParallelFor(uint32(Rect.w - Rect.y + 1), [&](const Jobs::FRange& Range)
            {
                for (uint32 RowIdx = Range.Start; RowIdx < Range.End; ++RowIdx)
                {
                    const int32 Y  = Rect.y + int32(RowIdx);
                    const float DZ = OriginXZ.y + float(Y) * Stride - Dab.WorldPosition.z;
                    for (int32 X = Rect.x; X <= Rect.z; ++X)
                    {
                        const float DX = OriginXZ.x + float(X) * Stride - Dab.WorldPosition.x;
                        const float W  = FTerrainSculptSystem::FalloffWeight(std::sqrt(DX * DX + DZ * DZ), Dab.Radius, Dab.Falloff, Dab.FalloffShape);
                        if (W > 0.0f)
                        {
                            Op(X, Y, W);
                        }
                    }
                }
            }, 64u);
        }

        // A block of heights copied out around a footprint, so a brush that reads neighbors sees the surface from before the dab.
        struct FHeightBlock
        {
            int32          MinX   = 0;
            int32          MinY   = 0;
            int32          Width  = 0;
            int32          Height = 0;
            TVector<float> Values;

            FHeightBlock(const STerrainComponent& Terrain, const FIntVector4& Rect, int32 Apron)
            {
                const int32 Res = Terrain.Resolution;
                MinX   = Math::Max(Rect.x - Apron, 0);
                MinY   = Math::Max(Rect.y - Apron, 0);
                Width  = Math::Min(Rect.z + Apron, Res - 1) - MinX + 1;
                Height = Math::Min(Rect.w + Apron, Res - 1) - MinY + 1;
                Values.SetNum(size_t(Width) * size_t(Height));
                for (int32 Y = 0; Y < Height; ++Y)
                {
                    Memory::Memcpy(&Values[size_t(Y) * Width], &Terrain.Heightmap[(MinY + Y) * Res + MinX], size_t(Width) * sizeof(float));
                }
            }

            bool Contains(int32 X, int32 Y) const
            {
                return X >= MinX && Y >= MinY && X < MinX + Width && Y < MinY + Height;
            }

            float& At(int32 X, int32 Y) { return Values[size_t(Y - MinY) * Width + (X - MinX)]; }
        };

        // One box pass of half-width K over a Width by Height block, along rows or columns, clamping reads at the block edge.
        void BoxBlur(const TVector<float>& Src, TVector<float>& Dst, int32 Width, int32 Height, int32 K, bool bColumns)
        {
            const int32  Lines    = bColumns ? Width : Height;
            const int32  Length   = bColumns ? Height : Width;
            const size_t Step     = bColumns ? size_t(Width) : 1u;
            const size_t LineStep = bColumns ? 1u : size_t(Width);
            const float  Inv      = 1.0f / float(2 * K + 1);
            Jobs::ParallelFor(uint32(Lines), [&](const Jobs::FRange& Range)
            {
                for (uint32 Line = Range.Start; Line < Range.End; ++Line)
                {
                    const float* In  = Src.GetData() + Line * LineStep;
                    float*       Out = Dst.GetData() + Line * LineStep;
                    float Sum = 0.0f;
                    for (int32 I = -K; I <= K; ++I)
                    {
                        Sum += In[size_t(Math::Clamp(I, 0, Length - 1)) * Step];
                    }
                    for (int32 I = 0; I < Length; ++I)
                    {
                        Out[size_t(I) * Step] = Sum * Inv;
                        Sum += In[size_t(Math::Min(I + K + 1, Length - 1)) * Step] - In[size_t(Math::Max(I - K, 0)) * Step];
                    }
                }
            }, 16u);
        }
    }

    float FTerrainSculptSystem::FalloffWeight(float Distance, float Radius, float Falloff, ETerrainBrushFalloff Shape)
    {
        if (Distance >= Radius)
        {
            return 0.0f;
        }
        const float Soft = Radius * Math::Saturate(Falloff);
        if (Soft <= 1e-4f)
        {
            return 1.0f;
        }

        // U runs from 0 at the rim to 1 where the full-strength core begins.
        const float U = Math::Saturate((Radius - Distance) / Soft);
        switch (Shape)
        {
        case ETerrainBrushFalloff::Linear: return U;
        case ETerrainBrushFalloff::Sphere: return std::sqrt(Math::Max(1.0f - (1.0f - U) * (1.0f - U), 0.0f));
        case ETerrainBrushFalloff::Tip:    return 1.0f - std::sqrt(Math::Max(1.0f - U * U, 0.0f));
        default:                           return U * U * (3.0f - 2.0f * U);
        }
    }

    FTerrainHeightView FTerrainSculptSystem::MakeHeightView(const STerrainComponent& Terrain, const FVector3& TerrainOrigin)
    {
        FTerrainHeightView View;
        const int32 Res = Terrain.Resolution;
        if (Res >= 2 && static_cast<size_t>(Terrain.Heightmap.Num()) >= size_t(Res) * size_t(Res))
        {
            View.Heights = Terrain.Heightmap.GetData();
        }
        View.Resolution    = Res;
        View.TileWorldSize = Terrain.TileWorldSize;
        View.MaxHeight     = Terrain.MaxHeight;
        View.Origin        = TerrainOrigin;
        return View;
    }

    bool FTerrainHeightView::SampleHeight(float WorldX, float WorldZ, float& OutHeight) const
    {
        const int32 Res = Resolution;
        if (Heights == nullptr || Res < 2)
        {
            return false;
        }

        // Matches DefaultSceneRenderer OriginXZ, the terrain centered on its entity.
        const float OriginX = Origin.x - TileWorldSize * 0.5f;
        const float OriginZ = Origin.z - TileWorldSize * 0.5f;
        const float Stride  = TileWorldSize / float(Res - 1);

        const float Lx = (WorldX - OriginX) / Stride;
        const float Lz = (WorldZ - OriginZ) / Stride;
        if (Lx < 0.0f || Lz < 0.0f || Lx > float(Res - 1) || Lz > float(Res - 1))
        {
            return false;
        }

        const int X0 = int(Lx), Z0 = int(Lz);
        const int X1 = Math::Min(X0 + 1, Res - 1), Z1 = Math::Min(Z0 + 1, Res - 1);
        const float Tx = Lx - float(X0), Tz = Lz - float(Z0);
        const float H00 = Heights[Z0 * Res + X0];
        const float H10 = Heights[Z0 * Res + X1];
        const float H01 = Heights[Z1 * Res + X0];
        const float H11 = Heights[Z1 * Res + X1];
        const float H   = Math::Mix(Math::Mix(H00, H10, Tx), Math::Mix(H01, H11, Tx), Tz);

        OutHeight = Origin.y + H * MaxHeight;
        return true;
    }

    FVector3 FTerrainHeightView::SampleNormal(float WorldX, float WorldZ) const
    {
        const float Stride = TileWorldSize / float(Math::Max(Resolution - 1, 1));
        const float Eps    = Math::Max(Stride, 0.5f);

        float HxL, HxR, HzL, HzR;
        if (!SampleHeight(WorldX - Eps, WorldZ, HxL)) { return FVector3(0.0f, 1.0f, 0.0f); }
        if (!SampleHeight(WorldX + Eps, WorldZ, HxR)) { return FVector3(0.0f, 1.0f, 0.0f); }
        if (!SampleHeight(WorldX, WorldZ - Eps, HzL)) { return FVector3(0.0f, 1.0f, 0.0f); }
        if (!SampleHeight(WorldX, WorldZ + Eps, HzR)) { return FVector3(0.0f, 1.0f, 0.0f); }

        const float dHdx = (HxR - HxL) / (2.0f * Eps);
        const float dHdz = (HzR - HzL) / (2.0f * Eps);
        return Math::Normalize(FVector3(-dHdx, 1.0f, -dHdz));
    }

    bool FTerrainSculptSystem::SampleHeight(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ, float& OutHeight)
    {
        return MakeHeightView(Terrain, TerrainOrigin).SampleHeight(WorldX, WorldZ, OutHeight);
    }

    FVector3 FTerrainSculptSystem::SampleNormal(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ)
    {
        return MakeHeightView(Terrain, TerrainOrigin).SampleNormal(WorldX, WorldZ);
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

        // Bilinear so the hit tracks the rendered surface instead of snapping to texels.
        auto SurfaceY = [&](float Wx, float Wz)
        {
            const float Fx = Math::Clamp((Wx - OriginXZ.x) / Stride, 0.0f, float(Res - 1));
            const float Fz = Math::Clamp((Wz - OriginXZ.y) / Stride, 0.0f, float(Res - 1));
            const int X0 = int(Fx), Z0 = int(Fz);
            const int X1 = Math::Min(X0 + 1, Res - 1), Z1 = Math::Min(Z0 + 1, Res - 1);
            const float Tx = Fx - float(X0), Tz = Fz - float(Z0);
            const float H00 = Terrain.Heightmap[Z0 * Res + X0];
            const float H10 = Terrain.Heightmap[Z0 * Res + X1];
            const float H01 = Terrain.Heightmap[Z1 * Res + X0];
            const float H11 = Terrain.Heightmap[Z1 * Res + X1];
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
                // Downward crossing between Prev and Pos, so bisect for a tight hit.
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
        if (Terrain.Resolution <= 0 || Terrain.Heightmap.IsEmpty())
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
        case ETerrainBrushMode::Erode:   ApplyErode  (Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Terrace: ApplyTerrace(Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Paint:   ApplyPaint  (Terrain, Dab, Rect); break;
        case ETerrainBrushMode::Num:     break;
        }

        if (Dab.Mode == ETerrainBrushMode::Paint)
        {
            const int32 Layer = Dab.ActiveLayer;
            if (Layer >= 0 && Layer < Terrain.Layers.Num())
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
        const float Delta = SculptDelta(Terrain, Dab);
        const int32 Res   = Terrain.Resolution;
        ForEachBrushSample(Terrain, Dab, Rect, [&](int32 X, int32 Y, float W)
        {
            float& H = Terrain.Heightmap[Y * Res + X];
            H = Math::Clamp(H + Delta * W, 0.0f, 1.0f);
        });
    }

    void FTerrainSculptSystem::ApplyFlatten(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const float Target = Terrain.MaxHeight > 0.0f ? Math::Clamp(Dab.FlattenHeight / Terrain.MaxHeight, 0.0f, 1.0f) : 0.0f;
        const float Alpha  = BlendAlpha(Dab);
        const int32 Res    = Terrain.Resolution;
        ForEachBrushSample(Terrain, Dab, Rect, [&](int32 X, int32 Y, float W)
        {
            float& H = Terrain.Heightmap[Y * Res + X];
            const float Flat = Math::Mix(H, Target, Alpha * W);
            switch (Dab.FlattenSide)
            {
            case ETerrainFlattenSide::RaiseOnly: H = Math::Max(H, Flat); break;
            case ETerrainFlattenSide::LowerOnly: H = Math::Min(H, Flat); break;
            default:                             H = Flat;               break;
            }
        });
    }

    void FTerrainSculptSystem::ApplySmooth(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const int32 Kernel = Math::Clamp(int32(Dab.Radius * kSmoothKernelShare / SampleStride(Terrain) + 0.5f), 1, kMaxSmoothKernel);

        // Two box passes approach a Gaussian, and each reads a kernel of real neighbors past the footprint.
        FHeightBlock Block(Terrain, Rect, 2 * Kernel);
        TVector<float> Scratch;
        Scratch.SetNum(Block.Values.Num());
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            BoxBlur(Block.Values, Scratch, Block.Width, Block.Height, Kernel, false);
            BoxBlur(Scratch, Block.Values, Block.Width, Block.Height, Kernel, true);
        }

        const float Alpha = BlendAlpha(Dab);
        const int32 Res   = Terrain.Resolution;
        ForEachBrushSample(Terrain, Dab, Rect, [&](int32 X, int32 Y, float W)
        {
            float& H = Terrain.Heightmap[Y * Res + X];
            H = Math::Mix(H, Block.At(X, Y), Alpha * W);
        });
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
        const int32    Res      = Terrain.Resolution;
        const float    Stride   = SampleStride(Terrain);
        const float    Delta    = SculptDelta(Terrain, Dab);
        const float    Freq     = Math::Max(Dab.NoiseFrequency, 1e-6f);
        const int      Octaves  = Math::Clamp(Dab.NoiseOctaves, 1, 8);
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);
        ForEachBrushSample(Terrain, Dab, Rect, [&](int32 X, int32 Y, float W)
        {
            const float N = Fbm2D((OriginXZ.x + float(X) * Stride) * Freq, (OriginXZ.y + float(Y) * Stride) * Freq, Octaves);
            float& H = Terrain.Heightmap[Y * Res + X];
            H = Math::Clamp(H + Delta * W * N, 0.0f, 1.0f);
        });
    }

    void FTerrainSculptSystem::ApplyRamp(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const int32 Res    = Terrain.Resolution;
        const float Stride = SampleStride(Terrain);
        const float Speed  = BlendAlpha(Dab);
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);

        const FVector2 A   = FVector2(Dab.RampStart.x, Dab.RampStart.z);
        const FVector2 B   = FVector2(Dab.RampEnd.x,   Dab.RampEnd.z);
        const FVector2 AB  = B - A;
        const float ABLen2  = Math::Dot(AB, AB);
        if (ABLen2 < 1e-3f)
        {
            return;
        }

        // Heights are relative to the terrain origin, or ramps break once the entity moves off Y=0.
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
        Jobs::ParallelFor((uint32)RowCount, [&, Rect, Speed, Stride, OriginXZ, A, AB, ABLen2, StartN, EndN, HalfWidth, InvLen, Feather](const Jobs::FRange& Range)
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

                    // Lateral feather so the ramp blends in instead of forming a vertical wall.
                    const float Edge = Perp / HalfWidth;       // 0 center, 1 edge
                    float Lat = 1.0f;
                    if (Feather > 1e-4f)
                    {
                        const float U = Math::Clamp((Edge - (1.0f - Feather)) / Feather, 0.0f, 1.0f);
                        Lat = 1.0f - U * U * (3.0f - 2.0f * U);
                    }

                    // Longitudinal feather so the ramp tapers in rather than stamping a flat disc.
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

    void FTerrainSculptSystem::ApplyErode(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const float Stride = SampleStride(Terrain);
        // Normalized height a straight neighbor may stand below a sample before material slides onto it.
        const float Talus = std::tan(Math::Radians(Math::Clamp(Dab.TalusAngle, 1.0f, 89.0f))) * Stride / Math::Max(Terrain.MaxHeight, 1e-3f);
        // Under a sixteenth, a sample standing above all eight neighbors cannot shed past them in one pass.
        const float Rate = Math::Saturate(Dab.Strength * Dab.DeltaSeconds * kErodeRate) / 16.0f;
        if (Rate <= 0.0f)
        {
            return;
        }

        // Each pair trades by the smaller of its two weights, so whatever one sample sheds its neighbor gains.
        FHeightBlock Weights(Terrain, Rect, 1);
        const FVector2 OriginXZ = TerrainOriginXZ(Terrain, Dab.TerrainOrigin);
        for (int32 Y = Weights.MinY; Y < Weights.MinY + Weights.Height; ++Y)
        {
            const float DZ = OriginXZ.y + float(Y) * Stride - Dab.WorldPosition.z;
            for (int32 X = Weights.MinX; X < Weights.MinX + Weights.Width; ++X)
            {
                const float DX = OriginXZ.x + float(X) * Stride - Dab.WorldPosition.x;
                Weights.At(X, Y) = FalloffWeight(std::sqrt(DX * DX + DZ * DZ), Dab.Radius, Dab.Falloff, Dab.FalloffShape);
            }
        }

        const int32 Res = Terrain.Resolution;
        for (int32 Pass = 0; Pass < kErodePasses; ++Pass)
        {
            FHeightBlock Before(Terrain, Rect, 1);
            ForEachBrushSample(Terrain, Dab, Rect, [&](int32 X, int32 Y, float)
            {
                const float Here       = Before.At(X, Y);
                const float WeightHere = Weights.At(X, Y);
                float Change = 0.0f;
                for (int32 OY = -1; OY <= 1; ++OY)
                {
                    for (int32 OX = -1; OX <= 1; ++OX)
                    {
                        if ((OX == 0 && OY == 0) || !Before.Contains(X + OX, Y + OY))
                        {
                            continue;
                        }
                        const float Limit = (OX != 0 && OY != 0) ? Talus * 1.41421356f : Talus;
                        const float Drop  = Before.At(X + OX, Y + OY) - Here;
                        const float Pair  = Math::Min(WeightHere, Weights.At(X + OX, Y + OY));
                        if (Drop > Limit)
                        {
                            Change += (Drop - Limit) * Pair;
                        }
                        else if (-Drop > Limit)
                        {
                            Change -= (-Drop - Limit) * Pair;
                        }
                    }
                }
                Terrain.Heightmap[Y * Res + X] = Math::Clamp(Here + Rate * Change, 0.0f, 1.0f);
            });
        }
    }

    void FTerrainSculptSystem::ApplyTerrace(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const float Step  = Math::Max(Dab.TerraceStep, 0.01f) / Math::Max(Terrain.MaxHeight, 1e-3f);
        const float Alpha = BlendAlpha(Dab);
        const int32 Res   = Terrain.Resolution;
        ForEachBrushSample(Terrain, Dab, Rect, [&](int32 X, int32 Y, float W)
        {
            // Each step eases from tread to tread, so holding the brush keeps sharpening the risers.
            float& H = Terrain.Heightmap[Y * Res + X];
            const float Level  = H / Step;
            const float Tread  = std::floor(Level);
            const float Rise   = Level - Tread;
            const float Target = (Tread + Rise * Rise * (3.0f - 2.0f * Rise)) * Step;
            H = Math::Clamp(Math::Mix(H, Target, Alpha * W), 0.0f, 1.0f);
        });
    }

    void FTerrainSculptSystem::ApplyPaint(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect)
    {
        const int32 Layer = Dab.ActiveLayer;
        if (Layer < 0 || Layer >= Terrain.Layers.Num())
        {
            return;
        }

        const int32 Res         = Terrain.Resolution;
        const int32 LayerStride = Res * Res;
        const int32 Needed      = Terrain.Layers.Num() * LayerStride;
        if (Terrain.LayerWeights.Num() < Needed)
        {
            Terrain.LayerWeights.SetNum(Needed, 0u);
        }

        const float Alpha = BlendAlpha(Dab);
        const float Goal  = Dab.SculptSign >= 0 ? 1.0f : 0.0f;
        ForEachBrushSample(Terrain, Dab, Rect, [&](int32 X, int32 Y, float W)
        {
            uint8& Target = Terrain.LayerWeights[Layer * LayerStride + Y * Res + X];
            const float New = Math::Mix(float(Target) / 255.0f, Goal, Alpha * W);
            Target = uint8(Math::Clamp(New * 255.0f + 0.5f, 0.0f, 255.0f));
        });
    }
}
