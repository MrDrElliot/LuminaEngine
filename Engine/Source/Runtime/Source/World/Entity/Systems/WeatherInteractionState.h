#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    enum class EWeatherSurface : uint8
    {
        None,
        Ground,
        Water,
    };

    // A contact sphere this frame and where it was the frame before, so the renderer can measure the water it pushed aside.
    struct FWeatherContact
    {
        FVector3        Position       = FVector3(0.0f);
        float           Radius         = 0.0f;
        FVector3        PrevPosition   = FVector3(0.0f);
        // Height of the ground or water under the sphere, now and last frame.
        float           SurfaceY       = 0.0f;
        float           PrevSurfaceY   = 0.0f;
        float           RippleStrength = 1.0f;
        float           FoamStrength   = 1.0f;
        float           TrailStrength  = 1.0f;
        EWeatherSurface Surface        = EWeatherSurface::None;
    };

    // A contact striking a surface hard enough to throw water, which gameplay can read to play a matching sound.
    struct FWeatherImpact
    {
        FVector3        Position = FVector3(0.0f);
        FVector3        Normal   = FVector3(0.0f, 1.0f, 0.0f);
        // Meters per second into the surface.
        float           Speed    = 0.0f;
        float           Radius   = 0.0f;
        float           Scale    = 1.0f;
        EWeatherSurface Surface  = EWeatherSurface::None;
        uint32          Entity   = 0;
    };

    // How wet an entity is, which its meshes read back by entity id when they draw.
    struct FWeatherInteractorState
    {
        uint32 EntityID         = 0;
        float  RootY            = 0.0f;
        float  Soak             = 0.0f;
        // Meters above RootY the water reached, and how wet the skin under that line still is.
        float  WaterlineHeight  = 0.0f;
        float  WaterlineWetness = 0.0f;
    };

    // What every Weather Interactor did this frame, written by SWeatherInteractionSystem into the registry context.
    struct FWeatherInteractionState
    {
        TVector<FWeatherContact>         Contacts;
        TVector<FWeatherImpact>          Impacts;
        TVector<FWeatherInteractorState> Interactors;
    };
}
