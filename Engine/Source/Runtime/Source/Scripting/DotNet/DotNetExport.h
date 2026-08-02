#pragma once

#include "Platform/GenericPlatform.h"
#include "World/Entity/EntityHandle.h"   // entt::entity - the engine's single entt include site.

namespace Lumina { class CWorld; }

// Boilerplate helper for the hand-written native -> C# bindings (the gameplay World / Physics / Nav / Net
// surface plus the host's component-ops / registry / asset exports the Reflector can't auto-generate).
// Instead of repeating the flat-C signature everywhere:
//
//     extern "C" LUMINA_SCRIPT_API FVector3 LuminaSharp_Physics_GetLinearVelocity(uint64 World, uint32 Entity) { ... }
//
// write:
//
//     LUMINA_DOTNET_EXPORT(FVector3, Physics_GetLinearVelocity)(uint64 World, uint32 Entity) { ... }
//
// The macro stamps the `LuminaSharp_` prefix and the `extern "C" LUMINA_SCRIPT_API` linkage the C# side
// resolves by name: bind it with [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Physics_GetLinearVelocity")].
// Convention: `World` is an opaque CWorld* (uint64); `Entity` is an entt id (uint32). Game thread only.
//
// LUMINA_SCRIPT_API is always dllexport (see ModuleAPI.h) so these thunks land in the exe's export table in
// monolithic Shipping just as they do in their module DLL in modular builds; C# resolves them by name either way.
//
// MSVC C4190 ("UDT returned with C linkage") is expected here and harmless: every export that returns
// FVector3 / FQuat / a wire struct does so deliberately, mirroring a blittable C# struct byte for byte. The
// macro suppresses it at each export site so the real build log stays clean.
#if defined(_MSC_VER)
    #define LUMINA_DOTNET_EXPORT(ReturnType, Name)  \
        __pragma(warning(suppress: 4190))           \
        extern "C" LUMINA_SCRIPT_API ReturnType LuminaSharp_##Name
#else
    #define LUMINA_DOTNET_EXPORT(ReturnType, Name)  \
        extern "C" LUMINA_SCRIPT_API ReturnType LuminaSharp_##Name
#endif

namespace Lumina::DotNet
{
    // Opaque-handle <-> engine-type conversions shared by every gameplay export.
    FORCEINLINE CWorld*      AsWorld (uint64 Handle)        { return reinterpret_cast<CWorld*>(Handle); }
    FORCEINLINE entt::entity AsEntity(uint32 Entity)        { return static_cast<entt::entity>(Entity); }
    FORCEINLINE uint32       ToId    (entt::entity Entity)  { return static_cast<uint32>(Entity); }
}
