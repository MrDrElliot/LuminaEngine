#include <gtest/gtest.h>

#include "Containers/StaticArray.h"
#include "Networking/NetRealm.h"
#include "World/Entity/Systems/ScriptSyncSystem.h"
#include "World/WorldContext.h"

using namespace Lumina;

namespace
{
    constexpr ENetMode kModes[] = { ENetMode::Standalone, ENetMode::Client, ENetMode::ListenServer, ENetMode::DedicatedServer };

    // Which of Standalone, Client, ListenServer and DedicatedServer, each with presentation, Realm lets a system run in.
    TArray<bool, 4> RunsIn(ENetRealm Realm)
    {
        TArray<bool, 4> Out{};
        for (int32 Index = 0; Index < 4; ++Index)
        {
            Out[Index] = NetRealm::Allows(Realm, kModes[Index], true);
        }
        return Out;
    }
}

TEST(NetRealm, EachRealmKeepsASystemOutOfTheSchedulersItDoesNotBelongIn)
{
    using R = TArray<bool, 4>;
    EXPECT_EQ(RunsIn(ENetRealm::Any),        (R{ true,  true,  true,  true  }));
    EXPECT_EQ(RunsIn(ENetRealm::HostOnly),   (R{ true,  false, true,  true  }));
    EXPECT_EQ(RunsIn(ENetRealm::ClientOnly), (R{ false, true,  false, false }));
    EXPECT_EQ(RunsIn(ENetRealm::Networked),  (R{ false, true,  true,  true  }));
    EXPECT_EQ(RunsIn(ENetRealm::HostOnly | ENetRealm::Networked), (R{ false, false, true, true })) << "a server, never a standalone game";
}

TEST(NetRealm, CosmeticNeedsSomethingToDrawOrPlay)
{
    EXPECT_FALSE(NetRealm::Allows(ENetRealm::Cosmetic, ENetMode::DedicatedServer, false));
    EXPECT_TRUE(NetRealm::Allows(ENetRealm::Cosmetic, ENetMode::ListenServer, true));
    EXPECT_FALSE(NetRealm::Allows(ENetRealm::Cosmetic | ENetRealm::ClientOnly, ENetMode::ListenServer, true));
}

// Script sync only feeds the network send, so a standalone game never schedules it.
TEST(NetRealm, ScriptSyncRunsOnlyInASession)
{
    EXPECT_EQ(NetRealm::Of(CScriptSyncSystem::StaticClass()), ENetRealm::Networked);
}
