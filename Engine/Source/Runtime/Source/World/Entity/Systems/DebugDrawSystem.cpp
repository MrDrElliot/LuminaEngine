#include "RuntimePCH.h"
#include "DebugDrawSystem.h"
#include "Core/Console/ConsoleVariable.h"
#include "Renderer/ImmediateLineRenderer.h"
#include "SystemSingletons.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarDebugDrawEnabled("DebugDraw.Enabled", true,
        "Master switch for immediate-mode debug lines (physics, grid, navigation).");

    static TConsoleVar<bool> CVarDebugDrawCullSources("DebugDraw.CullSources", true,
        "Frustum/distance cull debug draw at source granularity. Off draws every source, for debugging the cull itself.");

    static TConsoleVar<float> CVarDebugDrawDistance("DebugDraw.MaxDistance", 0.0f,
        "Prune debug sources further than this from the camera. 0 disables the distance test.");

    FSystemAccess SDebugDrawSystem::Access = FSystemAccess{}
        .Read<SCameraComponent>();

    void SDebugDrawSystem::Startup(const FSystemContext& Context) noexcept
    {
        Context.GetRegistry().ctx().emplace<FDebugDrawState>();
    }

    void SDebugDrawSystem::Update(const FSystemContext& Context) noexcept
    {
        FDebugDrawState& State = Context.GetRegistry().ctx().get<FDebugDrawState>();

        CWorld* World = Context.GetWorld();

        State.bEnabled     = CVarDebugDrawEnabled.GetValue();
        State.bCullSources = CVarDebugDrawCullSources.GetValue();
        State.bHasView     = false;

        const float MaxDistance = CVarDebugDrawDistance.GetValue();
        State.MaxDistanceSq = (MaxDistance > 0.0f) ? (MaxDistance * MaxDistance) : 0.0f;

        if (World == nullptr)
        {
            return;
        }

        // SCameraSystem bakes FResolvedSceneView at the END of the world's update (FrameEnd, or Paused
        // in the editor) and this runs at the start, so the view is one frame old. That is fine for a
        // broad-phase cull -- worst case a source that just entered view pops in a frame late -- and it
        // is the same view the renderer actually used, shake and preview overrides included.
        const FResolvedSceneView* Resolved = Context.GetRegistry().ctx().find<FResolvedSceneView>();
        if (Resolved != nullptr && Resolved->bHasView)
        {
            State.Frustum    = Resolved->ViewVolume.GetFrustum();
            State.ViewOrigin = Resolved->ViewVolume.GetViewPosition();
            State.bHasView   = true;
            return;
        }

        // Before the first bake (world just loaded) fall back to the live camera so frame one still draws.
        if (const SCameraComponent* Camera = World->GetActiveCamera())
        {
            State.Frustum    = Camera->GetViewVolume().GetFrustum();
            State.ViewOrigin = Camera->GetPosition();
            State.bHasView   = true;
        }
    }

    void SDebugDrawSystem::Teardown(const FSystemContext& Context) noexcept
    {
        Context.GetRegistry().ctx().erase<FDebugDrawState>();
    }

    namespace DebugDraw
    {
        const FDebugDrawState* GetState(CWorld* World)
        {
            if (World == nullptr)
            {
                return nullptr;
            }

            return ECS::GetWorldRegistry(*World).ctx().find<FDebugDrawState>();
        }

        FImmediateLineRenderer* GetLines(CWorld* World)
        {
            const FDebugDrawState* State = GetState(World);
            if (State == nullptr || !State->bEnabled || !State->bHasView)
            {
                return nullptr;
            }

            // Live off the world, so a renderer torn down since the last tick reads as null instead of
            // as a stale pointer. CWorld::GetImmediateLines already handles suspended/renderer-less.
            return World->GetImmediateLines();
        }

        bool ShouldDraw(const FDebugDrawState& State, const FVector3& Center, float Radius)
        {
            if (!State.bEnabled || !State.bHasView)
            {
                return false;
            }

            if (!State.bCullSources)
            {
                return true;
            }

            if (State.MaxDistanceSq > 0.0f)
            {
                const FVector3 Delta = Center - State.ViewOrigin;
                // Radius-inclusive so a large source straddling the limit is not popped by its origin.
                const float Reach = Math::Max(0.0f, Math::Sqrt(Math::Dot(Delta, Delta)) - Radius);
                if (Reach * Reach > State.MaxDistanceSq)
                {
                    return false;
                }
            }

            return State.Frustum.IntersectsSphere(Center, Radius);
        }

        bool ShouldDraw(const FDebugDrawState& State, const FAABB& Bounds)
        {
            if (!State.bEnabled || !State.bHasView)
            {
                return false;
            }

            if (!State.bCullSources)
            {
                return true;
            }

            if (State.MaxDistanceSq > 0.0f)
            {
                const FVector3 Center = (Bounds.Min + Bounds.Max) * 0.5f;
                const FVector3 Extent = (Bounds.Max - Bounds.Min) * 0.5f;
                const float    Radius = Math::Sqrt(Math::Dot(Extent, Extent));

                const FVector3 Delta = Center - State.ViewOrigin;
                const float Reach = Math::Max(0.0f, Math::Sqrt(Math::Dot(Delta, Delta)) - Radius);
                if (Reach * Reach > State.MaxDistanceSq)
                {
                    return false;
                }
            }

            return State.Frustum.IsInside(Bounds);
        }
    }
}
