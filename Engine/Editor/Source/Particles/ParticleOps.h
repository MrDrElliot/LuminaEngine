#pragma once

#include "ParticleModule.h"
#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Renderer/ShaderCompiler.h"

namespace Lumina
{
    class CClass;
    class CPackage;
    class CParticleEmitter;
    class CParticleSystem;
    class CParticleEmitterStack;
    class FParticleCompiler;

    struct FParticleEmitterCompileOutput
    {
        bool             bSucceeded = false;
        TVector<FString> Errors;
        TVector<FString> Notes;
    };

    // One emitter compile in flight, launched by BeginCompileEmitter and applied by FinishCompileEmitter on the game thread.
    struct EDITOR_API FParticleEmitterCompileJob
    {
        FParticleEmitterCompileJob();
        ~FParticleEmitterCompileJob();

        TStrongObjectPtr<CParticleSystem> System;
        int32                             EmitterIndex = -1;
        TUniquePtr<FParticleCompiler>     Compiler;
        // Shared with the compile callback, which can still be running when an abandoned job goes away.
        TSharedPtr<TVector<uint32>>       Binaries;
        FShaderCompileWaiter              Waiter;
    };

    // The authoring operations the particle editor and the agent tools share, so both edit an asset the same way.
    namespace ParticleOps
    {
        // The emitter's module stack, created with a starter stack when the package has none yet.
        EDITOR_API CParticleEmitterStack* FindOrCreateStack(CParticleSystem* System, int32 EmitterIndex);

        // Null when the package never saved a stack for the emitter.
        EDITOR_API CParticleEmitterStack* FindStack(CParticleSystem* System, int32 EmitterIndex);

        // A transient system holding only Emitter, so an emitter asset is edited, previewed and compiled the way a system's emitter is.
        EDITOR_API TStrongObjectPtr<CParticleSystem> WrapEmitter(CParticleEmitter* Emitter);

        // Copies the emitter and its authoring stack into Dest, where NAME_None picks the copy's name.
        EDITOR_API CParticleEmitter* CopyEmitter(CParticleEmitter* Source, CPackage* Dest, FName EmitterName = NAME_None);

        // Saves the emitter at EmitterIndex as a new emitter asset at Path, which the system then shares. Null on failure.
        EDITOR_API CParticleEmitter* SaveEmitterAsAsset(CParticleSystem* System, int32 EmitterIndex, FStringView Path);

        // Replaces the shared emitter asset at EmitterIndex with a copy the system owns.
        EDITOR_API CParticleEmitter* MakeEmitterLocal(CParticleSystem* System, int32 EmitterIndex);

        // Every concrete module class, found by reflection so a plugin's modules are included.
        EDITOR_API TVector<CClass*> GetModuleTypes();

        // Accepts the class name or the display name, ignoring case.
        EDITOR_API CClass* ResolveModuleType(FStringView Name);

        EDITOR_API TVector<TStrongObjectPtr<CParticleModule>>* GetModules(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage);

        // Inserts into the stack matching the module's stage, where a negative index appends.
        EDITOR_API CParticleModule* AddModule(CParticleSystem* System, int32 EmitterIndex, CClass* ModuleClass, int32 InsertIndex = -1);

        EDITOR_API bool RemoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 Index);

        EDITOR_API bool MoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 From, int32 To);

        // Builds the emitter's shader from its stacks and points the renderer at it.
        EDITOR_API bool CompileEmitter(CParticleSystem* System, int32 EmitterIndex, FParticleEmitterCompileOutput& Out);

        // CompileEmitter in two halves, so a caller can keep the game thread running while the shader builds.
        EDITOR_API bool BeginCompileEmitter(CParticleSystem* System, int32 EmitterIndex, FParticleEmitterCompileJob& Job,
                                            FParticleEmitterCompileOutput& Out, ETaskPriority Priority = ETaskPriority::Medium);
        EDITOR_API bool FinishCompileEmitter(FParticleEmitterCompileJob& Job, FParticleEmitterCompileOutput& Out);

        // Rewrites only the input values, which works while the generated code still matches the compiled shader.
        EDITOR_API bool RefreshModuleParams(CParticleSystem* System, int32 EmitterIndex);

        EDITOR_API FString StageName(EParticleModuleStage Stage);

        EDITOR_API bool ParseStage(FStringView Name, EParticleModuleStage& OutStage);

        // Every stage in stack order, so a caller listing modules covers the grid and render target ones too.
        EDITOR_API TSpan<const EParticleModuleStage> GetStages();
    }
}
