#include <gtest/gtest.h>

#include "World/ECS/Query.h"
#include "World/Entity/Systems/SystemAccess.h"
#include "World/Entity/Systems/SystemParam.h"

using namespace Lumina;

namespace SystemDisjointAccessTestsInternal
{
    struct FDisjointValue  { int32 Value = 0; };
    struct FDisjointPlayer { int32 Value = 0; };
    struct FDisjointEnemy  { int32 Value = 0; };

    template<typename T>
    uint32 IdOf()
    {
        return static_cast<uint32>(ECS::GetComponentTypeID<T>());
    }

    int32 Membership(FSystemAccess& Access, std::initializer_list<uint32> With, std::initializer_list<uint32> Without)
    {
        return DeclareParamMembership(Access, With.begin(), static_cast<int32>(With.size()), Without.begin(), static_cast<int32>(Without.size()));
    }
}

using namespace SystemDisjointAccessTestsInternal;

TEST(SystemDisjointAccess, WithAndWithoutTheSameComponentNeverConflict)
{
    FSystemAccess Players;
    DeclareParamWrite(Players, IdOf<FDisjointValue>(), true, Membership(Players, { IdOf<FDisjointValue>(), IdOf<FDisjointPlayer>() }, {}));

    FSystemAccess Others;
    DeclareParamWrite(Others, IdOf<FDisjointValue>(), true, Membership(Others, { IdOf<FDisjointValue>() }, { IdOf<FDisjointPlayer>() }));

    EXPECT_FALSE(FSystemAccess::Conflicts(Players, Others)) << "no entity can match both, so both write the value at once";
    EXPECT_FALSE(FSystemAccess::Conflicts(Others, Players));
}

TEST(SystemDisjointAccess, AnUnfilteredOrOverlappingAccessStillConflicts)
{
    FSystemAccess Players;
    DeclareParamWrite(Players, IdOf<FDisjointValue>(), true, Membership(Players, { IdOf<FDisjointValue>(), IdOf<FDisjointPlayer>() }, {}));

    FSystemAccess Everyone;
    DeclareParamRead(Everyone, IdOf<FDisjointValue>(), true);
    EXPECT_TRUE(FSystemAccess::Conflicts(Players, Everyone)) << "an access with no membership may reach any entity";

    FSystemAccess Enemies;
    DeclareParamWrite(Enemies, IdOf<FDisjointValue>(), true, Membership(Enemies, { IdOf<FDisjointValue>(), IdOf<FDisjointEnemy>() }, {}));
    EXPECT_TRUE(FSystemAccess::Conflicts(Players, Enemies)) << "an entity can be both a player and an enemy";

    FSystemAccess Mixed;
    DeclareParamWrite(Mixed, IdOf<FDisjointValue>(), true, Membership(Mixed, { IdOf<FDisjointValue>() }, { IdOf<FDisjointPlayer>() }));
    DeclareParamRead(Mixed, IdOf<FDisjointValue>(), true);
    EXPECT_TRUE(FSystemAccess::Conflicts(Players, Mixed)) << "one unfiltered param is enough to overlap";
}

TEST(SystemDisjointAccess, ReadsNeverConflictAndResourcesAlwaysDo)
{
    FSystemAccess First;
    DeclareParamRead(First, IdOf<FDisjointValue>(), true);
    FSystemAccess Second;
    DeclareParamRead(Second, IdOf<FDisjointValue>(), true);
    EXPECT_FALSE(FSystemAccess::Conflicts(First, Second));

    FSystemAccess Writer;
    DeclareParamWrite(Writer, IdOf<FDisjointEnemy>(), false);
    FSystemAccess Reader;
    DeclareParamRead(Reader, IdOf<FDisjointEnemy>(), false);
    EXPECT_TRUE(FSystemAccess::Conflicts(Writer, Reader)) << "a resource carries no membership";
}

TEST(SystemDisjointAccess, AnOptionalWriteStillConflictsWithADisjointWriter)
{
    FSystemAccess Players;
    ECS::TQuery<ECS::Write<FDisjointValue>, ECS::With<FDisjointPlayer>, ECS::Optional<FDisjointEnemy>>::DeclareAccess(Players);

    FSystemAccess Others;
    ECS::TQuery<ECS::Write<FDisjointValue>, ECS::Write<FDisjointEnemy>, ECS::Without<FDisjointPlayer>>::DeclareAccess(Others);

    const FComponentAccess* Optional = Players.ComponentAccesses.FindByPredicate([](const FComponentAccess& Entry) { return Entry.Id == IdOf<FDisjointEnemy>(); });
    ASSERT_NE(Optional, nullptr);
    EXPECT_TRUE(Optional->bWrite);
    EXPECT_EQ(Optional->Membership, Constants::kIndexNone) << "an optional term is declared unscoped";

    EXPECT_TRUE(FSystemAccess::Conflicts(Players, Others)) << "the optional write is not credited with the query's filters";
    EXPECT_TRUE(FSystemAccess::Conflicts(Others, Players));

    FSystemAccess Plain;
    ECS::TQuery<ECS::Write<FDisjointValue>, ECS::With<FDisjointPlayer>>::DeclareAccess(Plain);
    EXPECT_FALSE(FSystemAccess::Conflicts(Plain, Others)) << "without the optional the filters keep the two apart";
}

TEST(SystemDisjointAccess, QueryLookupsStayInsideTheQuery)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Player = Registry.Create();
    Registry.Emplace<FDisjointValue>(Player);
    Registry.Emplace<FDisjointPlayer>(Player);
    const ECS::FEntity Other = Registry.Create();
    Registry.Emplace<FDisjointValue>(Other);

    ECS::TQuery<ECS::Write<FDisjointValue>, ECS::With<FDisjointPlayer>> Players;
    ECS::TQuery<ECS::Write<FDisjointValue>, ECS::Without<FDisjointPlayer>> Others;
    const auto PlayerView = Players.View(Registry);
    const auto OtherView = Others.View(Registry);

    EXPECT_NE(PlayerView.TryGet<FDisjointValue>(Player), nullptr);
    EXPECT_EQ(PlayerView.TryGet<FDisjointValue>(Other), nullptr) << "a lookup cannot reach the other system's entities";
    EXPECT_NE(OtherView.TryGet<FDisjointValue>(Other), nullptr);
    EXPECT_EQ(OtherView.TryGet<FDisjointValue>(Player), nullptr);
}
