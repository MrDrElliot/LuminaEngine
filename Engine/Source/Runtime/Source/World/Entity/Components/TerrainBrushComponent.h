#pragma once

#include "TerrainBrushComponent.generated.h"

namespace Lumina
{
    /** What a brush stroke does to the target terrain. */
    REFLECT()
    enum class ETerrainBrushMode : uint8
    {
        Sculpt,     // Raise / lower based on stroke sign.
        Flatten,    // Drive heights toward a locked reference height.
        Smooth,     // Blur heights within the brush footprint.
        Noise,      // Add fbm noise within the brush footprint.
        Ramp,       // Drag a line; heights ramp linearly between endpoints.
        Paint,      // Apply weights for ActiveLayer.
    };

    /**
     * Transient component that exists on a scratch entity only while the world
     * editor's terrain edit mode is active. It carries the live brush cursor
     * state consumed by the async sculpt task each tick.
     */
    REFLECT(Component, Transient, Category = "Terrain")
    struct RUNTIME_API STerrainBrushComponent
    {
        GENERATED_BODY()

        /** Current brush operation. */
        PROPERTY(Editable, Category = "Brush")
        ETerrainBrushMode Mode = ETerrainBrushMode::Sculpt;

        /** Brush radius in world units. */
        PROPERTY(Editable, Category = "Brush", ClampMin = 1.0f)
        float Radius = 256.0f;

        /** Unit strength applied per second while the stroke is held. */
        PROPERTY(Editable, Category = "Brush", ClampMin = 0.0f)
        float Strength = 1.0f;

        /** Soft edge falloff, 0 gives a hard disk, 1 a full cosine taper. */
        PROPERTY(Editable, Category = "Brush", ClampMin = 0.0f, ClampMax = 1.0f)
        float Falloff = 0.5f;

        /** Height targeted by Flatten mode in world units, relative to terrain origin. */
        PROPERTY(Editable, Category = "Brush")
        float FlattenHeight = 0.0f;

        /** Active layer index for Paint mode. */
        PROPERTY(Editable, Category = "Brush", ClampMin = 0)
        int32 ActiveLayer = 0;

        /** World-space brush center raycast from the viewport cursor each frame. */
        FVector3 WorldPosition = FVector3(0.0f);

        /** True while the mouse button is held, consumed by the sculpt task. */
        bool bStrokeActive = false;

        /** True when WorldPosition is a valid terrain hit this frame. */
        bool bHitValid = false;

        /** Sign applied to Sculpt strokes; -1 when modifier held, +1 otherwise. */
        int8 SculptSign = 1;
    };
}
