using Lumina;

namespace LuminaSharp;

/// Opt-in local avoidance over SRVOAgentComponent. An agent with one has its path-follow velocity filtered so it steers around other agents instead of walking through them, which the navmesh route alone does not do.
public static class Avoidance
{
    /// Turns on avoidance for the agent, adding the component if it has none. Safe to call on an agent that already has it.
    public static SRVOAgentComponent Enable(CWorld World, Entity Agent)
    {
        return World.Registry.Emplace<SRVOAgentComponent>(Agent)!;
    }

    /// Same, with the two knobs worth setting per agent type. Radius of zero tracks the character capsule.
    public static SRVOAgentComponent Enable(CWorld World, Entity Agent, float Radius, float TimeHorizon)
    {
        SRVOAgentComponent Avoid = World.Registry.Emplace<SRVOAgentComponent>(Agent)!;
        Avoid.Radius = Radius;
        Avoid.TimeHorizon = TimeHorizon;
        return Avoid;
    }

    /// Stops the agent avoiding anyone. Others still avoid it, so a player or a boss keeps its ground.
    public static void SetIgnoresNeighbors(CWorld World, Entity Agent, bool bIgnore)
    {
        SRVOAgentComponent? Avoid = World.Registry.TryGet<SRVOAgentComponent>(Agent);
        if (Avoid != null)
        {
            Avoid.bIgnoreNeighbors = bIgnore;
        }
    }

    /// Below 0.5 the agent takes less than its half of each mutual correction, so others yield to it.
    public static void SetResponsibility(CWorld World, Entity Agent, float Share)
    {
        SRVOAgentComponent? Avoid = World.Registry.TryGet<SRVOAgentComponent>(Agent);
        if (Avoid != null)
        {
            Avoid.Responsibility = Share;
        }
    }

    /// Keeps the solved velocity on the navmesh at the cost of one raycast per solve. Off by default.
    public static void SetClampToNavMesh(CWorld World, Entity Agent, bool bClamp)
    {
        SRVOAgentComponent? Avoid = World.Registry.TryGet<SRVOAgentComponent>(Agent);
        if (Avoid != null)
        {
            Avoid.bClampToNavMesh = bClamp;
        }
    }

    /// True when the agent is currently steering around someone.
    public static bool IsAvoiding(CWorld World, Entity Agent)
    {
        SRVOAgentComponent? Avoid = World.Registry.TryGet<SRVOAgentComponent>(Agent);
        return Avoid != null && Avoid.LastNeighborCount > 0;
    }

    /// Neighbors that constrained the agent's most recent solve, or zero when it has no avoidance.
    public static int GetNeighborCount(CWorld World, Entity Agent)
    {
        return World.Registry.TryGet<SRVOAgentComponent>(Agent)?.LastNeighborCount ?? 0;
    }

    /// How far avoidance pushed the agent off its intended course, in meters per second. Useful for driving a lean or a sidestep animation.
    public static float GetAvoidanceMagnitude(CWorld World, Entity Agent)
    {
        SRVOAgentComponent? Avoid = World.Registry.TryGet<SRVOAgentComponent>(Agent);
        return Avoid != null ? Avoid.GetAvoidanceMagnitude() : 0.0f;
    }

    /// The velocity the agent is actually taking this frame, after avoidance.
    public static FVector3 GetVelocity(CWorld World, Entity Agent)
    {
        SRVOAgentComponent? Avoid = World.Registry.TryGet<SRVOAgentComponent>(Agent);
        return Avoid != null ? Avoid.GetAvoidanceVelocity() : new FVector3(0.0f);
    }

    /// Drives the agent directly, for a flow field or custom steering rather than a navmesh path. Call it every frame; a frame without a call leaves the agent stopped.
    public static void SetPreferredVelocity(CWorld World, Entity Agent, FVector3 Velocity)
    {
        SRVOAgentComponent? Avoid = World.Registry.TryGet<SRVOAgentComponent>(Agent);
        if (Avoid != null)
        {
            Avoid.SetPreferredVelocity(Velocity);
        }
    }
}
