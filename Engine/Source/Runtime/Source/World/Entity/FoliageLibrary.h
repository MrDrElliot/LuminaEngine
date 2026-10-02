#pragma once

#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "FoliageLibrary.generated.h"

namespace Lumina
{
    class CWorld;
    class CStaticMesh;

    // Script access to the world's instanced foliage, with every edit reaching the render scene.
    REFLECT()
    class RUNTIME_API CFoliageLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // The world's single foliage entity, created on first use.
        FUNCTION()
        static ECS::FEntity GetOrCreateFoliage(CWorld* World);

        // Registers a mesh as a foliage type and returns the index instances refer to it by.
        FUNCTION()
        static int32 AddFoliageType(CWorld* World, CStaticMesh* Mesh, bool bCastShadow = true, float CullDistance = 0.0f);

        FUNCTION()
        static void AddFoliageInstances(CWorld* World, const SFoliageInstance* Instances, int32 Count);

        // Removes instances of a type within Radius on the ground plane, or of every type when TypeFilter is negative.
        FUNCTION()
        static int32 RemoveFoliageInRadius(CWorld* World, FVector3 Center, float Radius, int32 TypeFilter = -1);

        FUNCTION()
        static int32 GetFoliageInstanceCount(CWorld* World);

        // Hides or shows one instance without rebaking the field, for a felled tree or an opened door.
        FUNCTION()
        static void SetFoliageInstanceHidden(CWorld* World, int32 Index, bool bHidden);
    };
}
