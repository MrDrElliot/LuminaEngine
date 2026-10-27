#pragma once

#include "ParticleModule.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "ParticleEmitterStack.generated.h"

namespace Lumina
{
    class FParticleCompiler;

    // One emitter's authoring data: ordered Spawn/Update module stacks (editor-time only; runtime consumes only the compiled shader).
    // CompileStacks walks enabled modules in order, emitting HLSL spliced into ParticleSimulateTemplate.slang.
    REFLECT()
    class EDITOR_API CParticleEmitterStack : public CObject
    {
        GENERATED_BODY()

    public:

        // Runs every enabled module, Spawn, Update, Grid, then Target, in stack order into the compiler.
        void CompileStacks(FParticleCompiler& Compiler);

        /** Appends a module of the given class to the stack section matching its stage. Returns it. */
        CParticleModule* AddModule(CClass* ModuleClass);

        /** Removes a module from whichever stack holds it. */
        void RemoveModule(CParticleModule* Module);

        /** Moves a module up (-1) or down (+1) within its stack. No-op at the ends. */
        void MoveModule(CParticleModule* Module, int32 Direction);

        /** Seeds a sensible starter stack (point burst with gravity + fade) when both stacks are empty. */
        void EnsureDefaultStack();

        TVector<TStrongObjectPtr<CParticleModule>>& GetStack(EParticleModuleStage Stage);

        /** Modules run once when a particle is born. */
        PROPERTY(Hidden)
        TVector<TStrongObjectPtr<CParticleModule>> SpawnModules;

        /** Modules run every frame on live particles. */
        PROPERTY(Hidden)
        TVector<TStrongObjectPtr<CParticleModule>> UpdateModules;

        // Modules run on every grid cell, split into simulation stages by the Simulation Stage modules among them.
        PROPERTY(Hidden)
        TVector<TStrongObjectPtr<CParticleModule>> GridModules;

        // Modules run on every texel of the grid's render target.
        PROPERTY(Hidden)
        TVector<TStrongObjectPtr<CParticleModule>> TargetModules;
    };
}
