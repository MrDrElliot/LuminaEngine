#pragma once

#include "MeshData.h"
#include "RHIFwd.h"

namespace Lumina
{
    // One GPU array of FMeshletHeaderGPU for every mesh, addressed by slot so a cached reference can never dangle.
    namespace MeshletHeaderSlab
    {
        // A permanently zeroed header that every unset or stale reference resolves to.
        constexpr uint32 kNullSlot = 0;

        // Returns a slot holding the null header until it is written. Thread-safe.
        RUNTIME_API uint32 Acquire();

        // Nulls the slot and recycles it FIFO, so a stale reference reads null or another valid header, never a fault.
        RUNTIME_API void Release(uint32 Slot);

        // Nulls the slot's contents but keeps the slot, so a rebuilt mesh keeps its identity.
        RUNTIME_API void Reset(uint32 Slot);

        // Overwrites in place, so the slot's identity never moves.
        RUNTIME_API void Write(uint32 Slot, const FMeshletHeaderGPU& Header);

        // Whether Slot names skinned geometry, which picks the raster vertex stride.
        RUNTIME_API bool IsSkinnedSlot(uint32 Slot);

        // Re-read every frame, since growth moves the slab.
        RUNTIME_API RHI::GPUPtr GetAddress();

        // Slots the published slab holds, so the scene root can carry the slab as a bounded span rather
        // than as a bare address every consumer indexes on trust.
        RUNTIME_API uint32 GetCapacity();

        RUNTIME_API void Shutdown();
    }
}
