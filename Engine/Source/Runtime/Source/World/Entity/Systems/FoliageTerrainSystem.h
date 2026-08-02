#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "FoliageTerrainSystem.generated.h"

namespace Lumina
{
    // Keeps painted foliage glued to the terrain surface: when a terrain's heightmap changes (sculpt, import),
    // re-projects the height of every follow-enabled foliage instance over the edited region. Runs in the
    // editor (Paused) and at runtime (FrameStart) so sculpting moves foliage live, with no repaint.
    REFLECT(System)
    struct RUNTIME_API SFoliageTerrainSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameStart), RequiresUpdate(EUpdateStage::Paused))

        static FSystemAccess Access;

        static void Startup(const FSystemContext& Context) noexcept;
        static void Update(const FSystemContext& Context) noexcept;
    };
}
