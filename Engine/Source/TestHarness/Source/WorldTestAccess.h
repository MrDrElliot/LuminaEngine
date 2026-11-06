#pragma once

#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Core/Object/Class.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/World.h"

namespace Lumina
{
    // What a test needs from a world's internals to run systems by hand, kept out of CWorld's public surface.
    struct FWorldTestAccess
    {
        static const SSystemContext& SystemContext(const CWorld& World) { return World.SystemContext; }
        static void RebuildSystemSchedule(CWorld& World) { World.RebuildSystemSchedule(); }
        static void TickFixedUpdate(CWorld& World, double FrameDeltaSeconds) { World.TickFixedUpdate(FrameDeltaSeconds); }

        // InitializeWorld is what sets the type, and it creates a renderer a test process cannot.
        static void SetWorldType(CWorld& World, EWorldType Type) { World.WorldType = Type; }

        static void TickStage(CWorld& World, EUpdateStage Stage, double DeltaSeconds)
        {
            World.SystemContext.UpdateStage   = Stage;
            World.SystemContext.DeltaTime     = DeltaSeconds;
            World.SystemContext.RealDeltaTime = DeltaSeconds;
            World.TickSystems(World.SystemContext);
        }

        // Discovery reaches every system class in the process, so a test disables all but the ones it names.
        static THashSet<FName> AllSystemsExcept(TSpan<const FName> Kept)
        {
            THashSet<FName> Disabled;
            EntitySystems::ForEachSystemClass([&](CClass* Class)
            {
                if (!Algo::Contains(Kept, Class->GetName()))
                {
                    Disabled.Add(Class->GetName());
                }
            });
            return Disabled;
        }

        static void KeepOnlySystems(CWorld& World, TSpan<const FName> Kept)
        {
            EntitySystems::CreateMissing(World, AllSystemsExcept(Kept), World.Systems);
            World.RebuildSystemSchedule();
        }
    };
}
