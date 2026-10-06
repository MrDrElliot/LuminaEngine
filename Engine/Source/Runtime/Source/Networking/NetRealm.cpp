#include "NetRealm.h"

#include "Core/Engine/GameLibrary.h"
#include "Core/Object/Class.h"
#include "Core/Object/ScriptClass.h"
#include "World/World.h"

namespace Lumina::NetRealm
{
    ENetRealm Of(const CClass* Class)
    {
        static const FName HostOnlyName("HostOnly");
        static const FName ClientOnlyName("ClientOnly");
        static const FName CosmeticName("Cosmetic");

        ENetRealm Realm = ENetRealm::Any;
        for (const CClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
        {
            if (const CScriptClass* Minted = ToScriptClass(Current))
            {
                Realm |= static_cast<ENetRealm>(Minted->ScriptNetRealm);
            }
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
        }
        return Realm;
    }

    bool Allows(ENetRealm Realm, CWorld* World)
    {
        if (Realm == ENetRealm::Any)
        {
            return true;
        }
        const ENetMode Mode = World != nullptr ? World->GetNetMode() : ENetMode::Standalone;
        if (EnumHasAnyFlags(Realm, ENetRealm::HostOnly) && Mode == ENetMode::Client)
        {
            return false;
        }
        if (EnumHasAnyFlags(Realm, ENetRealm::ClientOnly) && Mode != ENetMode::Client)
        {
            return false;
        }
        return !EnumHasAnyFlags(Realm, ENetRealm::Cosmetic) || CGameLibrary::HasPresentation(World);
    }

    uint32 KeyOf(CWorld* World)
    {
        const ENetMode Mode = World != nullptr ? World->GetNetMode() : ENetMode::Standalone;
        return static_cast<uint32>(Mode) | (CGameLibrary::HasPresentation(World) ? 0x100u : 0u);
    }
}
