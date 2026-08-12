#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
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

        /**
         * Records that a script class used to be called OldName and is now NewName, so a saved reference to
         * the old name still resolves.
         *
         * Name to name, not name to CClass*, on purpose: a redirect is registered before the class it points
         * at has necessarily been minted (the first reload after a rename gathers the aliases and the types
         * in the same pass), and resolving lazily removes that ordering dependency entirely.
         *
         * Populated from the `[Alias]` attributes on C# script classes.
         */
        static void RegisterClassRedirect(const FName& OldName, const FName& NewName);

        /**
         * The class for a serialized script class name, following any rename redirects.
         *
         * This is what makes a class rename non-destructive in BOTH directions that matter: a scene saved
         * before the rename still loads, and a hot reload can move live instances onto the new class by
         * round-tripping them through the same serializer.
         *
         * Returns null if neither the name nor anything it redirects to exists.
         */
        static CClass* ResolveClass(const FName& Name);

        /** Every minted class whose C# type is gone but whose name redirects to a live type, so a reload can
         *  move its instances across before it is retired. */
        static void GatherRenamedClasses(THashSet<CClass*>& Out);
    };
}
