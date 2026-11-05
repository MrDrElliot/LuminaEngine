#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/ScriptableTest.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Systems/EntitySystem.h"

using namespace Lumina;

namespace SystemParamTestsInternal
{
    template<typename T>
    uint32 IdOf()
    {
        return static_cast<uint32>(ECS::GetComponentTypeID<T>());
    }
}

using namespace SystemParamTestsInternal;

TEST(SystemParams, MembersDeclareTheirAccessByExisting)
{
    TStrongObjectPtr<CSystemParamTest> System(NewObject<CSystemParamTest>(nullptr, NAME_None, FGuid::New(), OF_Transient));
    System->ConfigureSystem();

    const FSystemAccess& Access = System->GetAccess();
    EXPECT_TRUE(Algo::Contains(Access.Writes, IdOf<STransformComponent>()));
    EXPECT_TRUE(Algo::Contains(Access.Reads, IdOf<SNameComponent>()));
    EXPECT_TRUE(Algo::Contains(Access.TrackedPools, ECS::GetComponentTypeID<STransformComponent>()));
    EXPECT_TRUE(Algo::Contains(Access.SentEvents, IdOf<FSystemParamTestEvent>()));
    EXPECT_TRUE(Algo::Contains(Access.Reads, IdOf<FSystemParamTestEvent>()));
    EXPECT_TRUE(Algo::Contains(Access.Writes, IdOf<FSystemParamTestState>()));
    EXPECT_FALSE(Algo::Contains(Access.Pools, ECS::GetComponentTypeID<FSystemParamTestState>())) << "a resource never gets a pool";
    EXPECT_FALSE(Algo::Contains(Access.Reads, IdOf<FSystemParamTestMarker>())) << "With tests membership without reading";
    EXPECT_FALSE(Algo::Contains(Access.Writes, IdOf<FSystemParamTestMarker>()));
    EXPECT_TRUE(Algo::Contains(Access.Pools, ECS::GetComponentTypeID<FSystemParamTestMarker>())) << "With's pool is made up front, never by a worker";

    bool bNameReadIsScoped = false;
    for (const FComponentAccess& Entry : Access.ComponentAccesses)
    {
        if (Entry.Id == IdOf<SNameComponent>() && Entry.Membership != Constants::kIndexNone
            && Algo::Contains(Access.Memberships[Entry.Membership].With, IdOf<FSystemParamTestMarker>()))
        {
            bNameReadIsScoped = true;
        }
    }
    EXPECT_TRUE(bNameReadIsScoped) << "a query's accesses carry its membership, With included";

    for (const FComponentAccess& Entry : Access.ComponentAccesses)
    {
        if (Entry.Id == IdOf<STransformComponent>())
        {
            EXPECT_EQ(Entry.Membership, Constants::kIndexNone) << "a transform reaches its parents, so no filter scopes it";
        }
    }
    EXPECT_FALSE(Access.bExclusive);
}

TEST(SystemParams, AQueryBuiltOutsideTheSystemNeverJoinsIt)
{
    TStrongObjectPtr<CSystemParamTest> System(NewObject<CSystemParamTest>(nullptr, NAME_None, FGuid::New(), OF_Transient));
    ECS::TQuery<ECS::Write<SDisabledTag>> Stray;
    System->ConfigureSystem();

    EXPECT_FALSE(Algo::Contains(System->GetAccess().Writes, IdOf<SDisabledTag>()));
}
