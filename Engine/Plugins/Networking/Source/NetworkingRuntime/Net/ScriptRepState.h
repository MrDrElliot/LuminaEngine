#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandle.h"

namespace Lumina
{
    // Server-only transient last-sent bytes of each Replicated field, so an unchanged field is not resent. Flat, with two allocations per component.
    struct FRepFieldSnapshot
    {
        TVector<uint8>  Bytes;
        TVector<uint32> Offsets;   // NumFields + 1 entries

        uint32 Num() const { return Offsets.IsEmpty() ? 0u : (uint32)Offsets.Num() - 1u; }
        uint32 FieldSize(uint32 Index) const { return Offsets[Index + 1] - Offsets[Index]; }
        const uint8* FieldData(uint32 Index) const { return Bytes.GetData() + Offsets[Index]; }
    };

    struct FComponentRepState
    {
        THashMap<uint32, FRepFieldSnapshot> LastSent;

        // Replicated components this entity has lost, so a resend after a dropped packet can remove them again.
        TVector<uint32> RemovedWireIndices;

        // Keyed by script handle rather than index, since removing a script shifts the ones after it.
        THashMap<FObjectHandle, FRepFieldSnapshot> ScriptLastSent;

        // When each script field last went out, only kept for scripts with a rate-limited field.
        THashMap<FObjectHandle, TVector<double>> ScriptFieldSentTime;

        // Server game-clock time of the last PropertyUpdate, driving oldest-first scheduling so a byte budget never starves an entity.
        double LastReplicatedTime = 0.0;
    };
}
