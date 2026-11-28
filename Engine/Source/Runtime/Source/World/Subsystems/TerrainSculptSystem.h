#pragma once

#include "Core/Templates/NumericLimits.h"

#include "Core/Math/Math.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    struct STerrainComponent;
    struct FTerrainSculptWindow;
    class FTerrainStore;

    // What a brush stroke does to the terrain, in the order the edit mode's palette and number keys list them.
    enum class ETerrainBrushMode : uint8
    {
        Sculpt,
        Smooth,
        Flatten,
        Ramp,
        Noise,
        Erode,
        Terrace,
        Paint,
        Num,
    };

    // The curve a brush's weight follows from its full core down to zero at its rim.
    enum class ETerrainBrushFalloff : uint8
    {
        Smooth,
        Linear,
        Sphere,
        Tip,
        Num,
    };

    // Which way Flatten may move the ground toward its target.
    enum class ETerrainFlattenSide : uint8
    {
        Both,
        RaiseOnly,
        LowerOnly,
        Num,
    };

    // A single brush dab captured during a stroke and replayed against the CPU heightmap.
    struct FTerrainSculptDab
    {
        ETerrainBrushMode    Mode          = ETerrainBrushMode::Sculpt;
        ETerrainBrushFalloff FalloffShape  = ETerrainBrushFalloff::Smooth;
        FVector3             WorldPosition = FVector3(0.0f);
        // The owning entity's position, which the terrain centers on.
        FVector3             TerrainOrigin = FVector3(0.0f);
        float                Radius        = 512.0f;
        // From 0 to 1 in every mode, which Sculpt and Noise turn into a height rate scaled by Radius and the rest into a blend rate.
        float                Strength      = 0.5f;
        // Share of the radius over which the weight falls to zero, so 0 is a hard disk.
        float                Falloff       = 0.5f;
        float                DeltaSeconds  = 0.0f;
        int32                ActiveLayer   = 0;
        int8                 SculptSign    = 1;

        // World units above the terrain origin.
        float                FlattenHeight = 0.0f;
        ETerrainFlattenSide  FlattenSide   = ETerrainFlattenSide::Both;

        // World-space frequency in 1/units.
        float                NoiseFrequency = 1.0f / 512.0f;
        int32                NoiseOctaves   = 4;

        // The steepest slope in degrees that Erode leaves standing.
        float                TalusAngle     = 35.0f;

        // World units between terrace levels, and how sheer the risers between them are from 0 to 1.
        float                TerraceStep      = 32.0f;
        float                TerraceSharpness = 0.7f;

        // Only XZ is used, and heights come from the terrain under the endpoints unless RampUseExplicitHeights is set.
        FVector3             RampStart              = FVector3(0.0f);
        FVector3             RampEnd                = FVector3(0.0f);
        bool                 RampUseExplicitHeights = false;
        float                RampStartHeight        = 0.0f;
        float                RampEndHeight          = 0.0f;
        // Half-width of the ramp's linear core, past which the falloff applies.
        float                RampHalfWidth          = 256.0f;
    };

    // A terrain's heights as the renderer draws them, which a caller may hold a copy of and sample from any thread.
    struct RUNTIME_API FTerrainHeightView
    {
        TSharedPtr<FTerrainStore> Store;
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
        // Apply a single dab, dispatching row updates to worker threads.
        static void ApplyDab(STerrainComponent& Terrain, const FTerrainSculptDab& Dab);

        // Brush weight at Distance from the center, 1 inside the core and 0 at and past Radius, shared with the editor's falloff preview.
        static float FalloffWeight(float Distance, float Radius, float Falloff, ETerrainBrushFalloff Shape);

        // Raycast against the current heightmap, where TerrainOrigin is the entity position the terrain centers on.
        static bool Raycast(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, const FVector3& RayOrigin, const FVector3& RayDir, FVector3& OutHit,
                            float MaxDistance = TNumericLimits<float>::Max());

        // Bilinear surface height at (WorldX, WorldZ), false outside the terrain footprint.
        static bool SampleHeight(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ, float& OutHeight);

        // Surface normal at (WorldX, WorldZ) by central differences, world up off the terrain.
        static FVector3 SampleNormal(const STerrainComponent& Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ);

        // A null store when the terrain has no data yet.
        static FTerrainHeightView MakeHeightView(const STerrainComponent& Terrain, const FVector3& TerrainOrigin);

    private:
        static void ApplySculpt (const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyFlatten(const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplySmooth (const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyNoise  (const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyRamp   (const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyErode  (const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyTerrace(const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
        static void ApplyPaint  (const STerrainComponent& Terrain, FTerrainSculptWindow& Win, const FTerrainSculptDab& Dab, const FIntVector4& Rect);
    };
}
