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

        // Opens every live scene's immediate-line write window. Must run before anything ticks or draws
        // this frame -- producers whose window is closed drop their lines rather than write into a slot
        // the GPU still owns. Closed again by each scene's Extract.
        void BeginImmediateLines();

        // Game thread
        void ReclaimIdleRenderers(double NowSeconds);

        // Steps every live world's physics and dispatches the contact events it raised. Game thread,
        // synchronous, between the DuringPhysics and PostPhysics stages.
        void TickPhysics();

        // Game thread
        void ExtractWorlds();

        // Called from FRenderManager::FrameEnd, after ExtractWorlds.
        void RenderWorlds(uint8 FrameIndex);

        // Creates a context for an already-constructed CWorld and calls InitializeWorld on it.
        FWorldContext* CreateWorldContext(CWorld* World, EWorldType Type, ENetMode NetMode = ENetMode::Standalone);

        // Tears the world down and removes its context. Safe to call with a world that has no context.
        void DestroyWorldContext(CWorld* World);

        // Context lookup. Returns nullptr if the world isn't registered.
        FWorldContext* FindContext(CWorld* World);

        // First Game-type context encountered; null if no game world is loaded (eg, editor-only session).
        FWorldContext* GetPrimaryGameContext();

        // Duplicates SourceWorld, registers a PIE context of the given type, returns the new world.
        // SessionType should be EWorldType::Game (Play-In-Editor) or EWorldType::Simulation (Simulate).
        CWorld* StartPIE(CWorld* SourceWorld, EWorldType SessionType, ENetMode NetMode = ENetMode::Standalone);

        // Tears down and removes a PIE context created by StartPIE.
        void StopPIE(CWorld* PIEWorld);

        const TVector<TUniquePtr<FWorldContext>>& GetContexts() const { return Contexts; }

    private:

        TVector<TUniquePtr<FWorldContext>> Contexts;
    };

    RUNTIME_API extern FWorldManager* GWorldManager;
}
