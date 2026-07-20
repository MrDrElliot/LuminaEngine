#pragma once

#include "Platform/GenericPlatform.h"
#include "Core/Object/ObjectMacros.h"
#include "Physics/PhysicsTypes.h"
#include "RayCast.generated.h"

namespace Lumina
{
    REFLECT()
    struct SRayResult
    {
        GENERATED_BODY()

        PROPERTY(Script)
        int64 BodyID;

        PROPERTY(Script)
        uint32 Entity = entt::null;

        PROPERTY(Script)
        FVector3 Start;

        PROPERTY(Script)
        FVector3 End;

        PROPERTY(Script)
        FVector3 Location;

        PROPERTY(Script)
        FVector3 Normal;

        /** Normalized distance along ray (0 = start, 1 = end). */
        PROPERTY(Script)
        float Fraction;

        PROPERTY(Script)
        float Distance;

        /** Skeleton bone the hit body belongs to (ragdoll per-bone bodies); INDEX_NONE otherwise. */
        PROPERTY(Script)
        int32 BoneIndex = INDEX_NONE;
    };

    REFLECT()
    struct SRayCastSettings
    {
        GENERATED_BODY()

        PROPERTY(Script)
        FVector3 Start = FVector3(0.0f);

        PROPERTY(Script)
        FVector3 End = FVector3(0.0f);

        PROPERTY(Script)
        bool bDrawDebug = false;

        /** Seconds; 0 = one frame. */
        PROPERTY(Script)
        float DebugDuration = 0.0f;
        
        PROPERTY(Script)
        bool bIgnoreSelf = false;

        PROPERTY(Script)
        FVector3 DebugHitColor = FVector3(0.0, 1.0f, 0.0f);

        PROPERTY(Script)
        FVector3 DebugMissColor = FVector3(1.0f, 0.0f, 0.0f);

        PROPERTY(Script)
        ECollisionProfiles LayerMask;

        PROPERTY(Script)
        TVector<uint32> IgnoreBodies;
        
        FUNCTION(Script)
        void AddIgnoredBody(uint32 Body)
        {
            IgnoreBodies.push_back(Body);
        }
    };

    REFLECT()
    struct SSphereCastSettings
    {
        GENERATED_BODY()

        PROPERTY(Script)
        FVector3 Start = FVector3(0.0f);

        PROPERTY(Script)
        FVector3 End = FVector3(0.0f);

        /** Sphere radius (meters). */
        PROPERTY(Script)
        float Radius = 0.0f;

        PROPERTY(Script)
        bool bDrawDebug = false;

        /** Seconds; 0 = one frame. */
        PROPERTY(Script)
        float DebugDuration = 0.0f;

        PROPERTY(Script)
        FVector3 DebugHitColor = FVector3(0.0, 1.0f, 0.0f);

        PROPERTY(Script)
        FVector3 DebugMissColor = FVector3(1.0f, 0.0f, 0.0f);

        PROPERTY(Script)
        ECollisionProfiles LayerMask;

        PROPERTY(Script)
        TVector<uint32> IgnoreBodies;
        
        FUNCTION(Script)
        void AddIgnoredBody(uint32 Body)
        {
            IgnoreBodies.push_back(Body);
        }
    };
}
