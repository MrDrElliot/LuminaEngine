#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "World/WorldTypes.h"
#include "ScriptReloadContext.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EScriptReloadReason : uint8
    {
        /** The first compile of the session, before any world has ticked. */
        InitialLoad,

        /** Someone asked for a reload: the editor hotkey, the toolbar, or the diagnostics tool. */
        Requested,
    };

    /** Handed to CEntityScript::OnReloaded so a script can tell a fresh start from a reload and rebuild
     *  whatever did not survive the old load context, its delegate bindings above all. */
    REFLECT()
    struct SScriptReloadContext
    {
        GENERATED_BODY()

        PROPERTY()
        EScriptReloadReason Reason = EScriptReloadReason::Requested;

        /** Editor, Game or Simulation, so a script can skip work that only matters while playing. */
        PROPERTY()
        EWorldType WorldType = EWorldType::None;

        /** The entity this script is attached to, the same one GetOwningEntity returns. */
        PROPERTY(Entity)
        uint32 Entity = 0;

        /** Generation of the script load context now live; it advances once per reload. */
        PROPERTY()
        int32 Generation = 0;
    };
}
