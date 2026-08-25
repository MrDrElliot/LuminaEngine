#pragma once

#include "MeshData.h"
#include "RHIFwd.h"

namespace Lumina
{
    /** One GPU array of FMeshletHeaderGPU for every mesh in the process, addressed by SLOT rather than by
     *  address. A meshlet header is the one GPU resource whose location gets cached in CPU-side scene state
     *  -- resolve entries, mesh components, the retained instance buffer -- and then re-emitted into frames
     *  recorded long after it was cached. RHI::Core::Retire fences against work already SUBMITTED, so it
     *  cannot see those copies, and freeing a header on the fence alone left the cull dispatch chasing a
     *  dangling pointer: a device loss, not a bad pixel.
     *
     *  A slot cannot dangle. Slot 0 is permanently a zeroed header -- no geometry pointers, MeshletCount 0
     *  -- and every field that names a header defaults to it, so an instance the CPU never filled and an
     *  instance still naming a mesh that has died both resolve to a header describing NO geometry. The
     *  slab itself is the only allocation, it is published into the scene root once per frame, and it is
     *  the only thing whose address is ever held.
     *
     *  That is what lets the shaders index it unconditionally: MeshletCount bounds every array the header
     *  points at, so there is nothing to null-check and nothing to bounds-check against a second source. */
    namespace MeshletHeaderSlab
    {
        /** The header that describes no geometry. Never acquired, never released, never written. */
        constexpr uint32 kNullSlot = 0;

        /** A slot holding the null header until it is Written. Thread-safe. */
        RUNTIME_API uint32 Acquire();

        /** Resets the slot to the null header and returns it to the free list. kNullSlot is ignored.
         *
         *  Slots are recycled FIFO, so a slot is not handed out again until every other free slot has
         *  been. Scene state still naming a released slot therefore reads the null header in practice,
         *  and in the worst case reads another mesh's header -- in-bounds, well-formed, and at most a
         *  frame of the wrong mesh before the retained upload catches up. Never a fault. */
        RUNTIME_API void Release(uint32 Slot);

        /** Resets the slot's CONTENTS to the null header while keeping the slot. What a rebuild uses: the
         *  slot is the mesh's identity and must survive, but the geometry it named is being retired. */
        RUNTIME_API void Reset(uint32 Slot);

        /** Publishes Header into Slot. Overwrites in place; the slot's identity never moves. */
        RUNTIME_API void Write(uint32 Slot, const FMeshletHeaderGPU& Header);

        /** Whether Slot's header names skinned geometry, which is what decides the vertex stride the raster
         *  reads it at. kNullSlot and an unwritten slot are both false. */
        RUNTIME_API bool IsSkinnedSlot(uint32 Slot);

        /** Base address of the slab. Re-read every frame when the scene root is built: growth moves it,
         *  and nothing may cache it across frames. Never 0 once the RHI is up. */
        RUNTIME_API RHI::GPUPtr GetAddress();

        /** Teardown. Frees the slab; every slot is invalid afterwards. */
        RUNTIME_API void Shutdown();
    }
}
