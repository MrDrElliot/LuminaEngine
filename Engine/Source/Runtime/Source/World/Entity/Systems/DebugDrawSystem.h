#pragma once

#include "EntitySystem.h"
#include "Core/Math/Frustum.h"
#include "Core/Object/ObjectMacros.h"
#include "DebugDrawSystem.generated.h"

namespace Lumina
{
    class CWorld;
    class FImmediateLineRenderer;

    /**
     * This frame's debug-draw view, published by SDebugDrawSystem into the registry context and read by
     * every debug-line producer. Plain data, transient, never serialized.
     *
     * The immediate line path deliberately has no per-line CPU cull -- by the time a line exists it is
     * already final GPU data, so testing it saves nothing. Culling instead happens one level up, at the
     * granularity of a draw SOURCE: a physics body, a terrain chunk, a navmesh tile. Ask ShouldDraw
     * before generating a source's lines and the pruned ones cost nothing at all.
     */
    struct FDebugDrawState
    {
        // Deliberately holds no FImmediateLineRenderer*. The renderer dies with its scene -- on tool
        // close, or when ReclaimIdleRenderers frees a hidden world's -- and this state is only
        // refreshed on the world's own tick, which for an editor world is the Paused stage while
        // producers like the grid run in FrameStart. A cached pointer dangles in that gap. Resolve the
        // sink through DebugDraw::GetLines at the point of use instead.
        FFrustum    Frustum;
        FVector3    ViewOrigin      = FVector3(0.0f, 0.0f, 0.0f);

        // Beyond this the source is pruned regardless of the frustum. 0 disables the distance test.
        float       MaxDistanceSq   = 0.0f;

        // False when no camera resolved this frame; producers should then draw nothing rather than
        // draw everything, or a world with no view dumps its entire debug set into the buffer.
        bool        bHasView        = false;

        // Master switch (DebugDraw.Enabled). Off short-circuits ShouldDraw for every source.
        bool        bEnabled        = true;

        // Off makes ShouldDraw pass everything, for when the cull itself is what you are debugging.
        bool        bCullSources    = true;
    };

    /**
     * Publishes FDebugDrawState once per frame, ahead of every other system, for both editor (Paused)
     * and game (FrameStart) worlds. Stateless: the state lives in the registry context.
     */
    REFLECT(System)
    struct RUNTIME_API SDebugDrawSystem
    {
        GENERATED_BODY()
        // Paused as well as FrameStart, so editor worlds -- which tick only Paused -- get a view too.
        // Highest on both: this has to land before anything that might draw reads it.
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest),
                      RequiresUpdate(EUpdateStage::Paused,     EUpdatePriority::Highest))

    public:

        // Writes only its own ctx singleton and reads the camera's resolved view, so it never conflicts
        // with a component-touching system. Defined in the .cpp.
        static FSystemAccess Access;

        static void Startup (const FSystemContext& Context) noexcept;
        static void Update  (const FSystemContext& Context) noexcept;
        static void Teardown(const FSystemContext& Context) noexcept;
    };

    namespace DebugDraw
    {
        /** This frame's state for the world, or null if the world has no registry/state yet. */
        RUNTIME_API const FDebugDrawState* GetState(CWorld* World);

        /** Immediate line sink for the world, or null when nothing should be drawn this frame.
         *  Resolved live off the world every call -- never cache the result across a frame boundary. */
        RUNTIME_API FImmediateLineRenderer* GetLines(CWorld* World);

        /** Coarse visibility for one draw source. Call it BEFORE generating the source's lines. */
        RUNTIME_API bool ShouldDraw(const FDebugDrawState& State, const FVector3& Center, float Radius);
        RUNTIME_API bool ShouldDraw(const FDebugDrawState& State, const FAABB& Bounds);
    }
}
