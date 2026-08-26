#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"

namespace Lumina
{
    // Server-only, transient (non-reflected). Last-sent serialized bytes of each PROPERTY(Replicated) field,
    // keyed by replicated component wire-index -> per-field bytes. Drives native field-granular diffing in
    // WriteEntityComponents so an unchanged component field isn't resent. Auto-removed with the entity by entt;
    // re-seeded on the spawn baseline.
    // Flat last-sent fields; two allocations per component rather than one per field.
    struct FRepFieldSnapshot
    {
        TVector<uint8>  Bytes;
        TVector<uint32> Offsets;   // NumFields + 1 entries

        uint32 Num() const { return Offsets.empty() ? 0u : (uint32)Offsets.size() - 1u; }
        uint32 FieldSize(uint32 Index) const { return Offsets[Index + 1] - Offsets[Index]; }
        const uint8* FieldData(uint32 Index) const { return Bytes.data() + Offsets[Index]; }
    };

    struct FComponentRepState
    {
        THashMap<uint32, FRepFieldSnapshot> LastSent;

        // Server game-clock time (seconds) of the last PropertyUpdate sent for this entity. Drives
        // oldest-first scheduling in ReplicateDirtyProperties so a per-tick byte budget never starves an entity.
        double LastReplicatedTime = 0.0;
    };
}
