#pragma once

#include "World/ECS/Registry.h"


#include "AI/Navigation/NavTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "PathFollowComponent.generated.h"

namespace Lumina
{
    // Result of the most recent path query; distinct from "is the agent moving" -- a follower may keep
    // walking prior corners after a failed replan.
    enum class EPathFollowStatus : uint8
    {
        None      = 0, // No target set, or target was just cleared.
        Searching = 1, // Target set; first query has not yet been issued (or just dirtied).
        Following = 2, // Last query succeeded; corners are fresh.
        Reached   = 3, // Agent has consumed the corner list.
        Failed    = 4, // Last query returned no valid path.
    };

    // Drives an entity along a navmesh path; SPathFollowSystem fills the paired controller's MoveInput from
    // the direction to the next corner. Target via SetTargetLocation/SetTargetEntity; replans past RepathDistance.
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SPathFollowComponent
    {
        GENERATED_BODY()

        //~ Begin script-facing API. Designed so a script can drive an AI with
        //  three lines: SetTargetLocation, IsFollowing, IsAtDestination.

        /** Set a static world-space goal. Triggers a fresh path request next tick. */
        FUNCTION()
        void SetTargetLocation(const FVector3& World)
        {
            TargetLocation = World;
            TargetEntity   = ECS::NullEntity;
            bHasTarget     = true;
            bPathDirty     = true;
            Status         = EPathFollowStatus::Searching;
            ConsecutiveFailures = 0;
            LastPathResult = ENavPathResult::NotQueried;
            ResetStuck();
        }

        /** Track an entity. The system re-projects the entity's current location each tick. */
        FUNCTION()
        void SetTargetEntity(ECS::FEntity Entity)
        {
            TargetEntity = Entity;
            bHasTarget   = (Entity != ECS::NullEntity);
            bPathDirty   = true;
            Status       = bHasTarget ? EPathFollowStatus::Searching : EPathFollowStatus::None;
            ConsecutiveFailures = 0;
            LastPathResult = ENavPathResult::NotQueried;
            ResetStuck();
        }

        /** Clear the goal and any cached path. */
        FUNCTION()
        void Stop()
        {
            bHasTarget = false;
            TargetEntity = ECS::NullEntity;
            CornerCount = 0;
            CurrentCorner = 0;
            bPathDirty = false;
            bPathPartial = false;
            bPathTruncated = false;
            PathEpoch = 0;
            Status = EPathFollowStatus::None;
            ConsecutiveFailures = 0;
            LastPathResult = ENavPathResult::NotQueried;
            ResetStuck();
        }

        void ResetStuck()
        {
            StuckTime = 0.0f;
            SidestepTime = 0.0f;
            StuckAttempts = 0;
        }

        FUNCTION()
        bool IsFollowing() const { return bHasTarget && CornerCount > 0; }

        /** True only once the agent consumed a path that actually ended at the target. */
        FUNCTION()
        bool IsAtDestination() const { return bHasTarget && CornerCount > 0 && CurrentCorner >= CornerCount && !bPathPartial && !bPathTruncated; }

        /** True when the cached path stops short of the target, whether unreachable or cut by a buffer limit. */
        FUNCTION()
        bool IsPathPartial() const { return bPathPartial || bPathTruncated; }

        /** True if the most recent path query failed. Stays true until a subsequent query succeeds or the target is cleared. */
        FUNCTION()
        bool DidPathFindingFail() const { return Status == EPathFollowStatus::Failed; }

        // Something the navmesh does not know about kept the agent from moving, and sidestepping did not free it.
        FUNCTION()
        bool IsStuck() const { return StuckAttempts >= MaxStuckAttempts; }

        // Sidesteps tried since the agent last made headway.
        FUNCTION()
        int32 GetStuckAttempts() const { return StuckAttempts; }

        /** Number of consecutive failed queries since the last success. Useful for script-side give-up logic. */
        FUNCTION()
        int32 GetConsecutivePathFailures() const { return ConsecutiveFailures; }

        // Why the most recent query ended as it did, so a stalled agent can say what stopped it.
        FUNCTION()
        ENavPathResult GetLastPathResult() const { return LastPathResult; }

        // The same outcome as text, ready to drop into a log line.
        FUNCTION()
        FString DescribeLastPathResult() const { return FString(ToString(LastPathResult)); }

        // True when the next corner is a link hop that gameplay drives rather than a walk.
        FUNCTION()
        bool IsEnteringOffMeshLink() const
        {
            if (CornerCount == 0 || CurrentCorner >= CornerCount) return false;
            return (PathCornerFlags[CurrentCorner] & (uint8)ENavCornerFlag::OffMeshLink) != 0;
        }

        /** Closest queued path corner, or the target if no path is cached. */
        FUNCTION()
        FVector3 GetNextCorner() const
        {
            if (CornerCount == 0 || CurrentCorner >= CornerCount) return TargetLocation;
            return PathCorners[CurrentCorner];
        }

        //~ End script-facing API.

        /** Distance below which the agent considers a corner "reached" and advances. */
        PROPERTY(Editable, Category = "PathFollow", ClampMin = 0.05f)
        float AcceptanceRadius = 0.5f;

        /** Throttle (0..1) applied to the controller's MoveSpeed; 1 = full speed.
         *  Lower it for a walk; the movement system supplies the absolute m/s. */
        PROPERTY(Editable, Category = "PathFollow", ClampMin = 0.0f, ClampMax = 1.0f)
        float Speed = 1.0f;

        // Replan when the tracked target moves more than this from the cached path's source point. Ignored
        // for a static world-location goal.
        PROPERTY(Editable, Category = "PathFollow", ClampMin = 0.0f)
        float RepathDistance = 1.5f;

        // How long a walking agent may make almost no headway before it repaths and sidesteps.
        PROPERTY(Editable, Category = "PathFollow|Stuck", ClampMin = 0.0f, Units = "s")
        float StuckTimeout = 0.8f;

        /** Hard repath interval as a backstop, in seconds. */
        PROPERTY(Editable, Category = "PathFollow", ClampMin = 0.0f)
        float RepathInterval = 1.0f;

        /** When true the system writes movement input each tick. Can be toggled by gameplay. */
        PROPERTY(Editable, Category = "PathFollow")
        bool bDriveCharacterController = true;

        // The navmesh volume whose Agent matches, left empty for the infantry default.
        PROPERTY(Editable, Category = "PathFollow")
        FName NavAgent;

        // Within this of the goal a driven vehicle eases off, so it stops at the goal instead of sailing past it.
        PROPERTY(Editable, Category = "PathFollow|Vehicle", ClampMin = 0.0f, Units = "m")
        float VehicleBrakingDistance = 12.0f;

        /** When true, the system emits debug lines along the cached path each tick. */
        PROPERTY(Editable, Category = "PathFollow|Debug")
        bool bDrawDebugPath = false;

        /** Cached path corners filled by the system. Capped to a fixed array to avoid per-tick heap churn. */
        // Queries are asked for exactly this many, so a longer route comes back flagged truncated.
        static constexpr int32 MaxCorners = 64;
        FVector3   PathCorners[MaxCorners] = {};

        // ENavCornerFlag per stored corner, parallel to PathCorners.
        uint8       PathCornerFlags[MaxCorners] = {};
        int32       CornerCount   = 0;
        int32       CurrentCorner = 0;

        /** World location of the active goal (latched from entity if tracking one). */
        FVector3   TargetLocation = FVector3(0.0f);
        ECS::FEntity TargetEntity = ECS::NullEntity;
        FVector3   PathSourceTarget = FVector3(0.0f); // target location at the moment the cached path was generated
        float       TimeSinceLastPath = 0.0f;

        /** Navmesh topology epoch the stored corners were found against. Tiles rebuilt or streamed out
         *  from under an agent leave the corners running through geometry that is no longer walkable. */
        uint64      PathEpoch = 0;

        bool        bHasTarget = false;
        bool        bPathDirty = false;

        /** The query reported the goal as unreachable; the corners stop at the nearest point it could reach. */
        bool        bPathPartial = false;

        /** The route was longer than PathCorners; the system repaths on arrival at the last stored corner. */
        bool        bPathTruncated = false;

        /** Latched outcome of the most recent path query. Updated by SPathFollowSystem. */
        EPathFollowStatus Status = EPathFollowStatus::None;

        /** Resets to 0 on a successful query, increments on every failed query. */
        int32 ConsecutiveFailures = 0;

        // Latched reason from the most recent path query, whether it succeeded or not.
        ENavPathResult LastPathResult = ENavPathResult::NotQueried;

        static constexpr int32 MaxStuckAttempts = 3;
        float StuckTime = 0.0f;
        FVector3 StuckAnchor = FVector3(0.0f);
        float SidestepTime = 0.0f;
        int32 StuckAttempts = 0;

        float VehicleStuckTime = 0.0f;
        float VehicleReverseTime = 0.0f;

        // Set while this follower is steering a vehicle, so stopping hands the controls back released rather than held.
        bool bDrivingVehicle = false;
    };
}
