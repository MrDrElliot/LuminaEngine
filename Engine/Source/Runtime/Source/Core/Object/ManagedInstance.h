#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObjectBase;

    // One C# wrapper per CObject on the game thread, weak for plain wrappers and strong for a script subclass's own instance.
    namespace ManagedInstances
    {
        using FFreeHandleFn = void (*)(void* Handle);
        using FIsHandleAliveFn = bool (*)(void* Handle);

        // Installed by the .NET host and cleared on host shutdown.
        RUNTIME_API void SetFreeHandleFn(FFreeHandleFn Fn);

        // Lets the table tell a collected weak twin from a live one without knowing what a GC handle is.
        RUNTIME_API void SetIsHandleAliveFn(FIsHandleAliveFn Fn);

        RUNTIME_API void* Find(const CObjectBase* Object);

        // A weak wrapper never displaces a script's own instance, which would then never receive another dispatch.
        RUNTIME_API void Set(CObjectBase* Object, void* Handle, bool bScriptTwin = false);

        RUNTIME_API bool IsScriptTwin(const CObjectBase* Object);

        // Find and IsScriptTwin under one lock, since every script event dispatch asks both. A collected weak twin reads as none.
        RUNTIME_API void* FindScriptTwin(const CObjectBase* Object);

        // C# owns an object nothing native owns, so its twin is held weakly and lives as long as C# keeps it.
        RUNTIME_API bool AdoptScriptTwin(CObjectBase* Object, void* WeakHandle);

        // Moves whenever a cached handle is freed or replaced, so a caller holding one can tell it is still good.
        RUNTIME_API uint32 GetHandleGeneration();

        // Called from ~CObjectBase, so it is safe when Object has no handle.
        RUNTIME_API void Release(CObjectBase* Object);

        // Strong entries pin the script load context, so a reload or shutdown must call this before dropping it.
        RUNTIME_API void ReleaseAll();

        RUNTIME_API int32 GetLiveCount();

        // Stays flat while the live count returns to zero when slots are recycled rather than leaked.
        RUNTIME_API int32 GetSlotCapacity();
    }
}
