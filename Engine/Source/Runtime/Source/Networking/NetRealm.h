#pragma once

#include "Platform/GenericPlatform.h"
#include "Core/LuminaMacros.h"
#include "ModuleAPI.h"

namespace Lumina
{
    class CClass;
    class CWorld;

    // Where a script, system or world subsystem class runs, from REFLECT(HostOnly, ClientOnly, Cosmetic) or the C# attributes of the same names.
    enum class ENetRealm : uint8
    {
        Any        = 0,
        HostOnly   = 1 << 0, // the host, a dedicated server or a standalone game, the peers whose word is final
        ClientOnly = 1 << 1, // a client joined to a host, never the host or a standalone game
        Cosmetic   = 1 << 2, // a world that draws or plays something, never a dedicated server, a bot or a headless process
    };

    ENUM_CLASS_FLAGS(ENetRealm);

    namespace NetRealm
    {
        // Inherited, so a subclass runs only where its parents allow.
        RUNTIME_API ENetRealm Of(const CClass* Class);

        // Every declared realm must hold, so ClientOnly and Cosmetic together mean a client with a screen.
        RUNTIME_API bool Allows(ENetRealm Realm, CWorld* World);

        inline bool Allows(const CClass* Class, CWorld* World) { return Allows(Of(Class), World); }

        // Changes whenever an answer of Allows could, so a world knows when to recheck its systems.
        RUNTIME_API uint32 KeyOf(CWorld* World);
    }
}
