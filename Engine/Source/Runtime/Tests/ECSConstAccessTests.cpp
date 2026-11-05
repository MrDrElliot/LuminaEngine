#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "World/ECS/Query.h"
#include "World/Entity/Systems/SystemAccess.h"

using namespace Lumina;

namespace ECSConstAccessTestsInternal
{
    struct FAccessValue
    {
        int32 Value = 0;
    };

    struct FAccessOther
    {
        int32 Value = 0;
    };

    using FMixed   = ECS::TQuery<const FAccessValue, FAccessOther>;
    using FWrapped = ECS::TQuery<ECS::Read<FAccessValue>, ECS::Write<FAccessOther>>;
    using FBoth    = ECS::TQuery<ECS::Read<FAccessValue>, FAccessOther>;

    static_assert(std::is_same_v<FMixed::FView, ECS::TView<ECS::TExclude<>, const FAccessValue, FAccessOther>>);
    static_assert(std::is_same_v<FWrapped::FView, ECS::TView<ECS::TExclude<>, const FAccessValue, ECS::Write<FAccessOther>>>, "Read<T> and const T are one access, and Write<T> keeps its spelling");
    static_assert(std::is_same_v<FBoth::FView, FMixed::FView>, "the two spellings mix in one query");
    static_assert(std::is_same_v<decltype(std::declval<const FWrapped&>().TryGet<FAccessValue>(ECS::FEntity{})), const FAccessValue*>);
    static_assert(std::is_same_v<decltype(std::declval<ECS::FRegistry&>().Get<const FAccessValue>(ECS::FEntity{})), const FAccessValue&>);
    static_assert(std::is_same_v<decltype(std::declval<ECS::FRegistry&>().Get<FAccessValue>(ECS::FEntity{})), FAccessValue&>);
    static_assert(std::is_same_v<decltype(std::declval<const ECS::FRegistry&>().Get<FAccessValue>(ECS::FEntity{})), const FAccessValue&>);
    static_assert(std::is_same_v<decltype(std::declval<ECS::FRegistry&>().TryGet<const FAccessValue>(ECS::FEntity{})), const FAccessValue*>);
    static_assert(std::is_same_v<decltype(std::declval<const FMixed&>().TryGet<FAccessValue>(ECS::FEntity{})), const FAccessValue*>, "a const term grants read");
    static_assert(std::is_same_v<decltype(std::declval<const FMixed&>().TryGet<FAccessOther>(ECS::FEntity{})), FAccessOther*>, "a plain term grants write");
    static_assert(!ECS::CComponent<const FAccessValue>, "const is a spelling of access, never a component type of its own");
}

using namespace ECSConstAccessTestsInternal;

TEST(ECSConstAccess, ConstNeverStampsAndPlainDoes)
{
    ECS::FRegistry Registry;
    Registry.TrackChanges<FAccessValue>();

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<FAccessValue>(Entity);
    const uint32 Since = Registry.AdvanceChangeTick();

    EXPECT_EQ(Registry.Get<const FAccessValue>(Entity).Value, 0);
    EXPECT_NE(Registry.TryGet<const FAccessValue>(Entity), nullptr);
    Registry.View<const FAccessValue>().ForEach([](const FAccessValue&) {});
    EXPECT_FALSE(Registry.FindStorage(ECS::GetComponentTypeID<FAccessValue>())->HasChangedSince(Entity, Since));

    Registry.Get<FAccessValue>(Entity).Value = 5;
    EXPECT_TRUE(Registry.FindStorage(ECS::GetComponentTypeID<FAccessValue>())->HasChangedSince(Entity, Since));
    EXPECT_EQ(Registry.Get<const FAccessValue>(Entity).Value, 5);
}

TEST(ECSConstAccess, QueryReadsConstTermsAndWritesPlainOnes)
{
    ECS::FRegistry Registry;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<FAccessValue>(Entity).Value = Index;
        Registry.Emplace<FAccessOther>(Entity);
    }

    FMixed Mixed;
    Mixed.ForEach(Registry, [](const FAccessValue& In, FAccessOther& Out)
    {
        Out.Value = In.Value * 2;
    });

    int32 Sum = 0;
    Registry.FindView<const FAccessOther>().ForEach([&](const FAccessOther& Other) { Sum += Other.Value; });
    EXPECT_EQ(Sum, 12);
}
