#pragma once

#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;
    class CClass;

    // Per-instance managed binding carried by a Reflector-generated Scriptable shim.
    struct RUNTIME_API FScriptableBridge
    {
        void*    Handle = nullptr;
        int32    OverrideFlags = 0;
        int32    Generation = -1;
        CObject* Self = nullptr;

        // Records the owner from the shim's PostInitProperties. Does NOT create the managed instance.
        void Attach(CObject* InSelf);

        // Creates the managed instance for Self's (minted) class and records the override flags. No-op for the
        // class default object. Called lazily from EnsureBound on the first dispatch (when OF_DefaultObject is set).
        void Bind(CObject* InSelf);

        // Re-binds when the script generation changed since Bind.
        void EnsureBound();

        // True when the managed subclass overrides ScriptEvent Bit.
        bool ShouldDispatch(int32 Bit);

        // Frees the managed instance (called from the shim destructor).
        void Destroy();
    };
    
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
        
        static CClass* Mint(FStringView TypeName, FStringView NativeBaseName);
        
        static void RefreshMintedClasses();
    };
}
