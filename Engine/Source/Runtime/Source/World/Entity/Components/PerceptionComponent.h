#pragma once

#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "AI/Perception/PerceptionTypes.h"
#include "GameplayTags/GameplayTag.h"
#include "Physics/PhysicsTypes.h"
#include "World/Entity/Events/PerceptionEvent.h"
#include "PerceptionComponent.generated.h"

namespace Lumina
{
    // Gives an entity AI senses (sight, hearing, damage). SPerceptionSystem fills PerceivedTargets each tick;
    // OnTargetPerceived / OnTargetLost fire on transitions (C++ delegates + C# EntityScript callbacks).
    // Designed so a script can drive an AI with a few lines: GetClosestPerceivedTarget + GetLastKnownLocation.
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SPerceptionComponent
    {
        GENERATED_BODY()

        //~ Begin script-facing query API (read perceived state without a registry crossing).

        FUNCTION()
        int32 GetPerceivedTargetCount() const { return PerceivedCount; }

        /** Perceived entity at Index in [0, GetPerceivedTargetCount), or null. (C++; use World.Perception in C#.) */
        entt::entity GetPerceivedTarget(int32 Index) const
        {
            if (Index < 0 || Index >= PerceivedCount) return entt::null;
            return PerceivedTargets[Index].Target;
        }

        FUNCTION()
        bool HasPerceivedTarget(entt::entity Target) const
        {
            return Find(Target) >= 0;
        }

        /** True if Target is sensed THIS tick via the given sense channel. (C++; use World.Perception.CanSense in C#.) */
        bool CanCurrentlySense(entt::entity Target, EAISenseChannel Channel) const
        {
            const int32 Index = Find(Target);
            return Index >= 0 && (PerceivedTargets[Index].ActiveSenses & (uint8)Channel) != 0;
        }

        /** Last sensed world location of Target (held during the forget window), or origin if unknown. */
        FUNCTION()
        FVector3 GetLastKnownLocation(entt::entity Target) const
        {
            const int32 Index = Find(Target);
            return Index >= 0 ? PerceivedTargets[Index].LastKnownLocation : FVector3(0.0f);
        }

        /** Nearest perceived target to this perceiver's eye, or null when nothing is perceived.
            (C++; use World.Perception.GetClosestPerceivedTarget in C#.) */
        entt::entity GetClosestPerceivedTarget() const
        {
            entt::entity Best = entt::null;
            float BestSq = 3.4e38f;
            for (int32 i = 0; i < PerceivedCount; ++i)
            {
                const FVector3 D = PerceivedTargets[i].LastKnownLocation - LastEyeLocation;
                const float Sq = D.x * D.x + D.y * D.y + D.z * D.z;
                if (Sq < BestSq)
                {
                    BestSq = Sq;
                    Best = PerceivedTargets[i].Target;
                }
            }
            return Best;
        }

        //~ End script-facing query API.

        /** Only sources whose AffiliationTags match any of these are sensed. Empty = sense everyone. */
        PROPERTY(Editable, Category = "AI|Perception")
        FGameplayTagContainer DetectableTags;

        //~ Sight
        PROPERTY(Editable, Category = "AI|Perception|Sight")
        bool bSightEnabled = true;

        /** Distance at which a target is first seen (meters). */
        PROPERTY(Editable, Category = "AI|Perception|Sight", ClampMin = 0.0f)
        float SightRadius = 20.0f;

        /** Distance at which an already-seen target is dropped (>= SightRadius for hysteresis; meters). */
        PROPERTY(Editable, Category = "AI|Perception|Sight", ClampMin = 0.0f)
        float LoseSightRadius = 25.0f;

        /** Full vision cone angle (degrees). */
        PROPERTY(Editable, Category = "AI|Perception|Sight", ClampMin = 0.0f, ClampMax = 360.0f)
        float SightFOVDegrees = 90.0f;

        /** Eye offset (along entity up) used as the sight ray origin. */
        PROPERTY(Editable, Category = "AI|Perception|Sight")
        FVector3 EyeOffset = FVector3(0.0f, 1.6f, 0.0f);

        /** What blocks line of sight: a body blocks the ray when its collision Mask intersects this. */
        PROPERTY(Editable, Category = "AI|Perception|Sight")
        ECollisionProfiles SightBlockingMask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;

        //~ Hearing
        PROPERTY(Editable, Category = "AI|Perception|Hearing")
        bool bHearingEnabled = true;

        /** Radius within which reported noise is heard (meters; scaled by noise loudness). */
        PROPERTY(Editable, Category = "AI|Perception|Hearing", ClampMin = 0.0f)
        float HearingRadius = 15.0f;

        //~ Damage
        PROPERTY(Editable, Category = "AI|Perception|Damage")
        bool bDamageEnabled = true;

        //~ Memory / throttling
        /** Seconds a lost target is remembered (LastKnownLocation held) before OnTargetLost fires. */
        PROPERTY(Editable, Category = "AI|Perception", ClampMin = 0.0f)
        float ForgetTime = 5.0f;

        /** Minimum seconds between sight scans for this perceiver (load is spread across frames). */
        PROPERTY(Editable, Category = "AI|Perception", ClampMin = 0.0f)
        float UpdateInterval = 0.1f;

        /** Draw this perceiver's sight cone / ranges / target lines (also gated by the ai.Perception.Debug CVar). */
        PROPERTY(Editable, Category = "AI|Perception|Debug")
        bool bDrawDebug = false;

        PROPERTY()
        TScriptDelegate<SPerceptionEvent> OnTargetPerceived;

        // Location is the last known position.
        PROPERTY()
        TScriptDelegate<SPerceptionEvent> OnTargetLost;

        //~ Runtime state (NOT reflected): recomputed every tick, never serialized, must stay trivially copyable.
        static constexpr int32 MaxPerceivedTargets = 16;
        FPerceivedTarget    PerceivedTargets[MaxPerceivedTargets] = {};
        int32               PerceivedCount      = 0;
        float               UpdateAccumulator   = 0.0f;   // sight-scan throttle.
        uint32              UpdatePhase         = 0;       // per-perceiver phase so they don't all scan one frame.
        FVector3            LastEyeLocation     = FVector3(0.0f); // for GetClosestPerceivedTarget without the registry.

        /** Index of Target in PerceivedTargets, or -1. */
        int32 Find(entt::entity Target) const
        {
            for (int32 i = 0; i < PerceivedCount; ++i)
            {
                if (PerceivedTargets[i].Target == Target)
                {
                    return i;
                }
            }
            return -1;
        }
    };
}
