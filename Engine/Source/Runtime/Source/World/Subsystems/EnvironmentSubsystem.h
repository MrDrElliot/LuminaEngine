#pragma once

#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "EnvironmentSubsystem.generated.h"

namespace Lumina
{
    class CStaticMesh;
    struct SFoliageInstance;

    // Terrain and foliage.
    REFLECT()
    class RUNTIME_API CEnvironmentSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        FUNCTION()
        bool IsOnTerrain(float X, float Z);

        // The terrain surface height under (X, Z), or Fallback when no terrain covers the point.
        FUNCTION()
        float GetTerrainHeight(float X, float Z, float Fallback = 0.0f);

        // The terrain surface normal under (X, Z), or world up when no terrain covers the point.
        FUNCTION()
        FVector3 GetTerrainNormal(float X, float Z);

        // The world's single foliage entity, created on first use.
        FUNCTION()
        ECS::FEntity GetOrCreateFoliage();

        // Registers a mesh as a foliage type and returns the index instances refer to it by.
        FUNCTION()
        int32 AddFoliageType(CStaticMesh* Mesh, bool bCastShadow = true, float CullDistance = 0.0f);

        FUNCTION()
        void AddFoliageInstances(const SFoliageInstance* Instances, int32 Count);

        // Removes instances of a type within Radius on the ground plane, or of every type when TypeFilter is negative.
        FUNCTION()
        int32 RemoveFoliageInRadius(FVector3 Center, float Radius, int32 TypeFilter = -1);

        FUNCTION()
        int32 GetFoliageInstanceCount();

        // Hides or shows one instance without rebaking the field, for a felled tree or an opened door.
        FUNCTION()
        void SetFoliageInstanceHidden(int32 Index, bool bHidden);
    };
}
