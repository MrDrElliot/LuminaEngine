#pragma once

#include "World.h"
#include "WorldContext.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class RUNTIME_API FWorldManager
    {
    public:

        FWorldManager() = default;
        ~FWorldManager();
        LE_NO_COPYMOVE(FWorldManager);

        /**
         * Latches each world's throttle decision for this frame. Must run before the frame's first
         * UpdateWorlds: every phase after it reads CWorld::IsTickingThisFrame, and UpdateWorlds alone is
         * called once per update stage, so deciding per call could half-tick a world across the stages.
         */
        void BeginFrame(double NowSeconds);

        void UpdateWorlds(const FUpdateContext& UpdateContext);

        // Opens every scene's immediate-line window before anything ticks, since producers with a closed window drop lines instead of writing a slot the GPU owns.
        void BeginImmediateLines();

        // Game thread
        void ReclaimIdleRenderers(double NowSeconds);

        // Steps every world's physics and dispatches contact events, on the game thread between PrePhysics and PostPhysics.
        void TickPhysics();

        // Game thread
        void ExtractWorlds();

        // Called from FRenderManager::FrameEnd, after ExtractWorlds.
        void RenderWorlds(uint8 FrameIndex);

        // Creates a context for an already-constructed CWorld and calls InitializeWorld on it.
        FWorldContext* CreateWorldContext(CWorld* World, EWorldType Type, ENetMode NetMode = ENetMode::Standalone, bool bBot = false);

        // Tears the world down and removes its context. Safe to call with a world that has no context.
        void DestroyWorldContext(CWorld* World);

        // Context lookup. Returns nullptr if the world isn't registered.
        FWorldContext* FindContext(CWorld* World);

        // First Game-type context encountered; null if no game world is loaded (eg, editor-only session).
        FWorldContext* GetPrimaryGameContext();

        // Duplicates SourceWorld into a PIE context of SessionType, either EWorldType::Game or EWorldType::Simulation.
        CWorld* StartPIE(CWorld* SourceWorld, EWorldType SessionType, ENetMode NetMode = ENetMode::Standalone);

        // Tears down and removes a PIE context created by StartPIE.
        void StopPIE(CWorld* PIEWorld);

        const TVector<TUniquePtr<FWorldContext>>& GetContexts() const { return Contexts; }

        /**
         * Every live world, in context order, skipping a context whose world has already gone away.
         *
         * The plain "I need to touch all of them" walk, so callers stop reimplementing the null checks over
         * GetContexts. A template rather than a TFunction so it costs nothing at the call site; the body is
         * short enough to be worth inlining.
         *
         * Do NOT create or destroy a world inside Fn: that mutates Contexts underneath this loop.
         */
        template<typename TFunc>
        void ForEachWorld(TFunc&& Fn)
        {
            for (const TUniquePtr<FWorldContext>& Context : Contexts)
            {
                if (Context == nullptr)
                {
                    continue;
                }
                if (CWorld* World = Context->World.Get())
                {
                    Fn(*World);
                }
            }
        }

    private:

        TVector<TUniquePtr<FWorldContext>> Contexts;
    };

    namespace Global
    {
        RUNTIME_API extern FWorldManager* GWorldManager;
    }
}
