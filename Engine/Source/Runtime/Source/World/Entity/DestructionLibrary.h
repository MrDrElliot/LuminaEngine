#pragma once

#include "Core/Math/Transform.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "DestructionLibrary.generated.h"

namespace Lumina
{
    class CWorld;
    class CStaticMesh;

    // Script access to runtime fracture, which splits a mesh into physics chunks that fly apart from a blast point.
    REFLECT()
    class RUNTIME_API CDestructionLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Shatters an entity carrying SDestructibleComponent, with Strength zero taking the component's own launch speed.
        FUNCTION()
        static bool FractureEntity(CWorld* World, ECS::FEntity Entity, FVector3 Origin, float Strength = 0.0f);

        // Shatters a mesh at a transform with no entity to set up, for something that turns to rubble the moment it falls.
        FUNCTION()
        static bool ShatterMesh(CWorld* World, CStaticMesh* Mesh, FTransform Transform, FVector3 Origin, float Strength = 6.0f,
                                int32 Pieces = 12, float Lifetime = 8.0f);
    };
}
