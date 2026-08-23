#include <gtest/gtest.h>

#include "Assets/AssetTypes/Prefabs/PrefabOverride.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"
#include "Scripting/ScriptStruct.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"

using namespace Lumina;

// CEntityScript is one base both languages subclass, driven by one loop of virtual calls.

namespace
{
    CEntityScriptTest* AttachTestScript(FEntityRegistry& Registry, entt::entity Entity)
    {
        return static_cast<CEntityScriptTest*>(
            EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    }
}

// The native half, a C++ subclass through the same driver a C# script uses.
TEST(EntityScriptUnification, CppScriptRunsThroughTheDriver)
{
    FEntityRegistry Registry{};
    const entt::entity Entity = Registry.create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Entity);
    ASSERT_NE(Script, nullptr);

    EXPECT_EQ(Script->AttachCount, 1) << "OnAttach runs at attach time";
    EXPECT_EQ(Script->ReadyCount, 0) << "OnReady is deferred so siblings added the same frame exist first";
    EXPECT_EQ(Script->UpdateCount, 0);
    EXPECT_EQ(Script->GetOwningEntity(), Entity) << "the script must know its entity from OnAttach onwards";

    EntityScripts::Tick(Registry, 0.5f);
    EXPECT_EQ(Script->ReadyCount, 1) << "OnReady runs once, before the first update";
    EXPECT_EQ(Script->UpdateCount, 1);

    EntityScripts::Tick(Registry, 0.25f);
    EXPECT_EQ(Script->ReadyCount, 1) << "OnReady must not run again";
    EXPECT_EQ(Script->UpdateCount, 2);
    EXPECT_FLOAT_EQ(Script->AccumulatedTime, 0.75f) << "delta time is passed through to the script";

    EntityScripts::TickFixed(Registry, 1.0f / 60.0f);
    EXPECT_EQ(Script->FixedUpdateCount, 1);

    EntityScripts::DetachAll(Registry, Entity);
    EXPECT_EQ(Script->DetachCount, 1);
    EXPECT_TRUE(Registry.get<SEntityScriptComponent>(Entity).Scripts.empty());
}

// A component copy (prefab stamp, entity duplicate) clones its scripts rather than sharing one object.
TEST(EntityScriptUnification, CopyingTheComponentClonesItsScripts)
{
    FEntityRegistry Registry{};
    const entt::entity Source = Registry.create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Source);
    ASSERT_NE(Script, nullptr);

    SEntityScriptComponent Copied = Registry.get<SEntityScriptComponent>(Source);
    const entt::entity Destination = Registry.create();
    Registry.emplace_or_replace<SEntityScriptComponent>(Destination, std::move(Copied));

    SEntityScriptComponent& Placed = Registry.get<SEntityScriptComponent>(Destination);
    ASSERT_EQ(Placed.Scripts.size(), size_t(1));

    CEntityScript* Clone = Placed.Scripts[0].Get();
    ASSERT_NE(Clone, nullptr);
    EXPECT_NE(Clone, Script) << "the copy must own its own script object, not the source's";
    EXPECT_TRUE(Clone->GetOwningEntity() == entt::null) << "a clone is unowned until the driver adopts it";

    EntityScripts::Tick(Registry, 0.1f);
    EXPECT_EQ(Clone->GetOwningEntity(), Destination) << "the clone adopts the entity it was copied onto";
    EXPECT_EQ(Script->GetOwningEntity(), Source) << "the source keeps its own entity";
}

// A container property must be copied through its own member address, never the object base.
TEST(EntityScriptUnification, CloningAScriptCopiesContainerPropertiesWithoutSmashingTheObject)
{
    // The property chain is built when the class is finalized; the editor does this at startup.
    ProcessNewlyLoadedCObjects();
    CEntityScriptTest::StaticClass()->GetDefaultObject();

    FEntityRegistry Registry{};
    const entt::entity Source = Registry.create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Source);
    ASSERT_NE(Script, nullptr);
    Script->Values.push_back(FName("Alpha"));
    Script->Values.push_back(FName("Beta"));

    SEntityScriptComponent Copied = Registry.get<SEntityScriptComponent>(Source);
    const entt::entity Destination = Registry.create();
    Registry.emplace_or_replace<SEntityScriptComponent>(Destination, std::move(Copied));

    ASSERT_EQ(Registry.get<SEntityScriptComponent>(Destination).Scripts.size(), size_t(1));
    CEntityScript* Cloned = Registry.get<SEntityScriptComponent>(Destination).Scripts[0].Get();
    ASSERT_NE(Cloned, nullptr);

    // Read through the class rather than a static_cast, so a smashed header shows up here.
    EXPECT_EQ(Cloned->GetClass(), CEntityScriptTest::StaticClass()) << "the clone's class survived the copy";
    CEntityScriptTest* Clone = Cast<CEntityScriptTest>(Cloned);
    ASSERT_NE(Clone, nullptr);

    ASSERT_EQ(Clone->Values.size(), size_t(2)) << "the array copied, and copied into the right place";
    EXPECT_EQ(Clone->Values[0], FName("Alpha"));
    EXPECT_EQ(Clone->Values[1], FName("Beta"));

    EXPECT_EQ(Script->Values.size(), size_t(2)) << "the source is untouched";

    // A virtual call is what a corrupt vtable pointer actually faults on; the driver makes several.
    EntityScripts::Tick(Registry, 0.1f);
    EXPECT_EQ(Clone->UpdateCount, 1);
}

// A component whose state lives in a custom serializer is tracked as ONE atomic leaf.
TEST(EntityScriptUnification, PrefabOverridesTrackAScriptComponentAsOneAtomicValue)
{
    ProcessNewlyLoadedCObjects();
    CEntityScriptTest::StaticClass()->GetDefaultObject();

    auto MakeScript = []
    {
        return static_cast<CEntityScript*>(
            NewObject(CEntityScriptTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient));
    };
    auto ValueOf = [](const SEntityScriptComponent& Component)
    {
        return static_cast<CEntityScriptTest*>(Component.Scripts[0].Get())->Values[0];
    };

    SEntityScriptComponent Authored;
    Authored.Scripts.push_back(MakeScript());
    static_cast<CEntityScriptTest*>(Authored.Scripts[0].Get())->Values.push_back(FName("Prefab"));

    SEntityScriptComponent Instance = Authored;   // a placed instance starts as an exact clone

    CStruct* Layout = SEntityScriptComponent::StaticStruct();
    TVector<FName> Paths;
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Authored, Paths);
    EXPECT_TRUE(Paths.empty()) << "an untouched instance overrides nothing";

    static_cast<CEntityScriptTest*>(Instance.Scripts[0].Get())->Values[0] = FName("Instance");
    Paths.clear();
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Authored, Paths);
    ASSERT_EQ(Paths.size(), size_t(1)) << "editing a script value diverges the component";
    EXPECT_EQ(Paths[0], PrefabOverride::WholeValuePath());

    THashSet<FName> Overridden;
    Overridden.insert(PrefabOverride::WholeValuePath());
    PrefabOverride::ApplyInheritedLeaves(Layout, &Instance, &Authored, Overridden);
    EXPECT_EQ(ValueOf(Instance), FName("Instance")) << "a refresh must not overwrite the override";

    PrefabOverride::ApplyInheritedLeaves(Layout, &Instance, &Authored, THashSet<FName>{});
    EXPECT_EQ(ValueOf(Instance), FName("Prefab")) << "with nothing overridden it still inherits the prefab";
}

// A script that never attached must not receive OnDetach, which would mint an instance.
TEST(EntityScriptUnification, DetachSkipsAScriptThatNeverAttached)
{
    FEntityRegistry Registry{};
    const entt::entity Entity = Registry.create();

    CObject* Created = NewObject(CEntityScriptTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
    CEntityScriptTest* Unadopted = static_cast<CEntityScriptTest*>(Created);
    ASSERT_NE(Unadopted, nullptr);
    Registry.get_or_emplace<SEntityScriptComponent>(Entity).Scripts.push_back(Unadopted);

    EXPECT_FALSE(Unadopted->IsAttached());
    EntityScripts::DetachAll(Registry, Entity);
    EXPECT_EQ(Unadopted->DetachCount, 0) << "no OnAttach ran, so no OnDetach is owed";

    // And the paired case still fires.
    const entt::entity Adopted = Registry.create();
    CEntityScriptTest* Attached = AttachTestScript(Registry, Adopted);
    ASSERT_NE(Attached, nullptr);
    EntityScripts::DetachAll(Registry, Adopted);
    EXPECT_EQ(Attached->DetachCount, 1);
}

// Fixed update must not reach a script that has not readied yet.
TEST(EntityScriptUnification, FixedUpdateWaitsForReady)
{
    FEntityRegistry Registry{};
    const entt::entity Entity = Registry.create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Entity);
    ASSERT_NE(Script, nullptr);

    EntityScripts::TickFixed(Registry, 1.0f / 60.0f);
    EXPECT_EQ(Script->FixedUpdateCount, 0) << "not ready yet; OnFixedUpdate must not have run";
    EXPECT_EQ(Script->ReadyCount, 0);

    EntityScripts::Tick(Registry, 0.1f);
    EntityScripts::TickFixed(Registry, 1.0f / 60.0f);
    EXPECT_EQ(Script->FixedUpdateCount, 1);
}

// Several scripts on one entity, and several scripted entities, all driven by the one loop.
TEST(EntityScriptUnification, ManyScriptsAndEntitiesTickThroughOneLoop)
{
    FEntityRegistry Registry{};
    const entt::entity First = Registry.create();
    const entt::entity Second = Registry.create();

    CEntityScriptTest* A = AttachTestScript(Registry, First);
    CEntityScriptTest* B = AttachTestScript(Registry, First);   // two scripts on one entity
    CEntityScriptTest* C = AttachTestScript(Registry, Second);
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);
    ASSERT_NE(C, nullptr);

    EntityScripts::Tick(Registry, 0.1f);

    EXPECT_EQ(A->UpdateCount, 1);
    EXPECT_EQ(B->UpdateCount, 1);
    EXPECT_EQ(C->UpdateCount, 1);
    EXPECT_EQ(A->GetOwningEntity(), First);
    EXPECT_EQ(B->GetOwningEntity(), First);
    EXPECT_EQ(C->GetOwningEntity(), Second);
}

// A minted CClass is attached and ticked by the SAME driver, with no separate code path.
TEST(EntityScriptUnification, MintedScriptClassTicksThroughTheSameDriver)
{
    CClass* Minted = FScriptableRegistry::Mint("EntityScript_GTestMinted", "CEntityScript", 0);
    ASSERT_NE(Minted, nullptr) << "minting failed (is the CEntityScript shim registered?)";
    ProcessNewlyLoadedCObjects();
    Minted->GetDefaultObject();

    EXPECT_TRUE(Minted->IsChildOf(CEntityScript::StaticClass()))
        << "a minted C# script class must BE a CEntityScript, or the driver could not hold it";

    FEntityRegistry Registry{};
    const entt::entity Entity = Registry.create();

    CEntityScript* Managed = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Managed, nullptr);
    CEntityScriptTest* Native = AttachTestScript(Registry, Entity);
    ASSERT_NE(Native, nullptr);

    // One loop, two kinds of subclass.
    EntityScripts::Tick(Registry, 0.1f);
    EntityScripts::Tick(Registry, 0.1f);

    EXPECT_EQ(Registry.get<SEntityScriptComponent>(Entity).Scripts.size(), 2u);
    EXPECT_TRUE(Managed->IsReady()) << "the minted script was readied by the same pass as the C++ one";
    EXPECT_EQ(Native->UpdateCount, 2);
    EXPECT_EQ(Managed->GetClass(), Minted);

    EntityScripts::DetachAll(Registry, Entity);
    EXPECT_EQ(Native->DetachCount, 1);
}

// A class that is not a CEntityScript must be refused rather than attached and later dispatched to.
TEST(EntityScriptUnification, AttachRefusesANonScriptClass)
{
    FEntityRegistry Registry{};
    const entt::entity Entity = Registry.create();

    EXPECT_EQ(EntityScripts::Attach(Registry, Entity, CScriptableTest::StaticClass()), nullptr);
    EXPECT_EQ(EntityScripts::Attach(Registry, Entity, nullptr), nullptr);
}

// A script is a per-entity SUBOBJECT, serialized by class name plus its tagged properties.
TEST(EntityScriptUnification, ComponentSerializationRoundTripsScriptsAndAdoptsThemOnTick)
{
    FEntityRegistry Source{};
    const entt::entity Entity = Source.create();

    CEntityScriptTest* A = AttachTestScript(Source, Entity);
    CEntityScriptTest* B = AttachTestScript(Source, Entity);
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Source.get<SEntityScriptComponent>(Entity).Serialize(Ar);
    }
    ASSERT_FALSE(Bytes.empty());

    // Load into a fresh registry + entity, exactly as scene load would.
    FEntityRegistry Loaded{};
    const entt::entity LoadedEntity = Loaded.create();
    SEntityScriptComponent& Restored = Loaded.emplace<SEntityScriptComponent>(LoadedEntity);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Restored.Serialize(Ar);
    }

    ASSERT_EQ(Restored.Scripts.size(), 2u) << "both scripts must come back";
    ASSERT_NE(Restored.Scripts[0].Get(), nullptr);
    EXPECT_EQ(Restored.Scripts[0].Get()->GetClass(), CEntityScriptTest::StaticClass())
        << "the script's concrete class must round-trip, not its base";

    // Freshly loaded, so no owner yet and no lifecycle has run.
    EXPECT_EQ(Restored.Scripts[0].Get()->GetOwningEntity(), entt::entity{entt::null});
    EXPECT_FALSE(Restored.Scripts[0].Get()->IsReady());

    EntityScripts::Tick(Loaded, 0.1f);

    for (TObjectPtr<CEntityScript>& Held : Loaded.get<SEntityScriptComponent>(LoadedEntity).Scripts)
    {
        CEntityScriptTest* Script = static_cast<CEntityScriptTest*>(Held.Get());
        ASSERT_NE(Script, nullptr);
        EXPECT_EQ(Script->GetOwningEntity(), LoadedEntity) << "the driver must adopt a loaded script";
        EXPECT_EQ(Script->AttachCount, 1) << "an adopted script gets OnAttach before OnReady";
        EXPECT_EQ(Script->ReadyCount, 1);
        EXPECT_EQ(Script->UpdateCount, 1);
    }
}

// A script's authored property VALUES must survive the round-trip a scene save and PIE take.
TEST(EntityScriptUnification, ScriptPropertyValuesSurviveTheComponentRoundTrip)
{
    Scripting::FScriptExportSchema Schema;
    {
        Scripting::FScriptExportField& Speed = Schema.Fields.emplace_back();
        Speed.Name = FName("Speed");
        Speed.Type = MakeShared<Scripting::FScriptExportType>();
        Speed.Type->Kind = EPropertyTypeFlags::Float;

        Scripting::FScriptExportField& Label = Schema.Fields.emplace_back();
        Label.Name = FName("Label");
        Label.Type = MakeShared<Scripting::FScriptExportType>();
        Label.Type->Kind = EPropertyTypeFlags::String;
    }

    CClass* Minted = FScriptableRegistry::Mint("EntityScript_GTestValues", "CEntityScript", 0);
    ASSERT_NE(Minted, nullptr);
    ASSERT_GT(Scripting::AppendScriptPropertiesToClass(Minted, Schema), 0u);
    ProcessNewlyLoadedCObjects();
    Minted->GetDefaultObject();

    FProperty* Speed = Minted->GetProperty(FName("Speed"));
    FProperty* Label = Minted->GetProperty(FName("Label"));
    ASSERT_NE(Speed, nullptr);
    ASSERT_NE(Label, nullptr);

    FEntityRegistry Source{};
    const entt::entity Entity = Source.create();
    CEntityScript* Script = EntityScripts::Attach(Source, Entity, Minted);
    ASSERT_NE(Script, nullptr);

    // Stand-in for the inspector, writing through exactly these FPropertys at this object.
    Speed->SetValue<float>(Script, 12.5f);
    *Label->GetValuePtr<FString>(Script) = "authored in the inspector";

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Source.get<SEntityScriptComponent>(Entity).Serialize(Ar);
    }
    ASSERT_FALSE(Bytes.empty());

    FEntityRegistry Loaded{};
    const entt::entity LoadedEntity = Loaded.create();
    SEntityScriptComponent& Restored = Loaded.emplace<SEntityScriptComponent>(LoadedEntity);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Restored.Serialize(Ar);
    }

    ASSERT_EQ(Restored.Scripts.size(), 1u);
    CEntityScript* LoadedScript = Restored.Scripts[0].Get();
    ASSERT_NE(LoadedScript, nullptr);
    ASSERT_EQ(LoadedScript->GetClass(), Minted);
    EXPECT_NE(LoadedScript, Script) << "the round-trip must produce a NEW object, not alias the source";

    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(LoadedScript), 12.5f)
        << "an authored scalar reverted; it would silently reset on every Play";
    EXPECT_STREQ(Label->GetValuePtr<FString>(LoadedScript)->c_str(), "authored in the inspector")
        << "an authored string reverted";

    // And the values survive the driver adopting the loaded script, which runs OnAttach/OnReady on it.
    EntityScripts::Tick(Loaded, 0.1f);
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(LoadedScript), 12.5f)
        << "adoption (OnAttach/OnReady) clobbered an authored value";
}

// Class-based lookup, so a C++ script is found by exactly the same call a C# one is.
TEST(EntityScriptUnification, FindAndRemoveByClass)
{
    FEntityRegistry Registry{};
    const entt::entity Entity = Registry.create();

    CEntityScriptTest* First = AttachTestScript(Registry, Entity);
    CEntityScriptTest* Second = AttachTestScript(Registry, Entity);
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);

    EXPECT_EQ(EntityScripts::Find(Registry, Entity, CEntityScriptTest::StaticClass()), First)
        << "Find returns the first match";
    EXPECT_EQ(EntityScripts::Find(Registry, Entity, CEntityScript::StaticClass()), First)
        << "a base class must match a derived script (IsChildOf, not exact type)";

    TVector<CEntityScript*> All;
    EntityScripts::FindAll(Registry, Entity, CEntityScriptTest::StaticClass(), All);
    EXPECT_EQ(All.size(), 2u);

    EXPECT_TRUE(EntityScripts::Remove(Registry, Entity, First));
    EXPECT_EQ(First->DetachCount, 1) << "Remove must run OnDetach";
    EXPECT_EQ(Registry.get<SEntityScriptComponent>(Entity).Scripts.size(), 1u);
    EXPECT_EQ(EntityScripts::Find(Registry, Entity, CEntityScriptTest::StaticClass()), Second);

    EXPECT_FALSE(EntityScripts::Remove(Registry, Entity, First)) << "removing twice must be refused";
}

// The reload evacuates first, since a class rebuild cannot happen with instances alive.
TEST(EntityScriptUnification, ScriptsSurviveAClassLayoutRebuild)
{
    FEntityRegistry Registry{};

    Scripting::FScriptExportSchema Before;
    {
        Scripting::FScriptExportField Speed;
        Speed.Name = FName("Speed");
        Speed.Type = MakeShared<Scripting::FScriptExportType>();
        Speed.Type->Kind = EPropertyTypeFlags::Float;
        Before.Fields.push_back(std::move(Speed));
    }

    CClass* Minted = FScriptableRegistry::Mint("EvacTest_Script", "CEntityScript", 0);
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Before);
    ProcessNewlyLoadedCObjects();
    Minted->GetDefaultObject();

    entt::entity Entity = Registry.create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);

    FProperty* Speed = Minted->GetProperty(FName("Speed"));
    ASSERT_NE(Speed, nullptr);
    Speed->SetValue<float>(Attached, 12.5f);

    // Identity by GUID, not by address, since the pooled allocator may reuse the block.
    const FGuid AttachedGuid = Attached->GetGUID();

    // A rebuild is refused while the script is attached, which is exactly why evacuation exists.
    Scripting::FScriptExportSchema After;
    {
        Scripting::FScriptExportField Kept;
        Kept.Name = FName("Speed");
        Kept.Type = MakeShared<Scripting::FScriptExportType>();
        Kept.Type->Kind = EPropertyTypeFlags::Float;
        After.Fields.push_back(std::move(Kept));

        Scripting::FScriptExportField Added;
        Added.Name = FName("Health");
        Added.Type = MakeShared<Scripting::FScriptExportType>();
        Added.Type->Kind = EPropertyTypeFlags::Int32;
        After.Fields.push_back(std::move(Added));
    }
    EXPECT_FALSE(Scripting::MigrateMintedClassLayout(Minted, After));

    // Evacuated by hand, since a unit test has no world contexts for ForEachWorld to walk.
    TVector<uint8> Bytes;
    {
        SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
        ASSERT_NE(Component, nullptr);
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Component->Serialize(Ar);
        Component->Scripts.clear();
    }
    ASSERT_FALSE(Bytes.empty());

    // With nothing attached the same rebuild goes through.
    ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Minted, After));

    {
        SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entity);
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Component.Serialize(Ar);
    }

    SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
    ASSERT_NE(Component, nullptr);
    ASSERT_EQ(Component->Scripts.size(), 1u) << "the script did not come back";

    CEntityScript* Restored = Component->Scripts[0].Get();
    ASSERT_NE(Restored, nullptr);
    EXPECT_EQ(Restored->GetClass(), Minted);
    EXPECT_NE(Restored->GetGUID(), AttachedGuid) << "restore must build a NEW object; the old one was the old size";

    // The authored value rode across the rebuild, keyed by name.
    FProperty* NewSpeed = Minted->GetProperty(FName("Speed"));
    ASSERT_NE(NewSpeed, nullptr);
    EXPECT_FLOAT_EQ(*NewSpeed->GetValuePtr<float>(Restored), 12.5f) << "the authored value was lost";

    // And the property added by this reload exists on the restored instance, at its default.
    FProperty* Health = Minted->GetProperty(FName("Health"));
    ASSERT_NE(Health, nullptr) << "the added property is missing from the restored instance";
    EXPECT_EQ(*Health->GetValuePtr<int32>(Restored), 0);

    // Still a working script, so the driver adopts it and ticks it like any other.
    EntityScripts::Tick(Registry, 0.016f);

    EntityScripts::DetachAll(Registry, Entity);
}

namespace
{
    Scripting::FScriptExportField MakeReloadField(const char* Name, EPropertyTypeFlags Kind, const char* Aliases = nullptr)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = Kind;
        if (Aliases != nullptr)
        {
            // Exactly what ReadAliasesInto folds a C# [Alias] list into, a ';'-joined metadata value.
            Field.Meta.Set(FName("Aliases"), Aliases);
        }
        return Field;
    }
}

// The replay is name-keyed, so [Alias] is what carries the old name across a rename.
TEST(EntityScriptUnification, RenamingAPropertyKeepsItsValueViaAlias)
{
    FEntityRegistry Registry{};

    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeReloadField("Speed", EPropertyTypeFlags::Float));

    CClass* Minted = FScriptableRegistry::Mint("RenameProp_Script", "CEntityScript", 0);
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Before);
    ProcessNewlyLoadedCObjects();
    Minted->GetDefaultObject();

    const entt::entity Entity = Registry.create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);
    Minted->GetProperty(FName("Speed"))->SetValue<float>(Attached, 9.75f);

    TVector<uint8> Bytes;
    {
        SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
        ASSERT_NE(Component, nullptr);
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Component->Serialize(Ar);
        Component->Scripts.clear();
    }

    // Speed -> Velocity, declaring the old name.
    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeReloadField("Velocity", EPropertyTypeFlags::Float, "Speed"));
    ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Minted, After));
    ASSERT_EQ(Minted->GetProperty(FName("Speed")), nullptr);

    {
        SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entity);
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Component.Serialize(Ar);
    }

    SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
    ASSERT_NE(Component, nullptr);
    ASSERT_EQ(Component->Scripts.size(), 1u);
    CEntityScript* Restored = Component->Scripts[0].Get();
    ASSERT_NE(Restored, nullptr);

    FProperty* Velocity = Minted->GetProperty(FName("Velocity"));
    ASSERT_NE(Velocity, nullptr);
    ASSERT_TRUE(Velocity->HasMetadata("Aliases")) << "the Aliases metadata never reached the property";
    EXPECT_EQ(Velocity->GetMetadata("Aliases"), FString("Speed"));
    EXPECT_FLOAT_EQ(*Velocity->GetValuePtr<float>(Restored), 9.75f)
        << "the renamed property did not inherit the old name's value";

    EntityScripts::DetachAll(Registry, Entity);
}

// Silently keeping the value would mean the replay was matching by OFFSET.
TEST(EntityScriptUnification, RenamingWithoutAnAliasResetsToDefault)
{
    FEntityRegistry Registry{};

    Scripting::FScriptExportSchema Before;
    Before.Fields.push_back(MakeReloadField("Speed", EPropertyTypeFlags::Float));

    CClass* Minted = FScriptableRegistry::Mint("RenameNoAlias_Script", "CEntityScript", 0);
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Before);
    ProcessNewlyLoadedCObjects();
    Minted->GetDefaultObject();

    const entt::entity Entity = Registry.create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);
    Minted->GetProperty(FName("Speed"))->SetValue<float>(Attached, 9.75f);

    TVector<uint8> Bytes;
    {
        SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Component->Serialize(Ar);
        Component->Scripts.clear();
    }

    Scripting::FScriptExportSchema After;
    After.Fields.push_back(MakeReloadField("Velocity", EPropertyTypeFlags::Float));   // no alias
    ASSERT_TRUE(Scripting::MigrateMintedClassLayout(Minted, After));

    {
        SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entity);
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Component.Serialize(Ar);
    }

    CEntityScript* Restored = Registry.get<SEntityScriptComponent>(Entity).Scripts[0].Get();
    ASSERT_NE(Restored, nullptr);
    EXPECT_FLOAT_EQ(*Minted->GetProperty(FName("Velocity"))->GetValuePtr<float>(Restored), 0.0f);

    EntityScripts::DetachAll(Registry, Entity);
}

// The instances are the wrong class, so a class-name redirect joins the evacuate round trip.
TEST(EntityScriptUnification, RenamingAScriptClassMovesItsInstances)
{
    FEntityRegistry Registry{};

    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeReloadField("Speed", EPropertyTypeFlags::Float));

    CClass* Old = FScriptableRegistry::Mint("RenameClass_Before", "CEntityScript", 0);
    ASSERT_NE(Old, nullptr);
    Scripting::AppendScriptPropertiesToClass(Old, Schema);
    ProcessNewlyLoadedCObjects();
    Old->GetDefaultObject();

    const entt::entity Entity = Registry.create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Old);
    ASSERT_NE(Attached, nullptr);
    Old->GetProperty(FName("Speed"))->SetValue<float>(Attached, 4.25f);

    // Evacuate, where the buffer records the OLD class name.
    TVector<uint8> Bytes;
    {
        SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Component->Serialize(Ar);
        Component->Scripts.clear();
    }

    // The reload brings up the renamed type and registers where the old name went.
    CClass* New = FScriptableRegistry::Mint("RenameClass_After", "CEntityScript", 0);
    ASSERT_NE(New, nullptr);
    Scripting::AppendScriptPropertiesToClass(New, Schema);
    ProcessNewlyLoadedCObjects();
    New->GetDefaultObject();
    FScriptableRegistry::RegisterClassRedirect(FName("RenameClass_Before"), FName("RenameClass_After"));

    EXPECT_EQ(FScriptableRegistry::ResolveClass(FName("RenameClass_Before")), New);
    EXPECT_EQ(FScriptableRegistry::ResolveClass(FName("RenameClass_After")), New);
    EXPECT_EQ(FScriptableRegistry::ResolveClass(FName("NeverExisted_Script")), nullptr);

    // And the class the reload should move across is reported for evacuation.
    THashSet<CClass*> Renamed;
    FScriptableRegistry::GatherRenamedClasses(Renamed);
    EXPECT_NE(Renamed.find(Old), Renamed.end()) << "the renamed class was not offered for evacuation";

    {
        SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entity);
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Component.Serialize(Ar);
    }

    SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
    ASSERT_NE(Component, nullptr);
    ASSERT_EQ(Component->Scripts.size(), 1u) << "the renamed script did not come back";

    CEntityScript* Restored = Component->Scripts[0].Get();
    ASSERT_NE(Restored, nullptr);
    EXPECT_EQ(Restored->GetClass(), New) << "the instance stayed on the old class";
    EXPECT_FLOAT_EQ(*New->GetProperty(FName("Speed"))->GetValuePtr<float>(Restored), 4.25f)
        << "the authored value did not survive the class rename";

    // Still a working script on the new class.
    EntityScripts::Tick(Registry, 0.016f);
    EntityScripts::DetachAll(Registry, Entity);
}

// [SkipHotReload] sends the value back to the class default instead of carrying it across.
TEST(EntityScriptUnification, SkipHotReloadFieldsResetOnRestore)
{
    FEntityRegistry Registry{};

    Scripting::FScriptExportSchema Schema;
    Schema.Fields.push_back(MakeReloadField("Kept", EPropertyTypeFlags::Float));
    {
        Scripting::FScriptExportField Scratch = MakeReloadField("Scratch", EPropertyTypeFlags::Float);
        Scratch.Meta.Set(FName("SkipHotReload"), FString());
        Schema.Fields.push_back(std::move(Scratch));
    }

    CClass* Minted = FScriptableRegistry::Mint("SkipHotReload_Script", "CEntityScript", 0);
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Schema);
    ProcessNewlyLoadedCObjects();
    Minted->GetDefaultObject();

    const entt::entity Entity = Registry.create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);
    Minted->GetProperty(FName("Kept"))->SetValue<float>(Attached, 3.5f);
    Minted->GetProperty(FName("Scratch"))->SetValue<float>(Attached, 99.0f);

    TVector<uint8> Bytes;
    {
        SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Component->Serialize(Ar);
        Component->Scripts.clear();
    }

    // A unit test has no world contexts to walk, so replay and reset are driven directly.
    {
        SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entity);
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Component.Serialize(Ar);
        for (const TObjectPtr<CEntityScript>& Held : Component.Scripts)
        {
            Scripting::ResetSkipHotReloadProperties(Held.Get());
        }
    }

    CEntityScript* Restored = Registry.get<SEntityScriptComponent>(Entity).Scripts[0].Get();
    ASSERT_NE(Restored, nullptr);

    EXPECT_FLOAT_EQ(*Minted->GetProperty(FName("Kept"))->GetValuePtr<float>(Restored), 3.5f)
        << "an ordinary property should still carry across";
    EXPECT_FLOAT_EQ(*Minted->GetProperty(FName("Scratch"))->GetValuePtr<float>(Restored), 0.0f)
        << "[SkipHotReload] should have returned this to its class default";

    EntityScripts::DetachAll(Registry, Entity);
}

namespace
{
    struct FDetachMutationContext
    {
        FEntityRegistry* Registry = nullptr;
        entt::entity     Spawned  = entt::null;
    };
}

// World teardown detaches over a snapshot, so an OnDetach touching the pool cannot invalidate the walk.
TEST(EntityScriptUnification, DetachAllInRegistrySurvivesAnOnDetachThatAttachesScripts)
{
    FEntityRegistry Registry{};

    const entt::entity First  = Registry.create();
    const entt::entity Second = Registry.create();
    const entt::entity Third  = Registry.create();

    CEntityScriptTest* FirstScript  = AttachTestScript(Registry, First);
    CEntityScriptTest* SecondScript = AttachTestScript(Registry, Second);
    CEntityScriptTest* ThirdScript  = AttachTestScript(Registry, Third);
    ASSERT_NE(FirstScript, nullptr);
    ASSERT_NE(SecondScript, nullptr);
    ASSERT_NE(ThirdScript, nullptr);

    // Detaching drops the component's only strong ref, so the counters are read through pins, not raw.
    TObjectPtr<CEntityScript> PinFirst(FirstScript);
    TObjectPtr<CEntityScript> PinSecond(SecondScript);
    TObjectPtr<CEntityScript> PinThird(ThirdScript);

    FDetachMutationContext Context;
    Context.Registry = &Registry;

    // Attaching mid-walk grows the very pool a view-based loop would still be iterating.
    FirstScript->HookContext = &Context;
    FirstScript->DetachHook = [](CEntityScriptTest&, void* Ctx)
    {
        FDetachMutationContext& Mutation = *static_cast<FDetachMutationContext*>(Ctx);
        Mutation.Spawned = Mutation.Registry->create();
        EntityScripts::Attach(*Mutation.Registry, Mutation.Spawned, CEntityScriptTest::StaticClass());
    };

    EntityScripts::DetachAllInRegistry(Registry);

    EXPECT_EQ(FirstScript->DetachCount, 1);
    EXPECT_EQ(SecondScript->DetachCount, 1) << "every entity in the snapshot is detached";
    EXPECT_EQ(ThirdScript->DetachCount, 1) << "a mutation mid-walk must not cut the pass short";

    EXPECT_TRUE(Registry.get<SEntityScriptComponent>(First).Scripts.empty());
    EXPECT_TRUE(Registry.get<SEntityScriptComponent>(Second).Scripts.empty());
    EXPECT_TRUE(Registry.get<SEntityScriptComponent>(Third).Scripts.empty());

    ASSERT_TRUE(Context.Spawned != entt::null);
    EXPECT_FALSE(Registry.get<SEntityScriptComponent>(Context.Spawned).Scripts.empty())
        << "a script attached during the pass is outside the snapshot; the clear that follows drops it";
}
