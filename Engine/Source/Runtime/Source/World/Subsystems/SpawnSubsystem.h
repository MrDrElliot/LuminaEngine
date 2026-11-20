#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Math/Transform.h"
#include "Scripting/ScriptCallback.h"
#include "World/ECS/Entity.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "SpawnSubsystem.generated.h"

namespace Lumina
{
    class CPrefab;
    class CParticleSystem;
    class CMaterialInterface;
    class CStaticMesh;

    // Spawning prefabs, particles, decals, projectiles and debris, and building meshes at runtime.
    REFLECT()
    class RUNTIME_API CSpawnSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        /** Instantiates at the origin, unparented. */
        FUNCTION()
        ECS::FEntity SpawnPrefab(const TSoftObjectPtr<CPrefab>& Prefab);

        /** Positions the spawned root at SpawnTransform, under Parent when it is not the null entity. */
        FUNCTION()
        ECS::FEntity SpawnPrefabAt(const TSoftObjectPtr<CPrefab>& Prefab,
            const FTransform& SpawnTransform, ECS::FEntity Parent);

        // A prefab the caller already holds, at the origin and unparented, with no path lookup or load.
        FUNCTION()
        ECS::FEntity SpawnLoadedPrefab(CPrefab* Prefab);

        // A prefab the caller already holds, at SpawnTransform and under Parent when it is not the null entity.
        FUNCTION()
        ECS::FEntity SpawnLoadedPrefabAt(CPrefab* Prefab, const FTransform& SpawnTransform, ECS::FEntity Parent);

        /** Loads the prefab off the game thread, then spawns and reports it once, back on the game thread. */
        FUNCTION()
        void SpawnPrefabAsync(const TSoftObjectPtr<CPrefab>& Prefab, SScriptCallback OnSpawned);

        // Bursts at SpawnTransform and despawns after Lifetime seconds, where zero leaves it to the caller and negative means the one-shot length.
        FUNCTION()
        ECS::FEntity SpawnParticleSystem(CParticleSystem* ParticleSystem,
            const FTransform& SpawnTransform, float Lifetime);

        /** The same, parented to Parent on a named socket or bone, where none means the entity origin. */
        FUNCTION()
        ECS::FEntity SpawnParticleSystemAttached(CParticleSystem* ParticleSystem,
            ECS::FEntity Parent, const FName& Socket, FVector3 Offset, float Lifetime);

        // Where the particles of emitters flagged bReportCollisions hit, a few frames ago. A negative index takes every emitter.
        FUNCTION()
        void GetParticleCollisions(ECS::FEntity Entity, int32 EmitterIndex, TVector<SParticleCollision>& OutCollisions);

        // Size.z is the depth the box reaches either side of the surface, and a positive Lifetime fades then destroys it.
        FUNCTION()
        ECS::FEntity SpawnDecal(CMaterialInterface* Material, FVector3 Location, FVector3 Normal, FVector3 Size,
                                       float RotationDegrees = 0.0f, float Lifetime = 0.0f, float FadeOutDuration = 1.0f,
                                       int32 AtlasCell = 0, int32 AtlasColumns = 1, int32 AtlasRows = 1);

        /** A Lifetime above zero despawns the projectile through the engine lifetime system. */
        FUNCTION()
        ECS::FEntity SpawnProjectile(FVector3 Position, FVector3 Velocity, float Damage = 0.0f,
            float Lifetime = 5.0f, ECS::FEntity Instigator = ECS::NullEntity);

        // Shatters a mesh at a transform with no entity to set up, for something that turns to rubble the moment it falls.
        FUNCTION()
        bool ShatterMesh(CStaticMesh* Mesh, FTransform Transform, FVector3 Origin, float Strength = 6.0f,
                                int32 Pieces = 12, float Lifetime = 8.0f);

        // Script geometry as a real mesh, so it can be instanced, used as foliage or fractured. The world keeps it alive.
        FUNCTION()
        CStaticMesh* CreateStaticMesh(const float* Positions, int32 PositionFloats,
            const float* Normals, int32 NormalFloats, const float* UVs, int32 UVFloats,
            const float* Colors, int32 ColorFloats, const uint32* Indices, int32 IndexCount,
            CMaterialInterface* Material, bool bLODHierarchy = true);
    };
}
