#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "WorldSettings.generated.h"


namespace Lumina
{
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SDefaultWorldSettings
    {
        GENERATED_BODY()

        /** Engine systems disabled for this world, by reflected type name. Unknown names are ignored,
            so deleting a system never breaks load; new systems default to enabled. Driven by the
            World Editor's Systems panel, not the property grid. */
        PROPERTY()
        TVector<FName> DisabledSystems;

        /** Entities below this Y position are automatically destroyed. */
        PROPERTY(Editable)
        float WorldKillHeight = -5'000;

        /** Multiplier applied to global gravity strength. */
        PROPERTY(Editable)
        float GravityScale = 1.0f;

        /** Time dilation factor - values below 1 slow the world, above 1 speed it up. */
        PROPERTY(Editable)
        float DeltaTimeScale = 1.0f;

        /** Normalized direction of gravity in world space. */
        PROPERTY(Editable, Category = "Physics")
        FVector3 GravityDirection = FVector3(0.0f, -1.0f, 0.0f);

        /** Fixed physics update rate in Hz. Higher = more accurate but more CPU. */
        PROPERTY(Editable, Category = "Physics")
        float PhysicsHz = 60.0f;

        /** Maximum fixed-step iterations per frame to prevent spiral-of-death under load. */
        PROPERTY(Editable, Category = "Physics")
        uint8 MaxPhysicsSteps = 8;

        /** Interpolate rigid body positions between fixed steps for smoother visuals.
        Disable for debugging or when PhysicsHz matches render rate. */
        PROPERTY(Editable, Category = "Physics")
        bool bEnablePhysicsInterpolation = true;

        /** Max rigid bodies the scene pre-allocates for. Drives up-front physics memory; exceeding it at runtime drops new bodies, so raise it for dense worlds. */
        PROPERTY(Editable, Category = "Physics")
        uint32 MaxPhysicsBodies = 65536;

        /** Contact constraints the solver pre-allocates. Raise it if dense contact piles start interpenetrating. */
        PROPERTY(Editable, Category = "Physics")
        uint32 MaxPhysicsContactConstraints = 131072;

        /** Solver sub-steps per fixed step. Higher is stiffer and more stable, and costs proportionally more. */
        PROPERTY(Editable, ClampMin = 1, ClampMax = 16, Category = "Physics")
        uint32 SolverSubStepCount = 4;

        /** Contact stiffness in cycles per second. Higher recovers overlap faster but can jitter. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float ContactHertz = 30.0f;

        /** Contact bounciness while resolving overlap. Lower recovers faster and more energetically. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float ContactDampingRatio = 10.0f;

        /** Cap on how fast overlap is pushed apart (m/s). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics", Units = "m/s")
        float ContactPushSpeed = 3.0f;

        /** Collisions slower than this (m/s) do not bounce, which stops resting bodies buzzing. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics", Units = "m/s")
        float RestitutionThreshold = 1.0f;

        /** Impact speed (m/s) above which a shape with hit events enabled reports one. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics", Units = "m/s")
        float HitEventThreshold = 1.0f;

        /** Hard ceiling on body speed (m/s). */
        PROPERTY(Editable, ClampMin = 0.001f, Category = "Physics", Units = "m/s")
        float MaxLinearSpeed = 500.0f;

        /** Speed (m/s) below which a body may fall asleep. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics", Units = "m/s")
        float SleepVelocityThreshold = 0.05f;

        /** Re-apply the previous step's impulses as a solver starting point. Improves convergence. */
        PROPERTY(Editable, Category = "Physics")
        bool bConstraintWarmStart = true;

        /** Allow bodies to go to sleep globally. Overrides per-body sleep settings when disabled. */
        PROPERTY(Editable, Category = "Physics")
        bool bAllowSleeping = true;

        /** Continuous collision for fast movers. Off trades tunnelling for a cheaper step. */
        PROPERTY(Editable, Category = "Physics")
        bool bEnableContinuousCollision = true;

        /** Speculative contacts between hulls and triangles. Off reduces ghost collisions on seams. */
        PROPERTY(Editable, Category = "Physics")
        bool bEnableSpeculativeContacts = true;

        //~ Networking: server-authoritative replication tuning for this world. (Client-side proxy smoothing is
        //  a global player preference -- see CNetworkSettings.)

        /** Seconds between full transform keyframes (server re-sends every replicated pose so a dropped delta
         *  self-heals). <= 0 disables keyframes. */
        PROPERTY(Editable, Category = "Networking", ClampMin = 0.0f)
        float TransformKeyframeInterval = 0.5f;

        /** Default movement send rate (Hz) for newly replicated entities. */
        PROPERTY(Editable, Category = "Networking", ClampMin = 0.0f)
        float DefaultNetUpdateFrequency = 30.0f;

        //~ Interest management (per-client relevancy). Distances are on the XZ ground plane, in meters.

        /** An entity becomes relevant to a client when it crosses inside this radius of the client's pawn. */
        PROPERTY(Editable, Category = "Networking|Interest", ClampMin = 1.0f)
        float AOIEnterRadius = 120.0f;

        /** A relevant entity stays relevant until it crosses outside this (larger) radius. Hysteresis to stop
         *  spawn/despawn thrash at the boundary. Should be >= AOIEnterRadius. */
        PROPERTY(Editable, Category = "Networking|Interest", ClampMin = 1.0f)
        float AOILeaveRadius = 150.0f;

        /** After an entity leaves the AOI, wait this long before despawning it on the client (absorbs fast
         *  boundary crossings; the copy just goes stale meanwhile). */
        PROPERTY(Editable, Category = "Networking|Interest", ClampMin = 0.0f)
        float RelevancyGraceSeconds = 1.5f;

        /** Spatial grid cell size (meters) for the relevancy broadphase. ~AOI radius is a good default so a
         *  client gathers ~4-9 cells. */
        PROPERTY(Editable, Category = "Networking|Interest", ClampMin = 1.0f)
        float GridCellSize = 64.0f;

        /** Half-extent (meters) of the replicated world on the XZ plane, centered at the origin. Entities
         *  outside clamp into the border cells. Sets the grid dimensions. */
        PROPERTY(Editable, Category = "Networking|Interest", ClampMin = 64.0f)
        float WorldHalfExtent = 8192.0f;

        //~ Distance LOD tiers. Tier boundaries on the XZ plane, in meters; send rates in Hz.

        /** Max distance for Tier 0 (near): full rate + full precision. */
        PROPERTY(Editable, Category = "Networking|LOD", ClampMin = 0.0f)
        float TierNearDistance = 30.0f;

        /** Max distance for Tier 1 (mid). Beyond this up to AOILeaveRadius is Tier 2 (far). */
        PROPERTY(Editable, Category = "Networking|LOD", ClampMin = 0.0f)
        float TierMidDistance = 80.0f;

        /** Send rate (Hz) for Tier 1 (mid) entities. */
        PROPERTY(Editable, Category = "Networking|LOD", ClampMin = 0.0f)
        float TierMidRate = 10.0f;

        /** Send rate (Hz) for Tier 2 (far) entities. */
        PROPERTY(Editable, Category = "Networking|LOD", ClampMin = 0.0f)
        float TierFarRate = 3.0f;
    };
}
