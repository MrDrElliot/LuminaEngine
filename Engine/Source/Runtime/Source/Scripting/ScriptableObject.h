#pragma once

#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;
    class CClass;

    namespace Scriptable
    {
        /**
         * The managed instance backing a C# subclass of a REFLECT(Scriptable) native class, created on first
         * dispatch. Returns null for the class default object, when the .NET host is down, and when the
         * managed type failed to instantiate.
         *
         * There is no per-instance bridge any more: the handle lives in the object's managed-instance slot
         * (Core/Object/ManagedInstance.h), so it is created lazily, freed by ~CObjectBase, and drained with
         * the rest of the table before a hot reload. "The slot is empty" is therefore the whole rebind
         * condition -- after a reload the next dispatch simply builds an instance of the new generation's
         * type, with no generation stamp to keep in step.
         *
         * Game thread only, like the rest of the scripting layer.
         */
        RUNTIME_API void* GetOrCreateInstance(CObject* Object);
    }

    struct FScriptableNativeInfo
    {
        CClass*  (*GetBaseClass)() = nullptr;          // the native Scriptable class's StaticClass()
        CObject* (*Factory)(void* Memory) = nullptr;   // placement-new the shim into Memory
        uint32   ShimSize = 0;
        uint32   ShimAlign = 0;
    };

    struct RUNTIME_API FScriptableRegistry
    {
        static void RegisterNative(const char* NativeClassName, const FScriptableNativeInfo& Info);

        static CClass* Mint(FStringView TypeName, FStringView NativeBaseName, uint64 OverrideFlags);

        static void RefreshMintedClasses();

    };
}
