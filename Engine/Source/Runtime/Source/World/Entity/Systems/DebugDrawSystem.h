#pragma once

#include "EntitySystem.h"
#include "SystemResources.h"
#include "SystemSingletons.h"
#include "Core/Math/Frustum.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/CameraComponent.h"
#include "DebugDrawSystem.generated.h"

namespace Lumina
{
    class CWorld;
    class FImmediateLineRenderer;

    // This frame's debug-draw view; producers cull whole draw sources through ShouldDraw, never single lines.
    struct FDebugDrawState : ECS::FResource
    {
        // Holds no renderer pointer, since the renderer can die between refreshes; resolve it through DebugDraw::GetLines.
        FFrustum    Frustum;
        FVector3    ViewOrigin      = FVector3(0.0f, 0.0f, 0.0f);

        // Beyond this the source is pruned regardless of the frustum. 0 disables the distance test.
        float       MaxDistanceSq   = 0.0f;

        // False when no camera resolved this frame, and producers then draw nothing.
        bool        bHasView        = false;

        // Master switch (DebugDraw.Enabled). Off short-circuits ShouldDraw for every source.
        bool        bEnabled        = true;

        // Off makes ShouldDraw pass everything, for when the cull itself is what you are debugging.
        bool        bCullSources    = true;
    };

    // Publishes FDebugDrawState once per frame, ahead of every other system, in editor and game worlds.
    REFLECT()
    class RUNTIME_API CDebugDrawSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        // Paused as well as FrameStart, so editor worlds get a view too, and its readers all run after the systems in either stage.
        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;
        void OnTeardown(const SExclusiveContext& Context) override;

        // Publishes the state every frame, even in a world with no camera.
        bool HasWork(const SSystemContext& Context) override { return true; }

    private:

        ECS::TQuery<ECS::Read<SCameraComponent>> Cameras;
        TResMut<FDebugDrawState> DrawState;
        TRes<FResolvedSceneView> ResolvedView;
        TRes<FCameraGlobalState> CameraState;
    };

    namespace DebugDraw
    {
        // This frame's state for the world, or null if it has none yet.
        RUNTIME_API const FDebugDrawState* GetState(CWorld* World);

        // The world's line sink, or null when nothing should draw; never cache it across frames.
        RUNTIME_API FImmediateLineRenderer* GetLines(CWorld* World);

        // Coarse visibility for one draw source, checked before generating its lines.
        RUNTIME_API bool ShouldDraw(const FDebugDrawState& State, const FVector3& Center, float Radius);
        RUNTIME_API bool ShouldDraw(const FDebugDrawState& State, const SAABB& Bounds);
    }
}
