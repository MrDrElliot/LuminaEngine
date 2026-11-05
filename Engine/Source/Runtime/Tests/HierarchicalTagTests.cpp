#include <gtest/gtest.h>

#include "HierarchicalTags/HierarchicalTag.h"
#include "HierarchicalTags/HierarchicalTagLibrary.h"
#include "HierarchicalTags/HierarchicalTagRegistry.h"
#include "Containers/StringFormat.h"

namespace LuminaHierarchicalTagTests
{
    using Lumina::CHierarchicalTagLibrary;
    using Lumina::FHierarchicalTagRegistry;
    using Lumina::FHierarchicalTagMask;
    using Lumina::SHierarchicalTag;
    using Lumina::SHierarchicalTagContainer;

    SHierarchicalTag MakeTag(const char* Name)
    {
        SHierarchicalTag Result;
        Result.TagName = Lumina::FName(Name);
        return Result;
    }

    SHierarchicalTagContainer Container(std::initializer_list<const char*> Names)
    {
        SHierarchicalTagContainer Result;
        for (const char* Name : Names)
        {
            Result.AddTag(MakeTag(Name));
        }
        return Result;
    }

    // This executable is a different module from Runtime, so a registry instanced per module would hand out unrelated ids.
    TEST(HierarchicalTagRegistry, IsSharedWithTheRuntimeModule)
    {
        FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
        const Lumina::uint32 FromHere = Registry.RequestTag("TagTests.Shared.InternedHereFirst");
        const Lumina::uint32 FromRuntime = CHierarchicalTagLibrary::RequestTag("TagTests.Shared.Leaf");
        ASSERT_NE(FromHere, 0u);
        ASSERT_NE(FromRuntime, 0u);

        EXPECT_EQ(CHierarchicalTagLibrary::GetTagName(FromHere), "TagTests.Shared.InternedHereFirst");
        EXPECT_EQ(Registry.RequestTag("TagTests.Shared.Leaf"), FromRuntime);
        EXPECT_EQ(Registry.GetName(FromRuntime), "TagTests.Shared.Leaf");
        EXPECT_EQ(Registry.GetParent(FromRuntime), CHierarchicalTagLibrary::GetParent(FromHere));
    }

    TEST(HierarchicalTagRegistry, MatchesThroughAncestors)
    {
        FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
        const Lumina::uint32 Leaf = Registry.RequestTag("TagTests.Ability.Fire.Fireball");
        const Lumina::uint32 Fire = Registry.RequestTag("TagTests.Ability.Fire");
        const Lumina::uint32 Ice  = Registry.RequestTag("TagTests.Ability.Ice");

        EXPECT_TRUE(Registry.Matches(Leaf, Fire));
        EXPECT_FALSE(Registry.Matches(Fire, Leaf));
        EXPECT_FALSE(Registry.Matches(Leaf, Ice));
        EXPECT_FALSE(Registry.Matches(Leaf, 0));
        EXPECT_EQ(Registry.RequestTag(""), 0u);

        SHierarchicalTag Tag;
        Tag.TagName = Lumina::FName("TagTests.Ability.Fire.Fireball");
        EXPECT_EQ(Tag.GetParent().TagName, Lumina::FName("TagTests.Ability.Fire"));
    }

    TEST(HierarchicalTagRegistry, KeepsNumberSuffixedNamesApart)
    {
        FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
        const Lumina::uint32 Two = Registry.RequestTag("TagTests.Level.Stage_2");
        const Lumina::uint32 Three = Registry.RequestTag("TagTests.Level.Stage_3");

        EXPECT_NE(Two, Three);
        EXPECT_EQ(Registry.GetName(Two), "TagTests.Level.Stage_2");
        EXPECT_EQ(Registry.GetParent(Two), Registry.GetParent(Three));
        EXPECT_EQ(Registry.FindTag(Lumina::FName("TagTests.Level.Never")), 0u);
    }

    TEST(HierarchicalTagMask, AnswersLikeTheContainer)
    {
        const SHierarchicalTagContainer Owned = Container({ "TagTests.Mask.Status.Burning", "TagTests.Mask.Team.Red" });
        const SHierarchicalTagContainer Queries[] =
        {
            Container({}),
            Container({ "TagTests.Mask.Status" }),
            Container({ "TagTests.Mask.Status.Burning" }),
            Container({ "TagTests.Mask.Status.Frozen" }),
            Container({ "TagTests.Mask.Status", "TagTests.Mask.Team.Blue" }),
            Container({ "TagTests.Mask.Status", "TagTests.Mask.Team" }),
            Container({ "TagTests.Mask.Status.Burning.Intense" }),
        };

        const FHierarchicalTagMask OwnedMask(Owned);
        for (const SHierarchicalTagContainer& Query : Queries)
        {
            const FHierarchicalTagMask QueryMask(Query);
            EXPECT_EQ(OwnedMask.HasAny(QueryMask), Owned.HasAny(Query));
            EXPECT_EQ(OwnedMask.HasAll(QueryMask), Owned.HasAll(Query));
            for (const SHierarchicalTag& Each : Query.Tags)
            {
                EXPECT_EQ(OwnedMask.HasTag(Each), Owned.HasTag(Each)) << *Each.TagName.ToString();
                EXPECT_EQ(OwnedMask.HasTagExact(Each), Owned.HasTagExact(Each)) << *Each.TagName.ToString();
            }
        }

        EXPECT_TRUE(OwnedMask.HasTag(MakeTag("TagTests.Mask.Team")));
        EXPECT_FALSE(OwnedMask.HasTagExact(MakeTag("TagTests.Mask.Team")));
        EXPECT_FALSE(OwnedMask.HasTag(MakeTag("TagTests.Mask.Unregistered")));
        EXPECT_FALSE(OwnedMask.HasTag(SHierarchicalTag()));
        EXPECT_TRUE(FHierarchicalTagMask().IsEmpty());
    }

    TEST(HierarchicalTagMask, ReachesIdsPastTheInlineWords)
    {
        for (int32_t Index = 0; Index < 300; ++Index)
        {
            FHierarchicalTagRegistry::Get().RequestTag(Lumina::FName(Lumina::Format("TagTests.Filler.T{}", Index)));
        }
        const SHierarchicalTagContainer Owned = Container({ "TagTests.Late.Leaf" });
        const FHierarchicalTagMask Mask(Owned);

        EXPECT_GT(Owned.Tags[0].GetId(), 256u);
        EXPECT_TRUE(Mask.HasTag(MakeTag("TagTests.Late")));
        EXPECT_TRUE(Mask.HasAll(FHierarchicalTagMask(Container({ "TagTests.Late", "TagTests.Late.Leaf" }))));
        EXPECT_FALSE(Mask.HasAny(FHierarchicalTagMask(Container({ "TagTests.Filler.T5" }))));
    }
}
