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
