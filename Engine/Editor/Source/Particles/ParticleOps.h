#pragma once

#include "ParticleModule.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CClass;
    class CParticleSystem;
    class CParticleEmitterStack;

    struct FParticleEmitterCompileOutput
    {
        bool             bSucceeded = false;
        TVector<FString> Errors;
        TVector<FString> Notes;
    };

    // The authoring operations the particle editor and the agent tools share, so both edit an asset the same way.
    namespace ParticleOps
    {
        // The emitter's module stack, created with a starter stack when the package has none yet.
        EDITOR_API CParticleEmitterStack* FindOrCreateStack(CParticleSystem* System, int32 EmitterIndex);

        // Every concrete module class, found by reflection so a plugin's modules are included.
        EDITOR_API TVector<CClass*> GetModuleTypes();

        // Accepts the class name or the display name, ignoring case.
        EDITOR_API CClass* ResolveModuleType(FStringView Name);

        EDITOR_API TVector<TObjectPtr<CParticleModule>>* GetModules(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage);

        // Inserts into the stack matching the module's stage, where a negative index appends.
        EDITOR_API CParticleModule* AddModule(CParticleSystem* System, int32 EmitterIndex, CClass* ModuleClass, int32 InsertIndex = -1);

        EDITOR_API bool RemoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 Index);

        EDITOR_API bool MoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 From, int32 To);

        // Builds the emitter's shader from its stacks and points the renderer at it.
        EDITOR_API bool CompileEmitter(CParticleSystem* System, int32 EmitterIndex, FParticleEmitterCompileOutput& Out);

        // Rewrites only the input values, which works while the generated code still matches the compiled shader.
        EDITOR_API bool RefreshModuleParams(CParticleSystem* System, int32 EmitterIndex);

        EDITOR_API FString StageName(EParticleModuleStage Stage);

        EDITOR_API bool ParseStage(FStringView Name, EParticleModuleStage& OutStage);
    }
}
