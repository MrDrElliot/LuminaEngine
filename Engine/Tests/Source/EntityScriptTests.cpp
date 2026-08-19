#include <gtest/gtest.h>

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

// Phase 5 of the C# scripting rewrite (Docs/CSharpScriptingRewrite.md): CEntityScript is one base that BOTH
// languages subclass, and EntityScripts::Tick is one loop of plain virtual calls with no language-specific
// path in it. These tests pin that: a C++ script and a minted (C#-shaped) script go through the exact same
// driver, and the driver never mentions either language.

namespace
{
    CEntityScriptTest* AttachTestScript(FEntityRegistry& Registry, entt::entity Entity)
    {
        return static_cast<CEntityScriptTest*>(
            EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    }
}

// The native half: a C++ subclass is created, attached, readied once, and ticked -- through the same driver a
// C# script uses. This is what "we can now write C++ entity scripts" means concretely.
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

// Fixed update must not reach a script that has not readied yet -- otherwise a script could observe a physics
// step before its own OnReady.
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

// THE UNIFICATION ITSELF: a minted CClass deriving CEntityScript -- the exact shape the host creates for a C#
// subclass -- is attached and ticked by the SAME driver, alongside a C++ script, with no separate code path.
// With no .NET host in the test process the shim falls through to the native default, which is the documented
// behavior; what this pins is that the driver accepts both and needs to know about neither.
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

// Step 3 of the cutover: a script is a per-entity SUBOBJECT, so the component serializes each one by class
// name plus its own tagged properties -- the same stock serializer every reflected type uses. Load cannot
// know the entity, so the driver adopts owner-less scripts on the next tick; that is asserted here too.
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

    // Freshly loaded: no owner yet, and no lifecycle has run.
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

// The link the test above does NOT cover: a script's authored PROPERTY VALUES surviving the round-trip.
// This is the exact path a scene save/load and a PIE start take -- CWorld::DuplicateWorld serializes the
// editor world and deserializes it into the play world, so a value that does not survive here is a value
// that silently reverts the moment you press Play, which is indistinguishable from "the inspector does not
// work".
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

    // Stand-in for the user typing values into the inspector: the property table writes through exactly
    // these FPropertys, at exactly this object.
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

// The by-class lookup API backing GetScript/AddScript/RemoveScript. Class-based, so a C++ script is found by
// exactly the same call a C# one is.
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

// Hot reload's round trip, end to end: a script attached to an entity survives its class's property block
// being torn down and rebuilt under it.
//
// The rebuild cannot happen with instances alive (an object's size is baked in at allocation), so the reload
// evacuates first: serialize every affected entity's scripts, drop them, migrate the class, read them back.
// This pins the whole sequence, because each half is useless alone.
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

    // Evacuate by hand against this registry (the reload path walks every world through
    // GWorldManager::ForEachWorld, which a unit test has no contexts for).
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
    EXPECT_NE(Restored, Attached) << "restore must build a NEW object; the old one was the old size";

    // The authored value rode across the rebuild, keyed by name.
    FProperty* NewSpeed = Minted->GetProperty(FName("Speed"));
    ASSERT_NE(NewSpeed, nullptr);
    EXPECT_FLOAT_EQ(*NewSpeed->GetValuePtr<float>(Restored), 12.5f) << "the authored value was lost";

    // And the property added by this reload exists on the restored instance, at its default.
    FProperty* Health = Minted->GetProperty(FName("Health"));
    ASSERT_NE(Health, nullptr) << "the added property is missing from the restored instance";
    EXPECT_EQ(*Health->GetValuePtr<int32>(Restored), 0);

    // Still a working script: the driver adopts it and ticks it like any other.
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
            // Exactly what ReadAliasesInto folds a C# [Alias] list into: a ';'-joined metadata value.
            Field.Meta.Set(FName("Aliases"), Aliases);
        }
        return Field;
    }
}

// Renaming a [Property] keeps its value, as long as the new name declares the old one with [Alias].
//
// The replay is name-keyed, so a rename is indistinguishable from "one property removed, another added"
// unless something carries the old name across. [Alias] is that something, and SerializeTaggedProperties
// already matches tags against it -- this pins that the hot-reload round trip inherits the behavior rather
// than needing its own.
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

// Without an [Alias] the value is genuinely gone, and the new property is at its default rather than holding
// whatever the old one happened to leave in those bytes. Worth pinning: silently keeping the value would mean
// the replay was matching by OFFSET, which is exactly the bug a name-keyed carrier exists to prevent.
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

// Renaming the SCRIPT CLASS. The instances are not the wrong size, they are the wrong class, so the fix is a
// redirect from the old class name to the new one plus the same evacuate/restore round trip. The redirect is
// consulted by SEntityScriptComponent's load path, which means a scene saved before the rename loads too.
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

    // Evacuate: the buffer records the OLD class name.
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

// [SkipHotReload] asks for the OPPOSITE of what a reload normally does: the value goes back to the class
// default instead of being carried across. The attribute crossed to native as metadata for a long time with
// nothing acting on it, so this pins that the restore path honors it.
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

    // EntityScripts::Restore does the replay and then this reset. A unit test has no world contexts for it
    // to walk, so the two steps are driven directly here.
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
