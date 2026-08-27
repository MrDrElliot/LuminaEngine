#pragma once

#include "World/ECS/Registry.h"



#include "Platform/GenericPlatform.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Physics/PhysicsTypes.h"
#include "RayCast.generated.h"

namespace Lumina
{
    // Inline capacity for a query's ignore list; going past it spills to the heap, never drops bodies.
    inline constexpr size_t MaxInlineIgnoreBodies = 8;

    inline constexpr ECollisionProfiles AllCollisionProfiles = (ECollisionProfiles)0xFFFF;

    REFLECT()
    struct SRayResult
    {
        GENERATED_BODY()

        PROPERTY()
        int64 BodyID;

        PROPERTY()
        uint32 Entity = ECS::NullEntity.Value;

        PROPERTY()
        FVector3 Start;

        PROPERTY()
        FVector3 End;

        PROPERTY()
        FVector3 Location;

        PROPERTY()
        FVector3 Normal;

        /** Normalized distance along ray (0 = start, 1 = end). */
        PROPERTY()
        float Fraction;

        PROPERTY()
        float Distance;

        /** Skeleton bone the hit body belongs to (ragdoll per-bone bodies); INDEX_NONE otherwise. */
        PROPERTY()
        int32 BoneIndex = INDEX_NONE;
    };

    REFLECT()
    struct SRayCastSettings
    {
        GENERATED_BODY()

        PROPERTY()
        FVector3 Start = FVector3(0.0f);

        PROPERTY()
        FVector3 End = FVector3(0.0f);

        PROPERTY()
        bool bDrawDebug = false;

        /** Seconds; 0 = one frame. */
        PROPERTY()
        float DebugDuration = 0.0f;
        
        PROPERTY()
        bool bIgnoreSelf = false;

        PROPERTY()
        FVector3 DebugHitColor = FVector3(0.0, 1.0f, 0.0f);

        PROPERTY()
        FVector3 DebugMissColor = FVector3(1.0f, 0.0f, 0.0f);

        PROPERTY()
        ECollisionProfiles LayerMask = AllCollisionProfiles;

        PROPERTY()
        TFixedVector<uint32, MaxInlineIgnoreBodies> IgnoreBodies;

        FUNCTION()
        void AddIgnoredBody(uint32 Body)
        {
            IgnoreBodies.push_back(Body);
        }
    };

    REFLECT()
    struct SSphereCastSettings
    {
        GENERATED_BODY()

        PROPERTY()
        FVector3 Start = FVector3(0.0f);

        PROPERTY()
        FVector3 End = FVector3(0.0f);

        /** Sphere radius (meters). */
        PROPERTY()
        float Radius = 0.0f;

        PROPERTY()
        bool bDrawDebug = false;

        /** Seconds; 0 = one frame. */
        PROPERTY()
        float DebugDuration = 0.0f;

        PROPERTY()
        FVector3 DebugHitColor = FVector3(0.0, 1.0f, 0.0f);

        PROPERTY()
        FVector3 DebugMissColor = FVector3(1.0f, 0.0f, 0.0f);

        PROPERTY()
        ECollisionProfiles LayerMask = AllCollisionProfiles;

        PROPERTY()
        TFixedVector<uint32, MaxInlineIgnoreBodies> IgnoreBodies;

        FUNCTION()
        void AddIgnoredBody(uint32 Body)
        {
            IgnoreBodies.push_back(Body);
        }
    };
}
