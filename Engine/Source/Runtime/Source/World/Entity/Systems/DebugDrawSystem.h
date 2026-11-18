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

    // Multiplayer views a viewport turns on for its own world, each drawn by the system that owns that data.
    enum class EDebugDrawNetworking : uint8
    {
        None            = 0,
        // The host's recorded hitboxes, now and at the far edge of the rewind window.
        LagCompensation = 1 << 0,
        // Each remote entity's newest received pose against where it is drawn.
        Proxies         = 1 << 1,
        // Connection, traffic and clock figures as screen text.
        Stats           = 1 << 2,
        // The deterministic simulation's ticks, rollbacks and checksums, with its entities marked.
        Simulation      = 1 << 3,
        // The host's relevancy grid, each client's interest radii, and what each client holds by detail tier.
        Interest        = 1 << 4,
    };
    ENUM_CLASS_FLAGS(EDebugDrawNetworking);

    struct FDebugDrawNetworkingView
    {
        const char*          Label;
        EDebugDrawNetworking View;
    };

    // Shared by every place that toggles the views, so a new one shows up in all of them.
    inline constexpr FDebugDrawNetworkingView kDebugDrawNetworkingViews[] =
    {
        { "Lag Compensation Hitboxes", EDebugDrawNetworking::LagCompensation },
        { "Proxy Interpolation",       EDebugDrawNetworking::Proxies },
        { "Network Stats",             EDebugDrawNetworking::Stats },
        { "Interest Management",       EDebugDrawNetworking::Interest },
        { "Deterministic Simulation",  EDebugDrawNetworking::Simulation },
    };

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

        // Set from the viewport's menu and kept across frames, unlike the view above.
        EDebugDrawNetworking Networking = EDebugDrawNetworking::None;

        NODISCARD bool Draws(EDebugDrawNetworking View) const { return bEnabled && EnumHasAnyFlags(Networking, View); }
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
        TRes<SCameraGlobalState> CameraState;
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
