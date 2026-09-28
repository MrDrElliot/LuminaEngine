#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "MeshLibrary.generated.h"

namespace Lumina
{
    class CWorld;
    class CStaticMesh;
    class CMaterialInterface;

    REFLECT()
    class RUNTIME_API CMeshLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Script geometry as a real static mesh, so it can be instanced, used as foliage or fractured. The world keeps it alive.
        FUNCTION()
        static CStaticMesh* CreateStaticMesh(CWorld* World, const float* Positions, int32 PositionFloats,
            const float* Normals, int32 NormalFloats, const float* UVs, int32 UVFloats,
            const float* Colors, int32 ColorFloats, const uint32* Indices, int32 IndexCount,
            CMaterialInterface* Material, int32 MaxLODs = 1);
    };
}
