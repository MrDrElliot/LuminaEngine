#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObjectBase;

    /**
     * One managed (C#) wrapper instance per CObject, so repeated wrapping of the same object returns the SAME
     * managed object and reference identity holds (`==`, `is`, dictionary keys). Replaces the previous model
     * where every Asset.Load / object-property read allocated a fresh wrapper.
     *
     * Handles stored here are WEAK on the managed side: this table never keeps a wrapper alive, it only
     * remembers the one that currently exists. That is what makes the cache safe across a script hot reload --
     * a weak handle cannot pin the collectible script ALC, and once the target is collected the slot simply
     * reports "no instance" and the next access re-creates it.
     *
     * Layering: storage and lifetime live in Core (CObjectBase owns a slot index); actually freeing the GC
     * handle is the scripting layer's job and is installed via SetFreeHandleFn. With no host, the table still
     * works and simply never has anything to free.
     *
     * Game thread only, consistent with the rest of the scripting layer.
     */
    namespace ManagedInstances
    {
        using FFreeHandleFn = void (*)(void* Handle);

        /** Installed by the .NET host. Cleared (nullptr) on host shutdown. */
        RUNTIME_API void SetFreeHandleFn(FFreeHandleFn Fn);

        /** The cached managed handle for Object, or null if it has none. */
        RUNTIME_API void* Find(const CObjectBase* Object);

        /** Caches Handle on Object. Frees any handle it replaces. A null Handle clears (and frees) the slot. */
        RUNTIME_API void Set(CObjectBase* Object, void* Handle);

        /** Frees Object's handle and reclaims its slot. Called from ~CObjectBase; safe when there is none. */
        RUNTIME_API void Release(CObjectBase* Object);

        /** Frees every cached handle and reclaims every slot. Called on script hot reload (the handles may
         *  point into the ALC being unloaded) and at host shutdown. Objects are untouched -- the next access
         *  re-creates the wrapper. */
        RUNTIME_API void ReleaseAll();

        /** Number of objects currently holding a cached handle. */
        RUNTIME_API int32 GetLiveCount();

        /** Total slots ever allocated. Live count returning to zero while this stays flat is what proves
         *  slots are recycled rather than leaked; exposed for tests and diagnostics. */
        RUNTIME_API int32 GetSlotCapacity();
    }
}
