#pragma once

#include "Core/Object/ObjectMacros.h"
#include "GrassComponent.generated.h"

namespace Lumina
{
    /**
     * Enables GPU grass on the terrain entity that carries it. Deliberately holds no instances: the
     * species come from the terrain material's GrassOutputs and the blades are generated on the GPU
     * every frame, so nothing here is ever serialized per blade.
     */
    REFLECT(Component, Category = "Terrain", HideInComponentList)
    struct RUNTIME_API SGrassComponent
    {
        GENERATED_BODY()

        /** Scatter radius around the camera. Species with a shorter CullDistance stop before this. */
        PROPERTY(Editable, Category = "Grass", ClampMin = 1.0f, NoDrag, Delta = 256.0f)
        float MaxDrawDistance = 8192.0f;

        /**
         * Per-species instance budget. The scatter clamps its append against this, so a dense species on
         * a large radius loses blades rather than writing past the allocation.
         *
         * These slots are reserved in the retained instance arrays for as long as the species is alive, at
         * roughly 80 bytes each across the three of them, so this is a memory dial and not just a cap.
         */
        PROPERTY(Editable, Category = "Grass", ClampMin = 1024, ClampMax = 4194304, NoDrag, Delta = 16384)
        uint32 MaxInstancesPerSpecies = 1u << 16;

        PROPERTY(Editable, Category = "Grass")
        bool bEnabled = true;
    };
}
