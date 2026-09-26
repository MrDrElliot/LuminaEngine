#pragma once

#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "RVOAgentComponent.generated.h"

namespace Lumina
{
    // Opt in to reciprocal local avoidance; the driver writes PreferredVelocity and the system solves it.
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SRVOAgentComponent
    {
        GENERATED_BODY()

        // World-space XZ velocity the agent would take with no one in the way, in meters per second.
        FUNCTION()
        void SetPreferredVelocity(const FVector3& Velocity) { PreferredVelocity = Velocity; }

        FUNCTION()
        FVector3 GetPreferredVelocity() const { return PreferredVelocity; }

        // The solved velocity from the most recent solve, which is what drives the controller.
        FUNCTION()
        FVector3 GetAvoidanceVelocity() const { return AvoidanceVelocity; }

        // How far the solved velocity was pushed off the preferred one, in meters per second.
        FUNCTION()
        float GetAvoidanceMagnitude() const { return Math::Length(AvoidanceVelocity - PreferredVelocity); }

        /** Radius used for avoidance. Zero falls back to the character capsule radius. */
        PROPERTY(Editable, Category = "Avoidance", ClampMin = 0.0f, Units = "m")
        float Radius = 0.0f;

        /** Seconds of lookahead. Larger values swerve earlier and read as more cautious. */
        PROPERTY(Editable, Category = "Avoidance", ClampMin = 0.1f, Units = "s")
        float TimeHorizon = 2.0f;

        /** How far to look for neighbors. Zero derives one from TimeHorizon and the agent's speed. */
        PROPERTY(Editable, Category = "Avoidance", ClampMin = 0.0f, Units = "m")
        float NeighborDistance = 0.0f;

        /** Neighbors considered per solve. More is smoother in a crush and costs more per agent. */
        PROPERTY(Editable, Category = "Avoidance", ClampMin = 1, ClampMax = 16)
        int32 MaxNeighbors = 8;

        // Fraction of MaxSpeed nudged off course, which is what stops symmetric standoffs deadlocking.
        PROPERTY(Editable, Category = "Avoidance", ClampMin = 0.0f, ClampMax = 0.1f)
        float SymmetryBreaking = 0.01f;

        /** Share of each mutual correction this agent takes. Below 0.5 makes others yield to it. */
        PROPERTY(Editable, Category = "Avoidance", ClampMin = 0.0f, ClampMax = 1.0f)
        float Responsibility = 0.5f;

        /** Speed cap for the solve. Zero falls back to the character's MoveSpeed. */
        PROPERTY(Editable, Category = "Avoidance", ClampMin = 0.0f, Units = "m/s")
        float MaxSpeed = 0.0f;

        // Others still avoid this agent. Use it for a player or a boss that should not be shoved.
        PROPERTY(Editable, Category = "Avoidance")
        bool bIgnoreNeighbors = false;

        // Costs one navmesh raycast per solve, so turn it on only where a squeeze can push agents off.
        PROPERTY(Editable, Category = "Avoidance")
        bool bClampToNavMesh = false;

        /** Write the solved velocity into the paired character controller each tick. */
        PROPERTY(Editable, Category = "Avoidance")
        bool bDriveCharacterController = true;

        /** Draw the preferred and solved velocities, and the neighbors that constrained the solve. */
        PROPERTY(Editable, Category = "Avoidance|Debug")
        bool bDrawDebug = false;

        /** Desired velocity for this tick, in meters per second. Y is ignored. */
        PROPERTY(ReadOnly, Category = "Avoidance", Units = "m/s")
        FVector3 PreferredVelocity = FVector3(0.0f);

        /** Solved velocity, held between solves when significance thins this agent's rate. */
        PROPERTY(ReadOnly, Category = "Avoidance", Units = "m/s")
        FVector3 AvoidanceVelocity = FVector3(0.0f);

        /** Neighbors that constrained the most recent solve. */
        PROPERTY(ReadOnly, Category = "Avoidance")
        int32 LastNeighborCount = 0;
    };
}
