#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "WorldSettings.generated.h"


namespace Lumina
{
    REFLECT()
    enum class ESMAAQuality : uint8
    {
        Off,
        Low,
        Medium,
        High,
        Ultra,
    };

    REFLECT()
    enum class EVariableRateShading : uint8
    {
        Off,        // 1x1 - full rate
        Rate2x2,    // quarter the fragment shader invocations
        Rate4x4,    // sixteenth (clamped to the GPU's max supported rate)
    };

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

        /** Antialiasing quality. Off disables SMAA; higher qualities detect more edges at higher GPU cost. */
        PROPERTY(Editable, Category = "Rendering")
        ESMAAQuality SMAAQuality = ESMAAQuality::High;

        // VRS rate for opted-in passes (sky, particles, translucency, fog, opaque base). Coarser = fewer PS
        // invocations but softer + reduced picker precision (base pass writes it). No-op without pipeline FSR.
        PROPERTY(Editable, Category = "Rendering")
        EVariableRateShading VariableRateShading = EVariableRateShading::Off;

        /** Screen-space ambient occlusion. Reconstructs occlusion from depth and darkens the ambient/IBL
            term -- only visible where there is some skylight/environment ambient. */
        PROPERTY(Editable, Category = "Rendering")
        bool bEnableGTAO = false;

        /** GTAO sample radius in world units. Larger = wider, softer occlusion. */
        PROPERTY(Editable, ClampMin = 0.01f, Category = "Rendering")
        float GTAORadius = 0.5f;

        /** GTAO strength multiplier. 0 = none. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Rendering")
        float GTAOIntensity = 1.0f;

        /** GTAO contrast exponent applied to the AO factor. Higher = darker, tighter contact shadows. */
        PROPERTY(Editable, ClampMin = 0.1f, Category = "Rendering")
        float GTAOPower = 2.0f;

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

        /** When on, the simulation is deterministic at the cost of speed (forces contact/island
         sorting and reduces solver parallelism). Enable only for lockstep netcode or replays. */
        PROPERTY(Editable, Category = "Physics")
        bool bDeterministicSimulation = false;

        /** Max rigid bodies the scene pre-allocates for. Drives up-front physics memory; exceeding it at runtime drops new bodies, so raise it for dense worlds. */
        PROPERTY(Editable, Category = "Physics")
        uint32 MaxPhysicsBodies = 65536;

        /** Max simultaneously-overlapping body pairs the broad phase tracks. Dense fracture piles overflow this; overflow trips a Jolt update error. */
        PROPERTY(Editable, Category = "Physics")
        uint32 MaxPhysicsBodyPairs = 98304;

        /** Max contact constraints the solver pre-allocates -- the dominant per-scene physics memory cost. Raise it if dense contact piles start interpenetrating. */
        PROPERTY(Editable, Category = "Physics")
        uint32 MaxPhysicsContactConstraints = 131072;

        /** Global velocity solver iterations. Entities can raise this per-body via NumVelocityStepsOverride. Minimum 2 required for friction. */
        PROPERTY(Editable, Category = "Physics")
        uint32 NumVelocitySteps = 10;

        /** Global position solver iterations. Higher = less penetration overlap. */
        PROPERTY(Editable, Category = "Physics")
        uint32 NumPositionSteps = 2;

        /** Fraction of penetration depth corrected per step (0 = nothing, 1 = all at once). */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, Category = "Physics")
        float BaumgarteStabilizationFactor = 0.2f;

        /** Radius around objects (meters) in which speculative contact points are detected. Too large causes ghost collisions. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float SpeculativeContactDistance = 0.02f;

        /** How far bodies are allowed to sink into each other (meters). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float PenetrationSlop = 0.02f;

        /** Maximum distance to correct in a single position iteration (meters). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float MaxPenetrationDistance = 0.2f;

        /** For LinearCast motion quality: fraction of inner radius a body must move per step to enable casting. */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, Category = "Physics")
        float LinearCastThreshold = 0.75f;

        /** For LinearCast motion quality: fraction of inner radius a body may penetrate another. */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, Category = "Physics")
        float LinearCastMaxPenetration = 0.25f;

        /** Max distance (meters) to determine if two points lie on the same contact manifold plane. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float ManifoldTolerance = 1.0e-3f;

        /** Minimum relative velocity (m/s) for a collision to produce restitution (bounce). Below this, restitution is forced to zero so objects settle. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float MinVelocityForRestitution = 1.0f;

        /** Seconds a body must remain nearly still before being allowed to sleep. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float TimeBeforeSleep = 0.5f;

        /** Point velocity (m/s) below which a body is considered still for sleep purposes. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float SleepVelocityThreshold = 0.03f;

        /** Max relative delta position (m^2) between frames for body-pair contact cache to be reused. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float BodyPairCacheMaxDeltaPositionSq = 1.0e-6f;

        /** Max relative delta rotation (as cos(angle/2)) for body-pair cache reuse. Default = cos(1 deg). */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, Category = "Physics")
        float BodyPairCacheCosMaxDeltaRotationDiv2 = 0.99984769515639123915701155881391f;

        /** Max angle between contact normals (as cosine) to merge sub-shape manifolds. Default = cos(5 deg). */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, Category = "Physics")
        float ContactNormalCosMaxDeltaRotation = 0.99619469809174553229501040247389f;

        /** Max distance (m^2) between old and new contact point to preserve warm-start impulses. Default = 1 cm^2. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Physics")
        float ContactPointPreserveLambdaMaxDistSq = 1.0e-4f;

        /** Max body pairs in-flight in the broadphase at once. Lower saves memory, may reduce parallelism. */
        PROPERTY(Editable, Category = "Physics")
        int32 MaxInFlightBodyPairs = 16384;

        /** How many step listeners to notify in one batch. */
        PROPERTY(Editable, Category = "Physics")
        int32 StepListenersBatchSize = 8;

        /** How many listener batches are needed before spawning another job (INT_MAX = no parallelism). */
        PROPERTY(Editable, Category = "Physics")
        int32 StepListenerBatchesPerJob = 1;

        /** Re-apply previous frame's constraint impulses as a solver starting point. Improves convergence. */
        PROPERTY(Editable, Category = "Physics")
        bool bConstraintWarmStart = true;

        /** Cache narrow-phase results when bodies haven't moved relative to each other. */
        PROPERTY(Editable, Category = "Physics")
        bool bUseBodyPairContactCache = true;

        /** Merge contact manifolds with similar normals into one. Reduces solver work. */
        PROPERTY(Editable, Category = "Physics")
        bool bUseManifoldReduction = true;

        /** Split large connected islands into smaller parallel work batches. */
        PROPERTY(Editable, Category = "Physics")
        bool bUseLargeIslandSplitter = true;

        /** Allow bodies to go to sleep globally. Overrides per-body sleep settings when disabled. */
        PROPERTY(Editable, Category = "Physics")
        bool bAllowSleeping = true;

        /** Prevent collision against non-active shared edges on triangle meshes. */
        PROPERTY(Editable, Category = "Physics")
        bool bCheckActiveEdges = true;

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
