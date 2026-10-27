#pragma once
#include "EntitySystem.h"
#include "EntityScriptSystem.generated.h"

namespace Lumina
{
    /**
     * Ticks entity scripts -- in either language.
     *
     * This is the whole driver's ECS entry point. It calls EntityScripts::Tick / TickFixed, which walk
     * SEntityScriptComponent and invoke plain virtuals on CEntityScript, so a C++ script and a C# script are
     * dispatched by the same code and this system contains nothing language-specific. It replaces
     * SCSharpScriptSystem, which was ~334 lines of managed binding, callback masks and generation gates.
     *
     * PrePhysics does OnReady + OnUpdate, once per frame. PostPhysics does OnFixedUpdate (it runs after the
     * physics step, so a script sees settled results) at the FIXED rate, not once per frame: a game-thread
     * accumulator using the world's PhysicsHz/MaxPhysicsSteps runs it 0..N times with a delta of 1/PhysicsHz,
     * so a script integrating in OnFixedUpdate behaves identically at 30 and 144 fps.
     *
     * Scripts on a disabled entity, or under the outliner's per-entity script toggle, are skipped by the view
     * filter in the driver.
     */
    REFLECT()
    class RUNTIME_API SEntityScriptSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;
    };
}
