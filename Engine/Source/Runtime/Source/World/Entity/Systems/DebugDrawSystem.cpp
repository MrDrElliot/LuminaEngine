#include "RuntimePCH.h"
#include "DebugDrawSystem.h"
#include "Core/Console/ConsoleVariable.h"
#include "Renderer/ImmediateLineRenderer.h"
#include "World/World.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarDebugDrawEnabled("DebugDraw.Enabled", true,
        "Master switch for immediate-mode debug lines (physics, navigation, skeletons).");

    static TConsoleVar<bool> CVarDebugDrawCullSources("DebugDraw.CullSources", true,
        "Frustum/distance cull debug draw at source granularity. Off draws every source, for debugging the cull itself.");

    static TConsoleVar<float> CVarDebugDrawDistance("DebugDraw.MaxDistance", 0.0f,
        "Prune debug sources further than this from the camera. 0 disables the distance test.");

    void CDebugDrawSystem::Configure()
    {
        #if !defined(LE_SHIPPING)
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
        #endif
    }

    void CDebugDrawSystem::OnUpdate(const SSystemContext& Context)
    {
        #if !defined(LE_SHIPPING)
        FDebugDrawState& State = *DrawState;
        State.bEnabled     = CVarDebugDrawEnabled.GetValue();
        State.bCullSources = CVarDebugDrawCullSources.GetValue();
        State.bHasView     = false;

        const float MaxDistance = CVarDebugDrawDistance.GetValue();
        State.MaxDistanceSq = (MaxDistance > 0.0f) ? (MaxDistance * MaxDistance) : 0.0f;

        // The resolved view is one frame old, which is fine for a broad-phase cull.
        const FResolvedSceneView& Resolved = *ResolvedView;
        if (Resolved.bHasView)
        {
            State.Frustum    = Resolved.ViewVolume.GetFrustum();
            State.ViewOrigin = Resolved.ViewVolume.GetViewPosition();
            State.bHasView   = true;
            return;
        }

        // Before the first bake (world just loaded) fall back to the live camera so frame one still draws.
        if (const SCameraComponent* Camera = Cameras.TryGet<SCameraComponent>(CameraState->ActiveCameraEntity))
        {
            State.Frustum    = Camera->GetViewVolume().GetFrustum();
            State.ViewOrigin = Camera->GetPosition();
            State.bHasView   = true;
        }
        #endif
    }

    void CDebugDrawSystem::OnTeardown(const SExclusiveContext& Context)
    {
        #if !defined(LE_SHIPPING)
        Context.GetRegistry().Ctx().Erase<FDebugDrawState>();
        #endif
    }

    namespace DebugDraw
    {
        const FDebugDrawState* GetState(CWorld* World)
        {
            #if !defined(LE_SHIPPING)
            if (World == nullptr)
            {
                return nullptr;
            }

            return World->GetEntityRegistry().Ctx().Find<FDebugDrawState>();
            #else
            return nullptr;
            #endif
        }

        FImmediateLineRenderer* GetLines(CWorld* World)
        {
            const FDebugDrawState* State = GetState(World);
            if (State == nullptr || !State->bEnabled || !State->bHasView)
            {
                return nullptr;
            }

            // Read live off the world so a torn-down renderer reads as null instead of a stale pointer.
            return World->GetImmediateLines();
        }

        #if !defined(LE_SHIPPING)
        template<typename TFrustumTest>
        static bool ShouldDrawSource(const FDebugDrawState& State, const FVector3& Center, float Radius, TFrustumTest&& FrustumTest)
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

            return FrustumTest();
        }
        #endif

        bool ShouldDraw(const FDebugDrawState& State, const FVector3& Center, float Radius)
        {
            #if !defined(LE_SHIPPING)
            return ShouldDrawSource(State, Center, Radius, [&] { return State.Frustum.IntersectsSphere(Center, Radius); });
            #else
            return false;
            #endif
        }

        bool ShouldDraw(const FDebugDrawState& State, const SAABB& Bounds)
        {
            #if !defined(LE_SHIPPING)
            const FVector3 Extent = (Bounds.Max - Bounds.Min) * 0.5f;
            return ShouldDrawSource(State, (Bounds.Min + Bounds.Max) * 0.5f, Math::Sqrt(Math::Dot(Extent, Extent)), [&] { return State.Frustum.IsInside(Bounds); });
            #else
            return false;
            #endif
        }
    }
}
