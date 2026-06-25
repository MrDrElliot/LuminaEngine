#pragma once
#include "EntitySystem.h"
#include "CSharpScriptSystem.generated.h"

namespace Lumina
{
    // Ticks C# EntityScripts. For each SScriptComponent it binds a managed instance
    // (generation-keyed, so the same path handles first attach and hot-reload rebind) and
    // dispatches OnUpdate. Runs in BOTH PrePhysics and PostPhysics: binding/input/OnReady happen once in the
    // PrePhysics pass, and OnUpdate is dispatched in two groups -- PrePhysics-phase scripts in the PrePhysics
    // pass, [UpdatePhase(PostPhysics)] scripts in the PostPhysics pass. Reflected -> auto-registered.
    REFLECT(System)
    struct SCSharpScriptSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics), RequiresUpdate(EUpdateStage::PostPhysics))

        static void Update(const FSystemContext& Context) noexcept;
    };
}
