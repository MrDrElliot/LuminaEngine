#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"
#include "World/Entity/Components/TerrainBrushComponent.h"

namespace Lumina
{
    struct STerrainComponent;

    // A single brush dab captured during a stroke and replayed against the CPU heightmap.
    struct FTerrainSculptDab
    {
        ETerrainBrushMode   Mode          = ETerrainBrushMode::Sculpt;
        FVector3           WorldPosition = FVector3(0.0f);
        /** Owning terrain entity's transform origin. The terrain is centered on this. */
        FVector3           TerrainOrigin = FVector3(0.0f);
        float               Radius        = 0.0f;
        float               Strength      = 0.0f;
        float               Falloff       = 0.5f;
        float               FlattenHeight = 0.0f;
        float               DeltaSeconds  = 0.0f;
        int32               ActiveLayer   = 0;
        int8                SculptSign    = 1;

        /** Noise mode parameters (world-space frequency in 1/units, octaves >= 1). */
        float               NoiseFrequency = 1.0f / 512.0f;
        int32               NoiseOctaves   = 4;

        /** Ramp mode endpoints in world space; only XZ used. Heights are sampled from
         *  the heightmap at the endpoints unless RampUseExplicitHeights is set. */
        FVector3           RampStart      = FVector3(0.0f);
        FVector3           RampEnd        = FVector3(0.0f);
        bool                RampUseExplicitHeights = false;
        float               RampStartHeight = 0.0f;
        float               RampEndHeight   = 0.0f;
        /** Half-width of the ramp's linear core (world units). Beyond this, falloff applies. */
        float               RampHalfWidth   = 0.0f;
    };

    // A terrain's heights as the renderer draws them, which a caller may hold a copy of and sample from any thread.
    struct RUNTIME_API FTerrainHeightView
    {
        const float* Heights       = nullptr;
        int32        Resolution    = 0;
        float        TileWorldSize = 0.0f;
        float        MaxHeight     = 0.0f;
        // The owning entity's world position, which the terrain centers on.
        FVector3     Origin        = FVector3(0.0f);

        bool     SampleHeight(float WorldX, float WorldZ, float& OutHeight) const;
        // World up when the point is off the terrain.
        FVector3 SampleNormal(float WorldX, float WorldZ) const;
    };

    // Stateless brush application parallelized across footprint rows, so apply one stroke at a time per component.
    class RUNTIME_API FTerrainSculptSystem
    {
    public:
        /** Apply a single dab, dispatching row updates to worker threads. */
        static void ApplyDab(STerrainComponent& Terrain, const FTerrainSculptDab& Dab);

        /** Convert a world-space XYZ point to heightmap sample coordinates. */
        static FIntVector2 WorldToSample(const STerrainComponent& Terrain, const FVector3& WorldPos);

        // Raycast against the current heightmap, where TerrainOrigin is the entity position the terrain centers on.
        static bool Raycast(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, const FVector3& RayOrigin, const FVector3& RayDir, FVector3& OutHit);

        // Bilinear surface height at (WorldX, WorldZ), false outside the terrain footprint.
        static bool SampleHeight(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ, float& OutHeight);

        // Surface normal at (WorldX, WorldZ) by central differences, world up off the terrain.
        static FVector3 SampleNormal(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ);

        // Null heights when the heightmap does not hold Resolution squared samples.
        static FTerrainHeightView MakeHeightView(const STerrainComponent& Terrain, const FVector3& TerrainOrigin);

    private:
        static void ApplySculpt (STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyFlatten(STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplySmooth (STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyNoise  (STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyRamp   (STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyPaint  (STerrainComponent& Terrain, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
    };
}
