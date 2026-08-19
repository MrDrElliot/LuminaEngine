#pragma once
#include <string_view>
#include "Physics/Physics.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#if JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>
#endif
#include "Containers/String.h"
#include "Jolt/Core/JobSystemThreadPool.h"

namespace Lumina
{
    class FImmediateLineRenderer;
}

namespace Lumina::Physics
{
    #if JPH_DEBUG_RENDERER
    /**
     * Jolt's debug output routed onto the immediate line path (FImmediateLineRenderer): every DrawLine
     * becomes two vertex writes into mapped GPU memory, so a scene with a hundred thousand collider
     * edges costs the step little more than the memory traffic.
     *
     * Bodies are pruned one level up, by SDebugDrawSystem's frustum via the BodyDrawFilter -- Jolt then
     * never walks the shape of a body that would not be seen.
     */
    class FJoltDebugRenderer : public JPH::DebugRendererSimple
    {
    public:

        void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override;
        void DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString, JPH::ColorArg inColor, float inHeight) override {}

        void DrawBodies(JPH::PhysicsSystem* System, CWorld* InWorld);

        FORCEINLINE void SetWorld(CWorld* InWorld) { World = InWorld; }
        FORCEINLINE void SetDrawDuration(float InDuration) { Duration = InDuration; }

        /** Point the renderer at this frame's immediate sink. Null falls back to the timed batcher,
         *  which is what the one-off query draws (CastSphere's debug shapes) still want. */
        FORCEINLINE void SetImmediateSink(FImmediateLineRenderer* InLines) { Lines = InLines; }

    private:

        double Duration = 0.0f;

        CWorld* World = nullptr;

        // Non-owning, valid only for the frame SetImmediateSink was called on.
        FImmediateLineRenderer* Lines = nullptr;
    };
    #endif
    
    struct FJoltData
    {
        // Either Jolt's own thread pool or our fiber-scheduler bridge, chosen at init by a CVar.
        TUniquePtr<JPH::JobSystem> JobSystem;
        #if JPH_DEBUG_RENDERER
        TUniquePtr<FJoltDebugRenderer> DebugRenderer;
        #endif
        FString LastErrorMessage;
    };

    
    class FJoltPhysicsContext : public IPhysicsContext
    {
    public:

        void Initialize() override;
        void Shutdown() override;
        TUniquePtr<IPhysicsScene> CreatePhysicsScene(CWorld* World) override;

        static JPH::JobSystem* GetThreadPool();
        #if JPH_DEBUG_RENDERER
		static FJoltDebugRenderer* GetDebugRenderer();
        #endif
    };
}
