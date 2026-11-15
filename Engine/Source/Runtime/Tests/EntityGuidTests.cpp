#include <gtest/gtest.h>

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/EntityGuidComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    CWorld* NewTestWorld()
    {
        // Saving and loading skip storages they cannot name, and these register on first use.
        STransformComponent::StaticStruct();
        SNameComponent::StaticStruct();
        SEntityGuidComponent::StaticStruct();
        SPrefabComponent::StaticStruct();
        SPrefabInstanceComponent::StaticStruct();
        return NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    FName GuidOf(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SEntityGuidComponent* Guid = Registry.TryGet<SEntityGuidComponent>(Entity);
        return Guid != nullptr ? Guid->Guid : FName();
    }

    THashMap<FName, FName> GuidsByName(ECS::FRegistry& Registry)
    {
        THashMap<FName, FName> Out;
        Registry.View<SNameComponent>().ForEach([&](ECS::FEntity Entity, const SNameComponent& Name)
        {
            Out[Name.Name] = GuidOf(Registry, Entity);
        });
        return Out;
    }

    void ExpectEveryGuidUnique(ECS::FRegistry& Registry, int32 ExpectedEntities)
    {
        THashSet<FName> Seen;
        int32 Count = 0;
        Registry.ForEachEntity([&](ECS::FEntity Entity)
        {
            const FName Guid = GuidOf(Registry, Entity);
            EXPECT_FALSE(Guid.IsNone()) << "entity " << Entity.GetIndex() << " has no guid";
            EXPECT_TRUE(Seen.TryAdd(Guid)) << "entity " << Entity.GetIndex() << " repeats guid " << *Guid;
            ++Count;
        });
        EXPECT_EQ(Count, ExpectedEntities);
    }

    TVector<uint8> SaveWorld(CWorld* World)
    {
        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        FInProcessObjectArchiver Proxy(Writer);
        World->Serialize(Proxy);
        return Bytes;
    }

    void LoadWorld(CWorld* World, TVector<uint8>& Bytes)
    {
        FMemoryReader Reader(Bytes);
        FInProcessObjectArchiver Proxy(Reader);
        World->Serialize(Proxy);
    }
}

TEST(EntityGuid, CreatedAndDuplicatedEntitiesEachGetTheirOwn)
{
    TStrongObjectPtr<CWorld> World(NewTestWorld());
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    const ECS::FEntity Parent = World->ConstructEntity("Parent");
    const ECS::FEntity Child = World->ConstructEntity("Child");
    World->SetParent(Child, Parent);

    const ECS::FEntity Copy = World->DuplicateEntity(Parent);
    ASSERT_TRUE(Registry.IsValid(Copy));
    const ECS::FEntity CopyChild = Registry.GetHierarchy().GetFirstChild(Copy);
    ASSERT_TRUE(Registry.IsValid(CopyChild));

    EXPECT_NE(GuidOf(Registry, Copy), GuidOf(Registry, Parent));
    EXPECT_NE(GuidOf(Registry, CopyChild), GuidOf(Registry, Child));
    ExpectEveryGuidUnique(Registry, 4);
}

TEST(EntityGuid, PlayWorldSpawnsGoWithoutAndCopiesNeverShareTheirSource)
{
    TStrongObjectPtr<CWorld> World(NewTestWorld());
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Placed = World->ConstructEntity("Placed");
    FWorldTestAccess::SetWorldType(*World, EWorldType::Game);

    EXPECT_TRUE(GuidOf(Registry, World->ConstructEntity("Spawned")).IsNone());
    EXPECT_TRUE(GuidOf(Registry, World->DuplicateEntity(Placed)).IsNone());
}

TEST(EntityGuid, EachPrefabInstanceGetsItsOwnAndThePrefabKeepsNone)
{
    TStrongObjectPtr<CWorld> World(NewTestWorld());
    TStrongObjectPtr<CPrefab> Prefab(NewObject<CPrefab>(nullptr, NAME_None, FGuid::New(), OF_Transient));

    const ECS::FEntity PrefabRoot = Prefab->Registry.Create();
    Prefab->Registry.Emplace<SPrefabComponent>(PrefabRoot).StableID = FName("root");
    Prefab->Registry.Emplace<STransformComponent>(PrefabRoot);
    const ECS::FEntity PrefabChild = Prefab->Registry.Create();
    Prefab->Registry.Emplace<SPrefabComponent>(PrefabChild).StableID = FName("child");
    Prefab->Registry.Emplace<STransformComponent>(PrefabChild);
    Prefab->Registry.AttachChild(PrefabChild, PrefabRoot);

    ASSERT_TRUE(World->GetEntityRegistry().IsValid(Prefab->Instantiate(World.Get())));
    ASSERT_TRUE(World->GetEntityRegistry().IsValid(Prefab->Instantiate(World.Get())));

    ExpectEveryGuidUnique(World->GetEntityRegistry(), 4);
    EXPECT_TRUE(Prefab->Registry.View<SEntityGuidComponent>().IsEmpty()) << "a capture must not carry entity ids into the prefab";
}

TEST(EntityGuid, SaveAndLoadKeepEveryGuid)
{
    TStrongObjectPtr<CWorld> Source(NewTestWorld());
    ECS::FRegistry& Registry = Source->GetEntityRegistry();
    const ECS::FEntity Parent = Source->ConstructEntity("Parent");
    Source->SetParent(Source->ConstructEntity("Child"), Parent);
    const ECS::FEntity Copy = Source->DuplicateEntity(Parent);
    Registry.Get<SNameComponent>(Copy).Name = FName("Copy");
    Registry.Get<SNameComponent>(Registry.GetHierarchy().GetFirstChild(Copy)).Name = FName("CopyChild");
    Source->ConstructEntity("Loose");
    const THashMap<FName, FName> Saved = GuidsByName(Source->GetEntityRegistry());

    TVector<uint8> Bytes = SaveWorld(Source.Get());
    TStrongObjectPtr<CWorld> Loaded(NewTestWorld());
    LoadWorld(Loaded.Get(), Bytes);

    ECS::FRegistry& Pending = FWorldTestAccess::PendingRegistry(*Loaded);
    ExpectEveryGuidUnique(Pending, 5);
    const THashMap<FName, FName> Reloaded = GuidsByName(Pending);
    ASSERT_EQ(Reloaded.Num(), Saved.Num());
    for (const auto& [Name, Guid] : Saved)
    {
        const FName* ReloadedGuid = Reloaded.Find(Name);
        ASSERT_NE(ReloadedGuid, nullptr) << *Name;
        EXPECT_EQ(*ReloadedGuid, Guid) << *Name;
    }
}

TEST(EntityGuid, WorldSavedBeforeGuidsLoadsEveryEntityWithItsOwnStableOne)
{
    TStrongObjectPtr<CWorld> Old(NewTestWorld());
    ECS::FRegistry Legacy;
    for (const char* Name : { "Sun", "Floor", "Floor", "Camera" })
    {
        const ECS::FEntity Entity = Legacy.Create();
        Legacy.Emplace<SNameComponent>(Entity, FName(Name));
        Legacy.Emplace<STransformComponent>(Entity);
    }

    // What CWorld::Serialize wrote before entities had ids.
    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FInProcessObjectArchiver Proxy(Writer);
        Old->CObject::Serialize(Proxy);
        ECS::Utils::SerializeRegistry(Proxy, Legacy);
    }
    ASSERT_TRUE(Legacy.View<SEntityGuidComponent>().IsEmpty());

    TStrongObjectPtr<CWorld> Loaded(NewTestWorld());
    LoadWorld(Loaded.Get(), Bytes);
    ECS::FRegistry& Pending = FWorldTestAccess::PendingRegistry(*Loaded);
    ExpectEveryGuidUnique(Pending, 4);

    TVector<FName> FirstLoad;
    Pending.ForEachEntity([&](ECS::FEntity Entity) { FirstLoad.Add(GuidOf(Pending, Entity)); });

    LoadWorld(Loaded.Get(), Bytes);
    TVector<FName> SecondLoad;
    Pending.ForEachEntity([&](ECS::FEntity Entity) { SecondLoad.Add(GuidOf(Pending, Entity)); });
    EXPECT_EQ(FirstLoad, SecondLoad) << "an unsaved old world must get the same ids on every load";
}
