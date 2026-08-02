#pragma once

#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // When a plugin's StartupModule fires during bring-up; phases run in declaration
    // order, modules within a phase in dependency order.
    enum class EPluginLoadingPhase : uint8
    {
        // Before any engine subsystem. Reserved for modules that wrap third-
        // party libs the engine itself depends on. Engine-only.
        Earliest,

        // After core (memory/log/task/physics) but before render/Lua/world.
        Core,

        // After Lua + reflection (ProcessNewlyLoadedCObjects) but before any
        // world or editor UI exists. Most gameplay extensions go here.
        PreEngineInit,

        // After world manager exists, before the project DLL loads.
        EngineInit,

        // After all engine init has finished, before the main loop.
        PostEngineInit,

        // After the project DLL loads; for modules building on project types (e.g. Lua bindings).
        PostProjectLoad,

        // Editor-only: after the editor UI is initialized. Editor tooling
        // plugins go here. Skipped entirely in non-editor builds.
        EditorInit,

        Count,
    };

    RUNTIME_API FStringView LexToString(EPluginLoadingPhase Phase);
    RUNTIME_API EPluginLoadingPhase ParsePluginLoadingPhase(FStringView Str, EPluginLoadingPhase Default = EPluginLoadingPhase::PreEngineInit);
}
