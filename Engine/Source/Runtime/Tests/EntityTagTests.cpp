#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/EntityUtils.h"

using namespace Lumina;

namespace
{
    TVector<FName> TagsOf(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        TVector<FName> Tags;
        ECS::Utils::ForEachEntityTag(Registry, Entity, [&](const FName& Tag) { Tags.Add(Tag); });
        return Tags;
    }

    void AddTag(ECS::FRegistry& Registry, ECS::FEntity Entity, const char* Tag)
    {
        Registry.NamedStorage<STagComponent>(FName(Tag)).Emplace(Entity).Tag = FName(Tag);
    }
}

// The details panel lists these, and setting a tag used to leave a second unnamed copy beside the per-tag one.
TEST(EntityTags, SetTagListsOnce)
{
    RegisterPendingTypes();
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    ECS::Utils::SetEntityTag(Registry, Entity, FName("Enemy"));
    EXPECT_EQ(TagsOf(Registry, Entity), TVector<FName>{ FName("Enemy") });
    EXPECT_EQ(ECS::Utils::GetEntityTag(Registry, Entity), FName("Enemy"));

    ECS::Utils::SetEntityTag(Registry, Entity, FName("Boss"));
    EXPECT_EQ(TagsOf(Registry, Entity), TVector<FName>{ FName("Boss") }) << "setting replaces rather than adds";

    ECS::Utils::ClearEntityTag(Registry, Entity);
    EXPECT_TRUE(TagsOf(Registry, Entity).IsEmpty());
}

// A save writes every tag storage and a load reads them back, so the tag must still be found after the round trip.
TEST(EntityTags, ATagSurvivesSaveAndLoad)
{
    RegisterPendingTypes();
    ECS::FRegistry Source;
    const ECS::FEntity Entity = Source.Create();
    ECS::Utils::SetEntityTag(Source, Entity, FName("Enemy"));
    AddTag(Source, Entity, "Flying");

    TVector<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    ECS::Utils::SerializeRegistry(Writer, Source);

    ECS::FRegistry Loaded;
    FMemoryReader Reader(Bytes);
    ECS::Utils::SerializeRegistry(Reader, Loaded);

    TVector<FName> Tags = TagsOf(Loaded, Entity);
    Algo::Sort(Tags, [](const FName& A, const FName& B) { return FStringView(*A) < FStringView(*B); });
    EXPECT_EQ(Tags, (TVector<FName>{ FName("Enemy"), FName("Flying") }));
    EXPECT_FALSE(ECS::Utils::GetEntityTag(Loaded, Entity).IsNone());
}

// A prefab spawn copies registries, and every tag of an entity has to arrive in its own storage.
TEST(EntityTags, CopyKeepsEveryTag)
{
    RegisterPendingTypes();
    ECS::FRegistry Source;
    const ECS::FEntity Entity = Source.Create();
    AddTag(Source, Entity, "Enemy");
    AddTag(Source, Entity, "Flying");

    ECS::FRegistry Dest;
    THashMap<ECS::FEntity, ECS::FEntity> Map;
    CPrefab::CopyRegistry(Source, Dest, Map);

    const ECS::FEntity Copy = Map[Entity];
    EXPECT_TRUE(ECS::Utils::EntityHasTag(FName("Enemy"), Dest, Copy));
    EXPECT_TRUE(ECS::Utils::EntityHasTag(FName("Flying"), Dest, Copy));
    EXPECT_EQ(TagsOf(Dest, Copy).Num(), 2);
}
