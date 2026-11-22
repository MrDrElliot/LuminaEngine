#include "NetRealm.h"

#include "Core/Engine/GameLibrary.h"
#include "Core/Object/Class.h"
#include "World/World.h"

namespace Lumina::NetRealm
{
    namespace
    {
        // A process with no world yet still has presentation unless it is headless.
        bool HasPresentation(const CWorld* World)
        {
            return World != nullptr ? World->HasPresentation() : !CGameLibrary::IsHeadless();
        }
    }

    ENetRealm Of(const CClass* Class)
    {
        static const FName HostOnlyName("HostOnly");
        static const FName ClientOnlyName("ClientOnly");
        static const FName CosmeticName("Cosmetic");
        static const FName NetworkedName("Networked");

        ENetRealm Realm = ENetRealm::Any;
        for (const CClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
        {
            if (Current->HasMeta(HostOnlyName))
            {
                Realm |= ENetRealm::HostOnly;
            }
            if (Current->HasMeta(ClientOnlyName))
            {
                Realm |= ENetRealm::ClientOnly;
            }
            if (Current->HasMeta(CosmeticName))
            {
                Realm |= ENetRealm::Cosmetic;
            }
            if (Current->HasMeta(NetworkedName))
            {
                Realm |= ENetRealm::Networked;
            }
        }
        return Realm;
    }

    bool Allows(ENetRealm Realm, CWorld* World)
    {
        return Realm == ENetRealm::Any || Allows(Realm, World != nullptr ? World->GetNetMode() : ENetMode::Standalone, HasPresentation(World));
    }

    bool Allows(ENetRealm Realm, ENetMode Mode, bool bHasPresentation)
    {
        if (EnumHasAnyFlags(Realm, ENetRealm::HostOnly) && Mode == ENetMode::Client)
        {
            return false;
        }
        if (EnumHasAnyFlags(Realm, ENetRealm::ClientOnly) && Mode != ENetMode::Client)
        {
            return false;
        }
        if (EnumHasAnyFlags(Realm, ENetRealm::Networked) && Mode == ENetMode::Standalone)
        {
            return false;
        }
        return !EnumHasAnyFlags(Realm, ENetRealm::Cosmetic) || bHasPresentation;
    }

    uint32 KeyOf(CWorld* World)
    {
        const ENetMode Mode = World != nullptr ? World->GetNetMode() : ENetMode::Standalone;
        return static_cast<uint32>(Mode) | (HasPresentation(World) ? 0x100u : 0u);
    }
}
