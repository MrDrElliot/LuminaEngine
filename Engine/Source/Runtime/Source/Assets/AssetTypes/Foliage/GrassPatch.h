#pragma once

#include "Renderer/MeshData.h"

namespace Lumina::GrassPatch
{
    struct FParams
    {
        // Blades per square meter, which the patch holds exactly over its square.
        float  Density    = 1.0f;
        float  ScaleMin   = 1.0f;
        float  ScaleMax   = 1.0f;
        bool   bRandomYaw = true;
        uint32 Seed       = 0;
    };

    // Side of the square one patch covers, sized to hold about the same number of blades at any density.
    RUNTIME_API float GetPatchSize(float Density);

    // Any thread. Copies Blade's build streams across a patch square centered on the origin, then builds its cluster hierarchy into Out.
    RUNTIME_API bool Build(const FMeshResource& Blade, const FParams& Params, FMeshResource& Out);
}
