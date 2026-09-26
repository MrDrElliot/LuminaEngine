#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObjectBase;

    /**
     * One managed (C#) wrapper instance per CObject, so repeated wrapping of the same object returns the same
     * managed object and reference identity holds (`==`, `is`, dictionary keys). Replaces the previous model
     * where every Asset.Load / object-property read allocated a fresh wrapper.
     *
     * Two kinds of entry share this table and their handles differ in strength. A plain wrapper from
     * Wrapper<T>.ForObject is weak, so the table never keeps one alive and a collected target leaves a slot
     * that reports no instance until the next access re-creates it. A C# subclass of a REFLECT(Scriptable)
     * class is strong, because its native object is the only owner it has.
     *
     * So this table can pin the collectible script load context, and what stops it is ReleaseAll running in
     * the unload sequence before that context is dropped. Draining it is required, not hygiene.
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

        /** Frees every cached handle and reclaims every slot. A strong entry points into the load context
         *  being unloaded, so a reload has to call this before it drops that context; shutdown calls it for
         *  the same reason. Objects are untouched and the next access re-creates the wrapper. */
        RUNTIME_API void ReleaseAll();

        /** Number of objects currently holding a cached handle. */
        RUNTIME_API int32 GetLiveCount();

        /** Total slots ever allocated. Live count returning to zero while this stays flat is what proves
         *  slots are recycled rather than leaked; exposed for tests and diagnostics. */
        RUNTIME_API int32 GetSlotCapacity();
    }
}
