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
    enum class EPrimitiveShape : uint8
    {
        Cube,
        Sphere,
        Plane,
        Cylinder,
        Cone,
        Capsule,
    };

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

        // The engine's built-in primitives span one meter, so a box of size S is this mesh at scale S.
        FUNCTION()
        static CStaticMesh* GetPrimitiveMesh(EPrimitiveShape Shape);
    };
}
