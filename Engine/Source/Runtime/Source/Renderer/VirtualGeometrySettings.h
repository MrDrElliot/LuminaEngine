#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Platform/Platform.h"
#include "Lumina.h"
#include "VirtualGeometrySettings.generated.h"

namespace Lumina
{
    // Build inputs for a mesh's virtual geometry. Lives on CMesh so a rebuild reproduces the import result.
    REFLECT()
    struct RUNTIME_API SVirtualGeometryBuildSettings
    {
        GENERATED_BODY()

        // Replaces the discrete LOD chain with a cluster hierarchy the GPU cuts per meshlet; static meshes only.
        PROPERTY(Editable, Category = "Virtual Geometry")
        bool bClusterLOD = false;

        // Bakes voxel splat levels past the coarsest geometry, which aggregates such as foliage coarsen into better than triangles do.
        PROPERTY(Editable, Category = "Virtual Geometry")
        bool bVoxelFarRepresentation = false;

        // Voxels along the longest side of the mesh at the finest voxel level; each level above halves it.
        PROPERTY(Editable, Category = "Virtual Geometry", ClampMin = "4", ClampMax = "128")
        uint32 VoxelResolution = 128;

        // Multiplies the voxel levels' errors, so above 1 the splats take over further away.
        PROPERTY(Editable, Category = "Virtual Geometry", ClampMin = "0.25", ClampMax = "8.0")
        float VoxelErrorScale = 1.0f;
    };
}
