#include <gtest/gtest.h>

#include "World/ECS/Registry.h"

using namespace Lumina;

namespace ECSAccessWrapperTestsInternal
{
    struct FAccessValue
    {
        int32 Value = 0;
    };

    struct FAccessOther
    {
        int32 Value = 0;
    };

    using FWrappedView = decltype(std::declval<ECS::FRegistry&>().View<ECS::Read<FAccessValue>, ECS::Write<FAccessOther>>());
    using FSpelledView = decltype(std::declval<ECS::FRegistry&>().View<const FAccessValue, FAccessOther>());

    static_assert(std::is_same_v<FWrappedView, FSpelledView>, "Read<T> is a const include and Write<T> a mutable one");
    static_assert(std::is_same_v<decltype(std::declval<ECS::FRegistry&>().Get<ECS::Read<FAccessValue>>(ECS::FEntity{})), const FAccessValue&>);
    static_assert(std::is_same_v<decltype(std::declval<ECS::FRegistry&>().Get<ECS::Write<FAccessValue>>(ECS::FEntity{})), FAccessValue&>);
    static_assert(std::is_same_v<decltype(std::declval<const ECS::FRegistry&>().Get<ECS::Read<FAccessValue>>(ECS::FEntity{})), const FAccessValue&>);
    static_assert(std::is_same_v<decltype(std::declval<ECS::FRegistry&>().TryGet<ECS::Read<FAccessValue>>(ECS::FEntity{})), const FAccessValue*>);
    static_assert(std::is_same_v<decltype(std::declval<ECS::FRegistry&>().TryGet<ECS::Write<FAccessValue>>(ECS::FEntity{})), FAccessValue*>);
    static_assert(std::is_same_v<decltype(std::declval<const FSpelledView&>().Get<ECS::Read<FAccessOther>>(ECS::FEntity{})), const FAccessOther&>,
        "asking a view for Read<T> of a mutable include reads it const");
    static_assert(!ECS::CComponent<ECS::Write<FAccessValue>>, "a wrapper is a spelling, never a component type of its own");

    template<typename T>
    concept CCanGetFromConst = requires(const ECS::FRegistry& Registry) { Registry.Get<T>(ECS::FEntity{}); };

    static_assert(CCanGetFromConst<ECS::Read<FAccessValue>>);
    static_assert(!CCanGetFromConst<ECS::Write<FAccessValue>>, "a const registry cannot hand out write access");
}

using namespace ECSAccessWrapperTestsInternal;

TEST(ECSAccessWrappers, ReadNeverStampsAndWriteDoes)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FAccessValue>();

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<FAccessValue>(Entity);
    const uint32 Since = Registry.AdvanceChangeTick();

    EXPECT_EQ(Registry.Get<ECS::Read<FAccessValue>>(Entity).Value, 0);
    EXPECT_NE(Registry.TryGet<ECS::Read<FAccessValue>>(Entity), nullptr);
    Registry.View<ECS::Read<FAccessValue>>().ForEach([](const FAccessValue&) {});
    EXPECT_FALSE(Registry.FindStorage(ECS::GetComponentTypeID<FAccessValue>())->HasChangedSince(Entity, Since));

    Registry.Get<ECS::Write<FAccessValue>>(Entity).Value = 5;
    EXPECT_TRUE(Registry.FindStorage(ECS::GetComponentTypeID<FAccessValue>())->HasChangedSince(Entity, Since));
    EXPECT_EQ(Registry.Get<ECS::Read<FAccessValue>>(Entity).Value, 5);
}

TEST(ECSAccessWrappers, WrappedViewIteratesLikeTheSpelledOne)
{
    ECS::FRegistry Registry;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<FAccessValue>(Entity).Value = Index;
        Registry.Emplace<FAccessOther>(Entity);
    }

    Registry.View<ECS::Read<FAccessValue>, ECS::Write<FAccessOther>>().ForEach([](const FAccessValue& In, FAccessOther& Out)
    {
        Out.Value = In.Value * 2;
    });

    int32 Sum = 0;
    Registry.FindView<ECS::Read<FAccessOther>>().ForEach([&](const FAccessOther& Other) { Sum += Other.Value; });
    EXPECT_EQ(Sum, 12);
}
