#include <gtest/gtest.h>
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Prefabs/PrefabOverride.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Systems/SystemAccess.h"
#include "Log/Log.h"
#include "LogCountTestHelpers.h"
#include "Core/Object/ObjectReinstancer.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
#include "ScriptTestUtil.h"
#include "Scripting/ScriptableTest.h"
#include "Scripting/ScriptStruct.h"
#include "Core/Threading/Thread.h"
#include "Containers/HashTable.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include <chrono>
#include <cstdio>

using namespace Lumina;

namespace
{
    // A registry a unit test owns is not reachable from GWorldManager, so it takes part in a reinstance the
    // same way the engine's own holders do rather than through anything test-specific in the reinstancer.
    class FLocalRegistryReferenceProvider final : public IObjectReferenceProvider
    {
    public:

        explicit FLocalRegistryReferenceProvider(ECS::FRegistry& InRegistry)
            : Registry(InRegistry)
        {
        }

        const char* GetReferenceProviderName() const override { return "test registry"; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override
        {
            for (ECS::FSparseSet* Storage : Registry.GetActiveStorages())
            {
                CStruct* const Struct = Storage != nullptr ? Storage->GetStruct() : nullptr;
                if (Struct == nullptr)
                {
                    continue;
                }

                const ECS::FEntity* Dense = Storage->GetDenseData();
                const size_t        Count = Storage->GetDenseSize();
                for (size_t Index = 0; Index < Count; ++Index)
                {
                    if (!Dense[Index].IsTombstone())
                    {
                        FObjectReferenceVisitor::VisitStruct(Struct, Storage->GetRawAtDense((uint32)Index), Func);
                    }
                }
            }
        }

    private:

        ECS::FRegistry& Registry;
    };
}


// CEntityScript is one base both languages subclass, driven by one loop of virtual calls.

namespace
{
    CEntityScriptTest* AttachTestScript(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return static_cast<CEntityScriptTest*>(
            EntityScripts::Attach(Registry, Entity, CEntityScriptTest::StaticClass()));
    }

    // An update phase is C# data, so only a minted class carries one. This is the shape production has:
    // a script type minted over a native scriptable base, which is what the driver then dispatches.
    CEntityScriptTest* AttachMintedTestScript(ECS::FRegistry& Registry, ECS::FEntity Entity, const char* TypeName)
    {
        FScriptableNativeInfo Info;
        Info.GetBaseClass = &CEntityScriptTest::StaticClass;
        Info.Factory      = &CEntityScriptTest::__PlacementNew;
        Info.ShimSize     = CEntityScriptTest::StaticClass()->GetSize();
        Info.ShimAlign    = CEntityScriptTest::StaticClass()->GetAlignment();
        FScriptableRegistry::RegisterNative("CEntityScriptTest", Info);

        CScriptClass* Minted = FScriptableRegistry::Mint(TypeName, "CEntityScriptTest");
        if (Minted == nullptr)
        {
            return nullptr;
        }
        RegisterPendingTypes();
        Minted->GetDefaultObject();

        return static_cast<CEntityScriptTest*>(EntityScripts::Attach(Registry, Entity, Minted));
    }
}

// The native half, a C++ subclass through the same driver a C# script uses.
TEST(EntityScriptUnification, CppScriptRunsThroughTheDriver)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

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

    // Detaching drops the component's only strong ref, so the counter is read through a pin.
    TStrongObjectPtr<CEntityScript> Pinned(Script);
    EntityScripts::DetachAll(Registry, Entity);
    EXPECT_EQ(Script->DetachCount, 1);
    EXPECT_TRUE(Registry.Get<SEntityScriptComponent>(Entity).Scripts.IsEmpty());
}

// [Reflect(UpdatePhase = PostPhysics)] moves OnUpdate to the pass after the physics step, keeping OnReady where it was.
TEST(EntityScriptUnification, UpdatePhaseSelectsWhichPassRunsOnUpdate)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    CEntityScriptTest* Script = AttachMintedTestScript(Registry, Entity, "EntityScript_GTestPhase");
    ASSERT_NE(Script, nullptr);

    CScriptClass* Class = ToScriptClass(Script->GetClass());
    ASSERT_NE(Class, nullptr) << "the phase lives on a minted class, so the script must be one";
    const uint8 PriorPhase = Class->ScriptUpdatePhase;
    Class->ScriptUpdatePhase = (uint8)EScriptUpdatePhase::PostPhysics;

    EntityScripts::Tick(Registry, 0.5f, EScriptUpdatePhase::PrePhysics);
    EXPECT_EQ(Script->ReadyCount, 1) << "OnReady still drains in the PrePhysics pass whatever the phase";
    EXPECT_EQ(Script->UpdateCount, 0) << "a PostPhysics script must not update before the physics step";

    EntityScripts::Tick(Registry, 0.5f, EScriptUpdatePhase::PostPhysics);
    EXPECT_EQ(Script->UpdateCount, 1) << "it updates in the pass its class declares";

    Class->ScriptUpdatePhase = PriorPhase;

    // Only a reload changes a class's phase in practice, and a reload notes a structure change the claim is keyed on.
    EntityScripts::NoteStructureChange();

    EntityScripts::Tick(Registry, 0.5f, EScriptUpdatePhase::PrePhysics);
    EXPECT_EQ(Script->UpdateCount, 2) << "the default phase is PrePhysics, exactly as before";

    EntityScripts::Tick(Registry, 0.5f, EScriptUpdatePhase::PostPhysics);
    EXPECT_EQ(Script->UpdateCount, 2) << "and it must not also run in the second pass";
}

// A component copy (prefab stamp, entity duplicate) clones its scripts rather than sharing one object.
TEST(EntityScriptUnification, CopyingTheComponentClonesItsScripts)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Source = Registry.Create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Source);
    ASSERT_NE(Script, nullptr);

    SEntityScriptComponent Copied = Registry.Get<SEntityScriptComponent>(Source);
    const ECS::FEntity Destination = Registry.Create();
    Registry.EmplaceOrReplace<SEntityScriptComponent>(Destination, std::move(Copied));

    SEntityScriptComponent& Placed = Registry.Get<SEntityScriptComponent>(Destination);
    ASSERT_EQ(Placed.Scripts.Num(), 1);

    CEntityScript* Clone = Placed.Scripts[0].Get();
    ASSERT_NE(Clone, nullptr);
    EXPECT_NE(Clone, Script) << "the copy must own its own script object, not the source's";
    EXPECT_TRUE(Clone->GetOwningEntity() == ECS::NullEntity) << "a clone is unowned until the driver adopts it";

    EntityScripts::Tick(Registry, 0.1f);
    EXPECT_EQ(Clone->GetOwningEntity(), Destination) << "the clone adopts the entity it was copied onto";
    EXPECT_EQ(Script->GetOwningEntity(), Source) << "the source keeps its own entity";
}

// A container property must be copied through its own member address, never the object base.
TEST(EntityScriptUnification, CloningAScriptCopiesContainerPropertiesWithoutSmashingTheObject)
{
    // The property chain is built when the class is finalized; the editor does this at startup.
    RegisterPendingTypes();
    CEntityScriptTest::StaticClass()->GetDefaultObject();

    ECS::FRegistry Registry{};
    const ECS::FEntity Source = Registry.Create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Source);
    ASSERT_NE(Script, nullptr);
    Script->Values.Emplace("Alpha");
    Script->Values.Emplace("Beta");

    SEntityScriptComponent Copied = Registry.Get<SEntityScriptComponent>(Source);
    const ECS::FEntity Destination = Registry.Create();
    Registry.EmplaceOrReplace<SEntityScriptComponent>(Destination, std::move(Copied));

    ASSERT_EQ(Registry.Get<SEntityScriptComponent>(Destination).Scripts.Num(), 1);
    CEntityScript* Cloned = Registry.Get<SEntityScriptComponent>(Destination).Scripts[0].Get();
    ASSERT_NE(Cloned, nullptr);

    // Read through the class rather than a static_cast, so a smashed header shows up here.
    EXPECT_EQ(Cloned->GetClass(), CEntityScriptTest::StaticClass()) << "the clone's class survived the copy";
    CEntityScriptTest* Clone = Cast<CEntityScriptTest>(Cloned);
    ASSERT_NE(Clone, nullptr);

    ASSERT_EQ(Clone->Values.Num(), 2) << "the array copied, and copied into the right place";
    EXPECT_EQ(Clone->Values[0], FName("Alpha"));
    EXPECT_EQ(Clone->Values[1], FName("Beta"));

    EXPECT_EQ(Script->Values.Num(), 2) << "the source is untouched";

    // A virtual call is what a corrupt vtable pointer actually faults on; the driver makes several.
    EntityScripts::Tick(Registry, 0.1f);
    EXPECT_EQ(Clone->UpdateCount, 1);
}

// A component whose state lives in a custom serializer is tracked as ONE atomic leaf.
TEST(EntityScriptUnification, PrefabOverridesTrackAScriptComponentAsOneAtomicValue)
{
    RegisterPendingTypes();
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
    Authored.Scripts.Add(MakeScript());
    static_cast<CEntityScriptTest*>(Authored.Scripts[0].Get())->Values.Emplace("Prefab");

    SEntityScriptComponent Instance = Authored;   // a placed instance starts as an exact clone

    CStruct* Layout = SEntityScriptComponent::StaticStruct();
    TVector<FName> Paths;
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Authored, Paths);
    EXPECT_TRUE(Paths.IsEmpty()) << "an untouched instance overrides nothing";

    static_cast<CEntityScriptTest*>(Instance.Scripts[0].Get())->Values[0] = FName("Instance");
    Paths.Reset();
    PrefabOverride::CollectOverriddenLeaves(Layout, &Instance, &Authored, Paths);
    ASSERT_EQ(Paths.Num(), 1) << "editing a script value diverges the component";
    EXPECT_EQ(Paths[0], PrefabOverride::WholeValuePath());

    THashSet<FName> Overridden;
    Overridden.Add(PrefabOverride::WholeValuePath());
    PrefabOverride::ApplyInheritedLeaves(Layout, &Instance, &Authored, Overridden);
    EXPECT_EQ(ValueOf(Instance), FName("Instance")) << "a refresh must not overwrite the override";

    PrefabOverride::ApplyInheritedLeaves(Layout, &Instance, &Authored, THashSet<FName>{});
    EXPECT_EQ(ValueOf(Instance), FName("Prefab")) << "with nothing overridden it still inherits the prefab";
}

// A script that never attached must not receive OnDetach, which would mint an instance.
TEST(EntityScriptUnification, DetachSkipsAScriptThatNeverAttached)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    CObject* Created = NewObject(CEntityScriptTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
    CEntityScriptTest* Unadopted = static_cast<CEntityScriptTest*>(Created);
    ASSERT_NE(Unadopted, nullptr);
    Registry.GetOrEmplace<SEntityScriptComponent>(Entity).Scripts.Add(Unadopted);

    EXPECT_FALSE(Unadopted->IsAttached());

    // Detaching drops the component's only strong ref, so the counters are read through pins.
    TStrongObjectPtr<CEntityScript> PinnedUnadopted(Unadopted);
    EntityScripts::DetachAll(Registry, Entity);
    EXPECT_EQ(Unadopted->DetachCount, 0) << "no OnAttach ran, so no OnDetach is owed";

    // And the paired case still fires.
    const ECS::FEntity Adopted = Registry.Create();
    CEntityScriptTest* Attached = AttachTestScript(Registry, Adopted);
    ASSERT_NE(Attached, nullptr);
    TStrongObjectPtr<CEntityScript> PinnedAttached(Attached);
    EntityScripts::DetachAll(Registry, Adopted);
    EXPECT_EQ(Attached->DetachCount, 1);
}

// Fixed update must not reach a script that has not readied yet.
TEST(EntityScriptUnification, FixedUpdateWaitsForReady)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Entity);
    ASSERT_NE(Script, nullptr);

    EntityScripts::TickFixed(Registry, 1.0f / 60.0f);
    EXPECT_EQ(Script->FixedUpdateCount, 0) << "not ready yet; OnFixedUpdate must not have run";
    EXPECT_EQ(Script->ReadyCount, 0);

    EntityScripts::Tick(Registry, 0.1f);
    EntityScripts::TickFixed(Registry, 1.0f / 60.0f);
    EXPECT_EQ(Script->FixedUpdateCount, 1);
}

// The managed boundary marks a script faulted when its OnAttach or OnReady throws, which is simulated here.
TEST(EntityScriptUnification, AFaultedScriptStopsTickingButStillDetaches)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    CEntityScriptTest* Script = AttachTestScript(Registry, Entity);
    ASSERT_NE(Script, nullptr);
    Script->MarkFaulted();

    EntityScripts::Tick(Registry, 0.1f);
    EntityScripts::Tick(Registry, 0.1f);
    EntityScripts::TickFixed(Registry, 1.0f / 60.0f);
    EXPECT_EQ(Script->ReadyCount, 0) << "a script whose attach failed is not readied";
    EXPECT_EQ(Script->UpdateCount, 0);
    EXPECT_EQ(Script->FixedUpdateCount, 0);

    TStrongObjectPtr<CEntityScript> Pinned(Script);
    EntityScripts::DetachAll(Registry, Entity);
    EXPECT_EQ(Script->DetachCount, 1) << "whatever OnAttach managed to set up is still owed its OnDetach";
}

// Several scripts on one entity, and several scripted entities, all driven by the one loop.
TEST(EntityScriptUnification, ManyScriptsAndEntitiesTickThroughOneLoop)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity First = Registry.Create();
    const ECS::FEntity Second = Registry.Create();

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
    CScriptClass* Minted = FScriptableRegistry::Mint("EntityScript_GTestMinted", "CEntityScript");
    ASSERT_NE(Minted, nullptr) << "minting failed (is the CEntityScript shim registered?)";
    RegisterPendingTypes();
    Minted->GetDefaultObject();

    EXPECT_TRUE(Minted->IsChildOf(CEntityScript::StaticClass()))
        << "a minted C# script class must BE a CEntityScript, or the driver could not hold it";

    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    CEntityScript* Managed = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Managed, nullptr);
    CEntityScriptTest* Native = AttachTestScript(Registry, Entity);
    ASSERT_NE(Native, nullptr);

    // One loop, two kinds of subclass.
    EntityScripts::Tick(Registry, 0.1f);
    EntityScripts::Tick(Registry, 0.1f);

    EXPECT_EQ(Registry.Get<SEntityScriptComponent>(Entity).Scripts.Num(), 2);
    EXPECT_TRUE(Managed->IsReady()) << "the minted script was readied by the same pass as the C++ one";
    EXPECT_EQ(Native->UpdateCount, 2);
    EXPECT_EQ(Managed->GetClass(), Minted);

    // Detaching drops the component's only strong ref, so the counter is read through a pin.
    TStrongObjectPtr<CEntityScript> PinnedNative(Native);
    EntityScripts::DetachAll(Registry, Entity);
    EXPECT_EQ(Native->DetachCount, 1);
}

namespace
{
    CParallelEntityScriptTest* AttachParallelTestScript(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        // The ParallelUpdate promise is class metadata, which only lands once registration finishes.
        RegisterPendingTypes();
        return static_cast<CParallelEntityScriptTest*>(
            EntityScripts::Attach(Registry, Entity, CParallelEntityScriptTest::StaticClass()));
    }

    constexpr int32 kParallelTestEntities = 4096;
}

// REFLECT(ParallelUpdate) scripts run across the workers, each exactly once per pass.
TEST(EntityScriptUnification, ParallelUpdateScriptsRunOncePerPassAcrossThreads)
{
    ECS::FRegistry Registry{};
    TVector<CParallelEntityScriptTest*> Scripts;
    for (int32 Index = 0; Index < kParallelTestEntities; ++Index)
    {
        Scripts.Add(AttachParallelTestScript(Registry, Registry.Create()));
        ASSERT_NE(Scripts.Last(), nullptr);
    }

    EntityScripts::Tick(Registry, 0.5f);
    EntityScripts::Tick(Registry, 0.5f);
    EntityScripts::TickFixed(Registry, 0.5f);

    for (CParallelEntityScriptTest* Script : Scripts)
    {
        EXPECT_EQ(Script->ReadyCount, 1);
        EXPECT_EQ(Script->UpdateCount, 2) << "every promising script updates once per pass, no more and no fewer";
        EXPECT_EQ(Script->FixedUpdateCount, 1);
    }
}

// One script that made no promise keeps its whole entity on the serial path, so the pair still runs in order.
TEST(EntityScriptUnification, AnEntityWithAnUnpromisedScriptStaysSerial)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Mixed = Registry.Create();
    CParallelEntityScriptTest* Promised = AttachParallelTestScript(Registry, Mixed);
    CEntityScriptTest*         Plain    = AttachTestScript(Registry, Mixed);
    ASSERT_NE(Promised, nullptr);
    ASSERT_NE(Plain, nullptr);

    for (int32 Index = 0; Index < kParallelTestEntities; ++Index)
    {
        ASSERT_NE(AttachParallelTestScript(Registry, Registry.Create()), nullptr);
    }

    EntityScripts::Tick(Registry, 0.5f);
    EntityScripts::Tick(Registry, 0.5f);

    EXPECT_EQ(Promised->UpdateCount, 2);
    EXPECT_EQ(Plain->UpdateCount, 2);
    EXPECT_EQ(Promised->LastThreadID, Threading::GetThreadID()) << "the mixed entity ran on the ticking thread";
}

// The C# attribute reaches the driver as a flag on the minted class.
TEST(EntityScriptUnification, AMintedClassOptsInThroughItsFlag)
{
    ECS::FRegistry Registry{};
    TVector<CEntityScriptTest*> Scripts;
    for (int32 Index = 0; Index < kParallelTestEntities; ++Index)
    {
        Scripts.Add(AttachMintedTestScript(Registry, Registry.Create(), "EntityScript_GTestParallel"));
        ASSERT_NE(Scripts.Last(), nullptr);
    }

    CScriptClass* Class = ToScriptClass(Scripts[0]->GetClass());
    ASSERT_NE(Class, nullptr);
    const FName ParallelUpdateName("ParallelUpdate");
    Class->Metadata.SetValue(ParallelUpdateName, FString());

    EntityScripts::Tick(Registry, 0.5f);
    EntityScripts::Tick(Registry, 0.5f);

    Class->Metadata.RemoveValue(ParallelUpdateName);
    for (CEntityScriptTest* Script : Scripts)
    {
        EXPECT_EQ(Script->UpdateCount, 2);
    }
}

// Priority, not attach order, decides which of an entity's scripts updates first, a C# one and a C++ one alike, on either path.
TEST(EntityScriptUnification, UpdatePriorityOrdersScriptsAcrossLanguagesAndPaths)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    CEntityScriptTest*         Managed = AttachMintedTestScript(Registry, Entity, "EntityScript_GTestPriority");
    CParallelEntityScriptTest* Native  = AttachParallelTestScript(Registry, Entity);
    ASSERT_NE(Managed, nullptr);
    ASSERT_NE(Native, nullptr);

    // The native script reads how far the managed one got, so a count already bumped this tick means the managed one ran first.
    struct FProbe { const CEntityScriptTest* Managed; int32 Seen; } Probe{ Managed, -1 };
    Native->HookContext = &Probe;
    Native->UpdateHook  = [](CParallelEntityScriptTest&, void* Context)
    {
        FProbe& Seen = *static_cast<FProbe*>(Context);
        Seen.Seen = Seen.Managed->UpdateCount;
    };
    auto ManagedRanFirst = [&]
    {
        EntityScripts::Tick(Registry, 0.1f);
        return Probe.Seen == Managed->UpdateCount;
    };

    EXPECT_TRUE(ManagedRanFirst()) << "equal priorities keep attach order";

    Managed->SetUpdatePriority(1);
    Native->SetUpdatePriority(-1);
    EXPECT_FALSE(ManagedRanFirst()) << "the lower priority runs first, though it attached second";

    Managed->SetUpdatePriority(-2);
    EXPECT_TRUE(ManagedRanFirst()) << "a priority changed after attach reorders the next pass";

    // Promising parallel safety moves the entity onto the claimed span, which must keep the same order.
    CScriptClass* Class = ToScriptClass(Managed->GetClass());
    ASSERT_NE(Class, nullptr);
    const FName ParallelUpdateName("ParallelUpdate");
    Class->Metadata.SetValue(ParallelUpdateName, FString());
    EntityScripts::NoteStructureChange();

    EXPECT_TRUE(ManagedRanFirst()) << "the claimed span runs in priority order";
    Managed->SetUpdatePriority(1);
    EXPECT_FALSE(ManagedRanFirst()) << "and follows a priority change the same way";

    Class->Metadata.RemoveValue(ParallelUpdateName);
    EntityScripts::NoteStructureChange();
    EXPECT_EQ(Native->UpdateCount, Managed->UpdateCount);
}

// Breaking the promise mid-pass cannot skip or repeat anyone, because each chunk's remainder runs serially.
TEST(EntityScriptUnification, AStructuralChangeMidPassHandsTheRestToTheSerialPath)
{
    ECS::FRegistry Registry{};
    TVector<CParallelEntityScriptTest*> Scripts;
    for (int32 Index = 0; Index < kParallelTestEntities; ++Index)
    {
        Scripts.Add(AttachParallelTestScript(Registry, Registry.Create()));
        ASSERT_NE(Scripts.Last(), nullptr);
    }
    EntityScripts::Tick(Registry, 0.5f);

    Scripts[kParallelTestEntities / 2]->UpdateHook = [](CParallelEntityScriptTest&, void*)
    {
        EntityScripts::NoteStructureChange();
    };
    EntityScripts::Tick(Registry, 0.5f);

    for (CParallelEntityScriptTest* Script : Scripts)
    {
        EXPECT_EQ(Script->UpdateCount, 2) << "every script still updates exactly once in the interrupted pass";
    }
}

#if !defined(LE_SHIPPING)
extern "C" void* LuminaSharp_GetComponent(uint64 World, uint32 Entity, const void* Ops);
extern "C" const void* LuminaSharp_GetComponentRead(uint64 World, uint32 Entity, const void* Ops);

namespace
{
    struct FPromiseTest
    {
        ECS::FRegistry Registry;
        ECS::FEntity   Bystander = ECS::NullEntity;
    };

    template<typename TTick>
    int32 CountPromiseReportsDuring(const char* Call, TTick&& Tick)
    {
        // The log backend is asynchronous, so drain an earlier test's report before clearing or it lands in this pass.
        Logging::Flush();
        Logging::ClearLogQueue();
        Tick();

        return CountLoggedMessages(ELogLevel::Error, "CParallelEntityScriptTest", Call);
    }

    // Few enough entities that the pass runs on the ticking thread, so a hook may change the registry without racing anyone.
    int32 CountPromiseReports(void (*Hook)(CParallelEntityScriptTest&, void*), const char* Call)
    {
        FPromiseTest Test;
        ConnectComponentAccessValidators(Test.Registry);
        Test.Bystander = Test.Registry.Create();
        for (int32 Index = 0; Index < 64; ++Index)
        {
            CParallelEntityScriptTest* Script = AttachParallelTestScript(Test.Registry, Test.Registry.Create());
            Script->UpdateHook  = Hook;
            Script->HookContext = &Test;
        }

        return CountPromiseReportsDuring(Call, [&]
        {
            EntityScripts::Tick(Test.Registry, 0.5f);
            EntityScripts::Tick(Test.Registry, 0.5f);
        });
    }
}

TEST(EntityScriptUnification, AParallelScriptTouchingOnlyItsOwnEntityIsNotReported)
{
    EXPECT_EQ(CountPromiseReports([](CParallelEntityScriptTest& Script, void*)
    {
        LuminaSharp_GetComponent(0, Script.GetOwningEntity().GetPacked(), nullptr);
        LuminaSharp_GetComponentRead(0, Script.GetOwningEntity().GetPacked(), nullptr);
    }, "ParallelUpdate"), 0);
}

TEST(EntityScriptUnification, AParallelScriptReadingAnotherEntityIsReportedOnce)
{
    EXPECT_EQ(CountPromiseReports([](CParallelEntityScriptTest&, void* Context)
    {
        LuminaSharp_GetComponentRead(0, static_cast<FPromiseTest*>(Context)->Bystander.GetPacked(), nullptr);
    }, "GetComponentRead"), 1);
}

TEST(EntityScriptUnification, AParallelScriptWritingAnotherEntityIsReportedOnce)
{
    EXPECT_EQ(CountPromiseReports([](CParallelEntityScriptTest&, void* Context)
    {
        LuminaSharp_GetComponent(0, static_cast<FPromiseTest*>(Context)->Bystander.GetPacked(), nullptr);
    }, "GetComponent"), 1);
}

TEST(EntityScriptUnification, AParallelScriptCreatingAnEntityIsReportedOnce)
{
    EXPECT_EQ(CountPromiseReports([](CParallelEntityScriptTest&, void* Context)
    {
        (void)static_cast<FPromiseTest*>(Context)->Registry.Create();
    }, "creating or destroying an entity"), 1);
}

TEST(EntityScriptUnification, AParallelScriptDestroyingAnEntityIsReportedOnce)
{
    EXPECT_EQ(CountPromiseReports([](CParallelEntityScriptTest&, void* Context)
    {
        FPromiseTest& Test = *static_cast<FPromiseTest*>(Context);
        if (Test.Registry.IsValid(Test.Bystander))
        {
            Test.Registry.Destroy(Test.Bystander);
        }
    }, "creating or destroying an entity"), 1);
}

TEST(EntityScriptUnification, AParallelScriptAttachingAScriptIsReportedOnce)
{
    EXPECT_EQ(CountPromiseReports([](CParallelEntityScriptTest&, void* Context)
    {
        FPromiseTest& Test = *static_cast<FPromiseTest*>(Context);
        AttachTestScript(Test.Registry, Test.Bystander);
    }, "attaching, detaching or destroying a script"), 1);
}
#endif

namespace
{
    // Hides the promise from anything that walks the class again, so whether a tick ran in parallel shows whether it re-claimed.
    struct FHiddenParallelPromise
    {
        FHiddenParallelPromise()  { CParallelEntityScriptTest::StaticClass()->Metadata.RemoveValue(FName("ParallelUpdate")); }
        ~FHiddenParallelPromise() { CParallelEntityScriptTest::StaticClass()->Metadata.SetValue(FName("ParallelUpdate"), FString()); }
    };

    TVector<CParallelEntityScriptTest*> AttachReadiedParallelScripts(ECS::FRegistry& Registry, int32 Count)
    {
        TVector<CParallelEntityScriptTest*> Scripts;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Scripts.Add(AttachParallelTestScript(Registry, Registry.Create()));
        }
        EntityScripts::Tick(Registry, 0.5f);
        return Scripts;
    }

    bool AllRanOnThisThread(const TVector<CParallelEntityScriptTest*>& Scripts)
    {
        return Algo::AllOf(Scripts, [](const CParallelEntityScriptTest* Script) { return Script->LastThreadID == Threading::GetThreadID(); });
    }
}

extern "C" void* LuminaSharp_EmplaceComponent(uint64 World, uint32 Entity, const void* Ops);

// GetOrEmplaceComponent from a worker leaves the live pool to its sibling chunks and adds the configured value at the sync point.
TEST(EntityScriptUnification, AParallelScriptEmplaceWaitsForTheSyncPoint)
{
    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const TVector<CParallelEntityScriptTest*> Scripts = AttachReadiedParallelScripts(Registry, kParallelTestEntities);
    for (CParallelEntityScriptTest* Script : Scripts)
    {
        Script->HookContext = World;
        Script->UpdateHook  = [](CParallelEntityScriptTest& Self, void* Context)
        {
            // The ticking thread's own chunks run undeferred, so only a worker's call is under test.
            if (Threading::IsMainThread())
            {
                return;
            }
            const uint32 Entity = Self.GetOwningEntity().GetPacked();
            void* Added = LuminaSharp_EmplaceComponent(reinterpret_cast<uint64>(Context), Entity, &Meta::GetComponentOps<SLifetimeComponent>());
            static_cast<SLifetimeComponent*>(Added)->Lifetime = static_cast<float>(Entity);
        };
    }

    EntityScripts::Tick(Registry, 0.5f);
    TVector<CParallelEntityScriptTest*> RanOnWorkers;
    for (CParallelEntityScriptTest* Script : Scripts)
    {
        if (Script->LastThreadID != Threading::GetThreadID())
        {
            RanOnWorkers.Add(Script);
            EXPECT_FALSE(Registry.HasAll<SLifetimeComponent>(Script->GetOwningEntity())) << "a worker's emplace must not touch the live pool";
        }
    }
    ASSERT_FALSE(RanOnWorkers.IsEmpty()) << "no script ran on a worker, so nothing was deferred";

    World->GetCommandBus().Flush();
    for (CParallelEntityScriptTest* Script : RanOnWorkers)
    {
        const SLifetimeComponent* Lifetime = Registry.TryGet<SLifetimeComponent>(Script->GetOwningEntity());
        ASSERT_NE(Lifetime, nullptr) << "the flush must add what the script staged";
        EXPECT_EQ(Lifetime->Lifetime, static_cast<float>(Script->GetOwningEntity().GetPacked())) << "the staged value carries what the script configured";
    }
}

extern "C" void* LuminaSharp_RegistryConnect(uint64 World, const void* Ops, int32 Kind);
extern "C" void LuminaSharp_RegistryDisconnect(uint64 World, const void* Ops, int32 Kind, void* Handle);

// Subscribing or unsubscribing writes the pool's shared listener list, so a worker's call waits for the sync point.
TEST(EntityScriptUnification, AParallelScriptSignalSubscriptionWaitsForTheSyncPoint)
{
    struct FSubscription
    {
        CWorld*                 World         = nullptr;
        FManagedSignalListener* Listener      = nullptr;
        bool                    bUnsubscribed = false;
    };
    constexpr int32 Construct = static_cast<int32>(EComponentSignal::Construct);
    const FComponentOps* Ops = &Meta::GetComponentOps<SLifetimeComponent>();

    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const TVector<CParallelEntityScriptTest*> Scripts = AttachReadiedParallelScripts(Registry, kParallelTestEntities);
    TVector<FSubscription> Subscriptions;
    Subscriptions.SetNum(Scripts.Num());
    for (int32 Index = 0; Index < Scripts.Num(); ++Index)
    {
        Subscriptions[Index].World = World;
        Scripts[Index]->HookContext = &Subscriptions[Index];
        Scripts[Index]->UpdateHook  = [](CParallelEntityScriptTest&, void* Context)
        {
            FSubscription& Subscription = *static_cast<FSubscription*>(Context);
            if (!Threading::IsMainThread())
            {
                Subscription.Listener = static_cast<FManagedSignalListener*>(LuminaSharp_RegistryConnect(
                    reinterpret_cast<uint64>(Subscription.World), &Meta::GetComponentOps<SLifetimeComponent>(), Construct));
            }
        };
    }

    EntityScripts::Tick(Registry, 0.5f);
    TVector<FSubscription*> Subscribed;
    for (FSubscription& Subscription : Subscriptions)
    {
        if (Subscription.Listener != nullptr)
        {
            Subscribed.Add(&Subscription);
            EXPECT_FALSE(Subscription.Listener->Connection.IsConnected()) << "a worker's subscribe must not touch the live pool";
        }
    }
    ASSERT_FALSE(Subscribed.IsEmpty()) << "no script ran on a worker, so nothing was deferred";

    World->GetCommandBus().Flush();
    for (const FSubscription* Subscription : Subscribed)
    {
        EXPECT_TRUE(Subscription->Listener->Connection.IsConnected()) << "the flush must connect what the script subscribed";
    }

    for (CParallelEntityScriptTest* Script : Scripts)
    {
        Script->UpdateHook = [](CParallelEntityScriptTest&, void* Context)
        {
            FSubscription& Subscription = *static_cast<FSubscription*>(Context);
            if (!Threading::IsMainThread() && Subscription.Listener != nullptr)
            {
                LuminaSharp_RegistryDisconnect(reinterpret_cast<uint64>(Subscription.World), &Meta::GetComponentOps<SLifetimeComponent>(),
                    Construct, Subscription.Listener);
                Subscription.bUnsubscribed = true;
            }
        };
    }

    EntityScripts::Tick(Registry, 0.5f);
    bool bAnyUnsubscribed = false;
    for (const FSubscription* Subscription : Subscribed)
    {
        if (Subscription->bUnsubscribed)
        {
            bAnyUnsubscribed = true;
            EXPECT_TRUE(Subscription->Listener->Connection.IsConnected()) << "a worker's unsubscribe must not touch the live pool or free the listener";
        }
    }
    ASSERT_TRUE(bAnyUnsubscribed) << "no subscribed script ran on a worker, so nothing was deferred";

    World->GetCommandBus().Flush();
    for (const FSubscription* Subscription : Subscribed)
    {
        if (!Subscription->bUnsubscribed)
        {
            LuminaSharp_RegistryDisconnect(reinterpret_cast<uint64>(World), Ops, Construct, Subscription->Listener);
        }
    }
    const ECS::FSparseSet* Pool = Registry.FindStorage(Ops->GetTypeID());
    ASSERT_NE(Pool, nullptr);
    EXPECT_TRUE(Pool->Signals.OnConstruct.IsEmpty()) << "every subscription was released once its unsubscribe flushed";
}

#if !defined(LE_SHIPPING)
// With nothing structural changed the next tick runs the claim it already has, without walking the entities again.
TEST(EntityScriptUnification, AnUnchangedWorldReusesItsParallelClaim)
{
    ECS::FRegistry Registry{};
    const TVector<CParallelEntityScriptTest*> Scripts = AttachReadiedParallelScripts(Registry, kParallelTestEntities);
    EntityScripts::Tick(Registry, 0.5f);
    EntityScripts::TickFixed(Registry, 0.5f);
    for (CParallelEntityScriptTest* Script : Scripts)
    {
        Script->UpdateHook = [](CParallelEntityScriptTest&, void*)
        {
            LuminaSharp_GetComponentRead(0, ECS::NullEntity.GetPacked(), nullptr);
        };
    }

    int32 Reports = 0;
    {
        FHiddenParallelPromise Hidden;
        EntityScripts::TickFixed(Registry, 0.5f);
        Reports = CountPromiseReportsDuring("GetComponentRead", [&] { EntityScripts::Tick(Registry, 0.5f); });
    }

    for (CParallelEntityScriptTest* Script : Scripts)
    {
        EXPECT_EQ(Script->UpdateCount, 3);
        EXPECT_EQ(Script->FixedUpdateCount, 2);
    }
    EXPECT_EQ(Reports, 1) << "only the claimed span watches promises, and a re-walk would have found none and run everything serially";
}
#endif

// A structural change throws the cached claim away, so the next tick claims from what the entities hold now.
TEST(EntityScriptUnification, AStructuralChangeInvalidatesTheCachedClaim)
{
    ECS::FRegistry Registry{};
    const TVector<CParallelEntityScriptTest*> Scripts = AttachReadiedParallelScripts(Registry, kParallelTestEntities);
    EntityScripts::Tick(Registry, 0.5f);
    {
        FHiddenParallelPromise Hidden;
        EntityScripts::NoteStructureChange();
        EntityScripts::Tick(Registry, 0.5f);
    }

    EXPECT_TRUE(AllRanOnThisThread(Scripts)) << "the re-claim saw no promise, so every script ran on the ticking thread";
    for (CParallelEntityScriptTest* Script : Scripts)
    {
        EXPECT_EQ(Script->UpdateCount, 3);
    }
}

// Entities that come, go or pause after a claim are seen by the next tick, though only the attach moves the script epoch.
TEST(EntityScriptUnification, TheCachedClaimFollowsSpawnsDestroysAndPauses)
{
    ECS::FRegistry Registry{};
    const TVector<CParallelEntityScriptTest*> Scripts = AttachReadiedParallelScripts(Registry, kParallelTestEntities);
    EntityScripts::Tick(Registry, 0.5f);

    CParallelEntityScriptTest* Paused = Scripts[1];
    Registry.Emplace<SScriptDisabledTag>(Paused->GetOwningEntity());
    Registry.Destroy(Scripts[0]->GetOwningEntity());
    CParallelEntityScriptTest* Spawned = AttachParallelTestScript(Registry, Registry.Create());
    EntityScripts::Tick(Registry, 0.5f);

    EXPECT_EQ(Paused->UpdateCount, 2) << "a paused entity's scripts must drop out of the claim";
    EXPECT_EQ(Spawned->ReadyCount, 1);
    EXPECT_EQ(Spawned->UpdateCount, 1);
    for (int32 Index = 2; Index < Scripts.Num(); ++Index)
    {
        EXPECT_EQ(Scripts[Index]->UpdateCount, 3);
    }

    Registry.Remove<SScriptDisabledTag>(Paused->GetOwningEntity());
    EntityScripts::Tick(Registry, 0.5f);
    EXPECT_EQ(Paused->UpdateCount, 3) << "resuming it must bring it back";
}

// Not a pass or fail bound, as wall-clock time under load is noise. Prints a whole tick with the claim rebuilt and cached.
TEST(EntityScriptUnification, ClaimTimingWithAndWithoutTheCache)
{
    constexpr int32 kEntities = 20000;
    constexpr int32 kTicks    = 50;

    ECS::FRegistry Registry{};
    AttachReadiedParallelScripts(Registry, kEntities);

    auto MicrosPerCall = [&](bool bInvalidate)
    {
        const auto Start = std::chrono::steady_clock::now();
        for (int32 Tick = 0; Tick < kTicks; ++Tick)
        {
            if (bInvalidate)
            {
                EntityScripts::NoteStructureChange();
            }
            EntityScripts::Tick(Registry, 0.5f);
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count() / kTicks;
    };

    const double Rebuilt = MicrosPerCall(true);
    const double Cached  = MicrosPerCall(false);
    std::printf("[ClaimTiming] %d entities, tick with claim rebuilt %.1f us, cached %.1f us\n", kEntities, Rebuilt, Cached);
}

// A class that is not a CEntityScript must be refused rather than attached and later dispatched to.
TEST(EntityScriptUnification, AttachRefusesANonScriptClass)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    EXPECT_EQ(EntityScripts::Attach(Registry, Entity, CScriptableTest::StaticClass()), nullptr);
    EXPECT_EQ(EntityScripts::Attach(Registry, Entity, nullptr), nullptr);
}

// A script is a per-entity SUBOBJECT, serialized by class name plus its tagged properties.
TEST(EntityScriptUnification, ComponentSerializationRoundTripsScriptsAndAdoptsThemOnTick)
{
    ECS::FRegistry Source{};
    const ECS::FEntity Entity = Source.Create();

    CEntityScriptTest* A = AttachTestScript(Source, Entity);
    CEntityScriptTest* B = AttachTestScript(Source, Entity);
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Source.Get<SEntityScriptComponent>(Entity).Serialize(Ar);
    }
    ASSERT_FALSE(Bytes.IsEmpty());

    // Load into a fresh registry + entity, exactly as scene load would.
    ECS::FRegistry Loaded{};
    const ECS::FEntity LoadedEntity = Loaded.Create();
    SEntityScriptComponent& Restored = Loaded.Emplace<SEntityScriptComponent>(LoadedEntity);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Restored.Serialize(Ar);
    }

    ASSERT_EQ(Restored.Scripts.Num(), 2) << "both scripts must come back";
    ASSERT_NE(Restored.Scripts[0].Get(), nullptr);
    EXPECT_EQ(Restored.Scripts[0].Get()->GetClass(), CEntityScriptTest::StaticClass())
        << "the script's concrete class must round-trip, not its base";

    // Freshly loaded, so no owner yet and no lifecycle has run.
    EXPECT_EQ(Restored.Scripts[0].Get()->GetOwningEntity(), ECS::FEntity{ECS::NullEntity});
    EXPECT_FALSE(Restored.Scripts[0].Get()->IsReady());

    EntityScripts::Tick(Loaded, 0.1f);

    for (TStrongObjectPtr<CEntityScript>& Held : Loaded.Get<SEntityScriptComponent>(LoadedEntity).Scripts)
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
        Scripting::FScriptExportField& Speed = Schema.Fields.EmplaceGetRef();
        Speed.Name = FName("Speed");
        Speed.Type = MakeShared<Scripting::FScriptExportType>();
        Speed.Type->Kind = EPropertyTypeFlags::Float;

        Scripting::FScriptExportField& Label = Schema.Fields.EmplaceGetRef();
        Label.Name = FName("Label");
        Label.Type = MakeShared<Scripting::FScriptExportType>();
        Label.Type->Kind = EPropertyTypeFlags::String;
    }

    CScriptClass* Minted = FScriptableRegistry::Mint("EntityScript_GTestValues", "CEntityScript");
    ASSERT_NE(Minted, nullptr);
    ASSERT_GT(Scripting::AppendScriptPropertiesToClass(Minted, Schema), 0u);
    RegisterPendingTypes();
    Minted->GetDefaultObject();

    FProperty* Speed = Minted->GetProperty(FName("Speed"));
    FProperty* Label = Minted->GetProperty(FName("Label"));
    ASSERT_NE(Speed, nullptr);
    ASSERT_NE(Label, nullptr);

    ECS::FRegistry Source{};
    const ECS::FEntity Entity = Source.Create();
    CEntityScript* Script = EntityScripts::Attach(Source, Entity, Minted);
    ASSERT_NE(Script, nullptr);

    // Stand-in for the inspector, writing through exactly these FPropertys at this object.
    Speed->SetValue<float>(Script, 12.5f);
    *Label->GetValuePtr<FString>(Script) = "authored in the inspector";

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Source.Get<SEntityScriptComponent>(Entity).Serialize(Ar);
    }
    ASSERT_FALSE(Bytes.IsEmpty());

    ECS::FRegistry Loaded{};
    const ECS::FEntity LoadedEntity = Loaded.Create();
    SEntityScriptComponent& Restored = Loaded.Emplace<SEntityScriptComponent>(LoadedEntity);
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Restored.Serialize(Ar);
    }

    ASSERT_EQ(Restored.Scripts.Num(), 1);
    CEntityScript* LoadedScript = Restored.Scripts[0].Get();
    ASSERT_NE(LoadedScript, nullptr);
    ASSERT_EQ(LoadedScript->GetClass(), Minted);
    EXPECT_NE(LoadedScript, Script) << "the round-trip must produce a NEW object, not alias the source";

    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(LoadedScript), 12.5f)
        << "an authored scalar reverted; it would silently reset on every Play";
    EXPECT_STREQ(*(*Label->GetValuePtr<FString>(LoadedScript)), "authored in the inspector")
        << "an authored string reverted";

    // And the values survive the driver adopting the loaded script, which runs OnAttach/OnReady on it.
    EntityScripts::Tick(Loaded, 0.1f);
    EXPECT_FLOAT_EQ(*Speed->GetValuePtr<float>(LoadedScript), 12.5f)
        << "adoption (OnAttach/OnReady) clobbered an authored value";
}

// Class-based lookup, so a C++ script is found by exactly the same call a C# one is.
TEST(EntityScriptUnification, FindAndRemoveByClass)
{
    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

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
    EXPECT_EQ(All.Num(), 2);

    // Held, since Remove drops the component's reference and the reads below would otherwise touch a freed script.
    const TStrongObjectPtr<CEntityScriptTest> PinnedFirst(First);
    EXPECT_TRUE(EntityScripts::Remove(Registry, Entity, First));
    EXPECT_EQ(First->DetachCount, 1) << "Remove must run OnDetach";
    EXPECT_EQ(Registry.Get<SEntityScriptComponent>(Entity).Scripts.Num(), 1);
    EXPECT_EQ(EntityScripts::Find(Registry, Entity, CEntityScriptTest::StaticClass()), Second);

    EXPECT_FALSE(EntityScripts::Remove(Registry, Entity, First)) << "removing twice must be refused";
}

// The reinstancer carries attached scripts across, so no holder has to empty itself first.
TEST(EntityScriptUnification, ScriptsSurviveAClassLayoutRebuild)
{
    ECS::FRegistry Registry{};

    Scripting::FScriptExportSchema Before;
    {
        Scripting::FScriptExportField Speed;
        Speed.Name = FName("Speed");
        Speed.Type = MakeShared<Scripting::FScriptExportType>();
        Speed.Type->Kind = EPropertyTypeFlags::Float;
        Before.Fields.Add(std::move(Speed));
    }

    CScriptClass* Minted = FScriptableRegistry::Mint("EvacTest_Script", "CEntityScript");
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Before);
    RegisterPendingTypes();
    Minted->GetDefaultObject();

    ECS::FEntity Entity = Registry.Create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);

    FProperty* Speed = Minted->GetProperty(FName("Speed"));
    ASSERT_NE(Speed, nullptr);
    Speed->SetValue<float>(Attached, 12.5f);

    // Identity by GUID, not by address, since the pooled allocator may reuse the block.
    const FGuid AttachedGuid = Attached->GetGUID();

    Scripting::FScriptExportSchema After;
    {
        Scripting::FScriptExportField Kept;
        Kept.Name = FName("Speed");
        Kept.Type = MakeShared<Scripting::FScriptExportType>();
        Kept.Type->Kind = EPropertyTypeFlags::Float;
        After.Fields.Add(std::move(Kept));

        Scripting::FScriptExportField Added;
        Added.Name = FName("Health");
        Added.Type = MakeShared<Scripting::FScriptExportType>();
        Added.Type->Kind = EPropertyTypeFlags::Int32;
        After.Fields.Add(std::move(Added));
    }

    // A local registry is not reachable from GWorldManager, so it takes part the same way any holder does.
    FLocalRegistryReferenceProvider Provider{ Registry };
    FObjectReferenceProviders::Register(&Provider);
    Minted = ScriptTest::Reshape(Minted, "EvacTest_Script_V2", After, "CEntityScript");
    FObjectReferenceProviders::Unregister(&Provider);
    ASSERT_NE(Minted, nullptr);

    SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
    ASSERT_NE(Component, nullptr);
    ASSERT_EQ(Component->Scripts.Num(), 1) << "the attached script did not survive the reshape";

    CEntityScript* Restored = Component->Scripts[0].Get();
    ASSERT_NE(Restored, nullptr);
    EXPECT_EQ(Restored->GetClass(), Minted);
    EXPECT_EQ(Restored->GetGUID(), AttachedGuid) << "identity must follow the instance onto the replacement";

    // The authored value rode across the reshape, keyed by name.
    FProperty* NewSpeed = Minted->GetProperty(FName("Speed"));
    ASSERT_NE(NewSpeed, nullptr);
    EXPECT_FLOAT_EQ(*NewSpeed->GetValuePtr<float>(Restored), 12.5f) << "the authored value was lost";

    // And the property added by this reload exists on the carried instance, at its default.
    FProperty* Health = Minted->GetProperty(FName("Health"));
    ASSERT_NE(Health, nullptr) << "the added property is missing from the carried instance";
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
    ECS::FRegistry Registry{};

    Scripting::FScriptExportSchema Before;
    Before.Fields.Add(MakeReloadField("Speed", EPropertyTypeFlags::Float));

    CScriptClass* Minted = FScriptableRegistry::Mint("RenameProp_Script", "CEntityScript");
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Before);
    RegisterPendingTypes();
    Minted->GetDefaultObject();

    const ECS::FEntity Entity = Registry.Create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);
    Minted->GetProperty(FName("Speed"))->SetValue<float>(Attached, 9.75f);

    // Speed -> Velocity, declaring the old name.
    Scripting::FScriptExportSchema After;
    After.Fields.Add(MakeReloadField("Velocity", EPropertyTypeFlags::Float, "Speed"));

    FLocalRegistryReferenceProvider Provider{ Registry };
    FObjectReferenceProviders::Register(&Provider);
    Minted = ScriptTest::Reshape(Minted, "RenameProp_Script_V2", After, "CEntityScript");
    FObjectReferenceProviders::Unregister(&Provider);
    ASSERT_NE(Minted, nullptr);
    ASSERT_EQ(Minted->GetProperty(FName("Speed")), nullptr);

    SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
    ASSERT_NE(Component, nullptr);
    ASSERT_EQ(Component->Scripts.Num(), 1);
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
    ECS::FRegistry Registry{};

    Scripting::FScriptExportSchema Before;
    Before.Fields.Add(MakeReloadField("Speed", EPropertyTypeFlags::Float));

    CScriptClass* Minted = FScriptableRegistry::Mint("RenameNoAlias_Script", "CEntityScript");
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Before);
    RegisterPendingTypes();
    Minted->GetDefaultObject();

    const ECS::FEntity Entity = Registry.Create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);
    Minted->GetProperty(FName("Speed"))->SetValue<float>(Attached, 9.75f);

    Scripting::FScriptExportSchema After;
    After.Fields.Add(MakeReloadField("Velocity", EPropertyTypeFlags::Float));   // no alias

    FLocalRegistryReferenceProvider Provider{ Registry };
    FObjectReferenceProviders::Register(&Provider);
    Minted = ScriptTest::Reshape(Minted, "RenameNoAlias_Script_V2", After, "CEntityScript");
    FObjectReferenceProviders::Unregister(&Provider);
    ASSERT_NE(Minted, nullptr);

    CEntityScript* Restored = Registry.Get<SEntityScriptComponent>(Entity).Scripts[0].Get();
    ASSERT_NE(Restored, nullptr);
    EXPECT_FLOAT_EQ(*Minted->GetProperty(FName("Velocity"))->GetValuePtr<float>(Restored), 0.0f);

    EntityScripts::DetachAll(Registry, Entity);
}

// The instances are the wrong class, so a class-name redirect joins the evacuate round trip.
TEST(EntityScriptUnification, RenamingAScriptClassMovesItsInstances)
{
    ECS::FRegistry Registry{};

    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(MakeReloadField("Speed", EPropertyTypeFlags::Float));

    CScriptClass* Old = FScriptableRegistry::Mint("RenameClass_Before", "CEntityScript");
    ASSERT_NE(Old, nullptr);
    Scripting::AppendScriptPropertiesToClass(Old, Schema);
    RegisterPendingTypes();
    Old->GetDefaultObject();

    const ECS::FEntity Entity = Registry.Create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Old);
    ASSERT_NE(Attached, nullptr);
    Old->GetProperty(FName("Speed"))->SetValue<float>(Attached, 4.25f);

    // Evacuate, where the buffer records the OLD class name.
    TVector<uint8> Bytes;
    {
        SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Component->Serialize(Ar);
        Component->Scripts.Reset();
    }

    // The reload brings up the renamed type and registers where the old name went.
    CScriptClass* New = FScriptableRegistry::Mint("RenameClass_After", "CEntityScript");
    ASSERT_NE(New, nullptr);
    Scripting::AppendScriptPropertiesToClass(New, Schema);
    RegisterPendingTypes();
    New->GetDefaultObject();
    FScriptableRegistry::RegisterClassRedirect(FName("RenameClass_Before"), FName("RenameClass_After"));

    EXPECT_EQ(FScriptableRegistry::ResolveClass(FName("RenameClass_Before")), New);
    EXPECT_EQ(FScriptableRegistry::ResolveClass(FName("RenameClass_After")), New);
    EXPECT_EQ(FScriptableRegistry::ResolveClass(FName("NeverExisted_Script")), nullptr);

    // And the class the reload should move across is reported for evacuation.
    THashSet<CClass*> Renamed;
    FScriptableRegistry::GatherRenamedClasses(Renamed);
    EXPECT_TRUE(Renamed.Contains(Old)) << "the renamed class was not offered for evacuation";

    {
        SEntityScriptComponent& Component = Registry.GetOrEmplace<SEntityScriptComponent>(Entity);
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Component.Serialize(Ar);
    }

    SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
    ASSERT_NE(Component, nullptr);
    ASSERT_EQ(Component->Scripts.Num(), 1) << "the renamed script did not come back";

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
    ECS::FRegistry Registry{};

    Scripting::FScriptExportSchema Schema;
    Schema.Fields.Add(MakeReloadField("Kept", EPropertyTypeFlags::Float));
    {
        Scripting::FScriptExportField Scratch = MakeReloadField("Scratch", EPropertyTypeFlags::Float);
        Scratch.Meta.Set(FName("SkipHotReload"), FString());
        Schema.Fields.Add(std::move(Scratch));
    }

    CScriptClass* Minted = FScriptableRegistry::Mint("SkipHotReload_Script", "CEntityScript");
    ASSERT_NE(Minted, nullptr);
    Scripting::AppendScriptPropertiesToClass(Minted, Schema);
    RegisterPendingTypes();
    Minted->GetDefaultObject();

    const ECS::FEntity Entity = Registry.Create();
    CEntityScript* Attached = EntityScripts::Attach(Registry, Entity, Minted);
    ASSERT_NE(Attached, nullptr);
    Minted->GetProperty(FName("Kept"))->SetValue<float>(Attached, 3.5f);
    Minted->GetProperty(FName("Scratch"))->SetValue<float>(Attached, 99.0f);

    TVector<uint8> Bytes;
    {
        SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Component->Serialize(Ar);
        Component->Scripts.Reset();
    }

    // A unit test has no world contexts to walk, so replay and reset are driven directly.
    {
        SEntityScriptComponent& Component = Registry.GetOrEmplace<SEntityScriptComponent>(Entity);
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Component.Serialize(Ar);
        for (const TStrongObjectPtr<CEntityScript>& Held : Component.Scripts)
        {
            Scripting::ResetSkipHotReloadProperties(Held.Get());
        }
    }

    CEntityScript* Restored = Registry.Get<SEntityScriptComponent>(Entity).Scripts[0].Get();
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
        ECS::FRegistry* Registry = nullptr;
        ECS::FEntity     Spawned  = ECS::NullEntity;
    };
}

// World teardown detaches over a snapshot, so an OnDetach touching the pool cannot invalidate the walk.
TEST(EntityScriptUnification, DetachAllInRegistrySurvivesAnOnDetachThatAttachesScripts)
{
    ECS::FRegistry Registry{};

    const ECS::FEntity First  = Registry.Create();
    const ECS::FEntity Second = Registry.Create();
    const ECS::FEntity Third  = Registry.Create();

    CEntityScriptTest* FirstScript  = AttachTestScript(Registry, First);
    CEntityScriptTest* SecondScript = AttachTestScript(Registry, Second);
    CEntityScriptTest* ThirdScript  = AttachTestScript(Registry, Third);
    ASSERT_NE(FirstScript, nullptr);
    ASSERT_NE(SecondScript, nullptr);
    ASSERT_NE(ThirdScript, nullptr);

    // Detaching drops the component's only strong ref, so the counters are read through pins, not raw.
    TStrongObjectPtr<CEntityScript> PinFirst(FirstScript);
    TStrongObjectPtr<CEntityScript> PinSecond(SecondScript);
    TStrongObjectPtr<CEntityScript> PinThird(ThirdScript);

    FDetachMutationContext Context;
    Context.Registry = &Registry;

    // Attaching mid-walk grows the very pool a view-based loop would still be iterating.
    FirstScript->HookContext = &Context;
    FirstScript->DetachHook = [](CEntityScriptTest&, void* Ctx)
    {
        FDetachMutationContext& Mutation = *static_cast<FDetachMutationContext*>(Ctx);
        Mutation.Spawned = Mutation.Registry->Create();
        EntityScripts::Attach(*Mutation.Registry, Mutation.Spawned, CEntityScriptTest::StaticClass());
    };

    EntityScripts::DetachAllInRegistry(Registry);

    EXPECT_EQ(FirstScript->DetachCount, 1);
    EXPECT_EQ(SecondScript->DetachCount, 1) << "every entity in the snapshot is detached";
    EXPECT_EQ(ThirdScript->DetachCount, 1) << "a mutation mid-walk must not cut the pass short";

    EXPECT_TRUE(Registry.Get<SEntityScriptComponent>(First).Scripts.IsEmpty());
    EXPECT_TRUE(Registry.Get<SEntityScriptComponent>(Second).Scripts.IsEmpty());
    EXPECT_TRUE(Registry.Get<SEntityScriptComponent>(Third).Scripts.IsEmpty());

    ASSERT_TRUE(Context.Spawned != ECS::NullEntity);
    EXPECT_FALSE(Registry.Get<SEntityScriptComponent>(Context.Spawned).Scripts.IsEmpty())
        << "a script attached during the pass is outside the snapshot; the clear that follows drops it";
}

namespace
{
    struct FSpawnChain
    {
        ECS::FRegistry*            Registry = nullptr;
        int32                      Length   = 0;
        TVector<CEntityScriptTest*> Links;

        CEntityScriptTest* SpawnLink()
        {
            CEntityScriptTest* Link = AttachTestScript(*Registry, Registry->Create());
            Link->HookContext = this;
            Link->ReadyHook = [](CEntityScriptTest&, void* Ctx)
            {
                FSpawnChain& Chain = *static_cast<FSpawnChain*>(Ctx);
                if (Chain.Links.Num() < Chain.Length)
                {
                    Chain.SpawnLink();
                }
            };
            Links.Add(Link);
            return Link;
        }

        int32 NumReady() const
        {
            int32 Count = 0;
            for (const CEntityScriptTest* Link : Links)
            {
                Count += Link->ReadyCount;
            }
            return Count;
        }
    };
}

// Each OnReady spawns the next link, so a chain longer than the per-frame lifecycle rounds spills into the next frame.
TEST(EntityScriptUnification, ASpawnChainPastTheLifecycleRoundCapWarnsAndFinishesNextFrame)
{
    constexpr const char* CapWarning = "spawned past the 8 lifecycle rounds a frame allows, so the 1 still pending run OnAttach or OnReady";

    ECS::FRegistry AtCapRegistry{};
    FSpawnChain AtCap{ &AtCapRegistry, 8 };
    AtCap.SpawnLink();
    EntityScripts::Tick(AtCapRegistry, 0.1f);
    EXPECT_EQ(AtCap.NumReady(), 8) << "eight links fit the eight rounds of one frame";
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "lifecycle rounds a frame allows"), 0) << "using every round with nothing left over is not an overflow";

    ECS::FRegistry PastCapRegistry{};
    FSpawnChain PastCap{ &PastCapRegistry, 12 };
    PastCap.SpawnLink();
    EntityScripts::Tick(PastCapRegistry, 0.1f);
    EXPECT_LT(PastCap.NumReady(), 12) << "the chain runs past what one frame readies";
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, CapWarning), 1);

    EntityScripts::Tick(PastCapRegistry, 0.1f);
    EXPECT_EQ(PastCap.Links.Num(), 12);
    EXPECT_EQ(PastCap.NumReady(), 12) << "the spilled links are delayed a frame, never dropped";
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, "lifecycle rounds a frame allows"), 1) << "the warning is not repeated every frame";
}

// One script class going missing used to end the record, taking its still-resolvable siblings with it.
TEST(EntityScriptUnification, AnUnresolvableScriptClassSkipsOnlyItself)
{
    // ResolveClass goes through FindObject, so the class has to be registered before the read path runs.
    RegisterPendingTypes();
    CEntityScriptTest::StaticClass()->GetDefaultObject();

    ECS::FRegistry Registry{};
    const ECS::FEntity Entity = Registry.Create();

    CEntityScriptTest* First = AttachTestScript(Registry, Entity);
    ASSERT_NE(First, nullptr);
    First->Values.Emplace("KeptFirst");

    CEntityScriptTest* Second = AttachTestScript(Registry, Entity);
    ASSERT_NE(Second, nullptr);
    Second->Values.Emplace("KeptSecond");

    // A real two-script payload, so the bogus record below sits between two well-formed ones.
    TVector<uint8> Good;
    {
        SEntityScriptComponent& Live = Registry.Get<SEntityScriptComponent>(Entity);
        FMemoryWriter Writer(Good);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Live.Serialize(Ar);
    }

    // Rebuilt by hand, since nothing can write a class name that does not resolve.
    TVector<uint8> Mixed;
    {
        FMemoryWriter Writer(Mixed);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);

        int32 Count = 3;
        Ar << Count;

        FName Missing("CScriptClassThatWasDeleted");
        Ar << Missing;
        int64 MissingSize = 8;
        Ar << MissingSize;
        int64 Filler = 0;
        Ar << Filler;

        // The two real scripts, appended verbatim after the count they were written with.
        TVector<uint8> Tail;
        {
            SEntityScriptComponent& Live = Registry.Get<SEntityScriptComponent>(Entity);
            FMemoryWriter TailWriter(Tail);
            FObjectProxyArchiver TailAr(TailWriter, /*bLoadIfFindFails*/ false);
            int32 Ignored = 0;
            TailAr << Ignored;
            Live.Serialize(TailAr);
        }
        const size_t HeaderBytes = sizeof(int32) * 2;
        ASSERT_GT(Tail.Num(), HeaderBytes);
        Ar.Serialize(Tail.GetData() + HeaderBytes, (int64)(Tail.Num() - HeaderBytes));
    }

    SEntityScriptComponent Restored;
    {
        FMemoryReader Reader(Mixed);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Restored.Serialize(Ar);
    }

    ASSERT_EQ(Restored.Scripts.Num(), 2) << "the siblings of a missing class must survive it";

    CEntityScriptTest* RestoredFirst  = Cast<CEntityScriptTest>(Restored.Scripts[0].Get());
    CEntityScriptTest* RestoredSecond = Cast<CEntityScriptTest>(Restored.Scripts[1].Get());
    ASSERT_NE(RestoredFirst, nullptr);
    ASSERT_NE(RestoredSecond, nullptr);

    ASSERT_EQ(RestoredFirst->Values.Num(), 1);
    ASSERT_EQ(RestoredSecond->Values.Num(), 1);
    EXPECT_EQ(RestoredFirst->Values[0], FName("KeptFirst")) << "skipping must not shift the next script";
    EXPECT_EQ(RestoredSecond->Values[0], FName("KeptSecond"));

    EntityScripts::DetachAll(Registry, Entity);
}

// Undo and duplicate copy entities through a proxy archive, and a held script's object slots once escaped to the inner one.
TEST(EntityScriptUnification, AHeldScriptKeepsItsObjectsThroughAProxyArchive)
{
    RegisterPendingTypes();
    CEntityScriptTest* Target = NewObject<CEntityScriptTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Target, nullptr);

    SEntityScriptComponent Source;
    SHeldValue& Held = Source.Pending.EmplaceGetRef();
    Held.TypeName = FName("CScriptClassThatIsNotLoaded");
    Held.Data.Bytes = { 1, 2, 3, 4 };
    Held.Data.Names.Emplace(0, FName("HeldName"));
    Held.Data.Objects.Emplace(0, Target->GetGUID());

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
        Source.Serialize(Ar);
    }

    SEntityScriptComponent Restored;
    {
        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
        Restored.Serialize(Ar);
        EXPECT_FALSE(Ar.HasError());
    }

    ASSERT_EQ(Restored.Pending.Num(), 1);
    const FPortableBytes& Data = Restored.Pending[0].Data;
    EXPECT_EQ(Restored.Pending[0].TypeName, FName("CScriptClassThatIsNotLoaded"));
    EXPECT_EQ(Data.Bytes, Held.Data.Bytes);
    ASSERT_EQ(Data.Names.Num(), 1);
    EXPECT_EQ(Data.Names[0].Name, FName("HeldName"));
    ASSERT_EQ(Data.Objects.Num(), 1);
    EXPECT_EQ(Data.Objects[0].Object, Target->GetGUID());
}

namespace
{
    Scripting::FScriptExportSchema IntSchema(const char* FieldName)
    {
        Scripting::FScriptExportSchema Schema;
        Scripting::FScriptExportField& Field = Schema.Fields.EmplaceGetRef();
        Field.Name = FName(FieldName);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = EPropertyTypeFlags::Int32;
        return Schema;
    }
}

// A C# class derived from another C# class stacks its block after the parent's, so the two never share a slot.
TEST(ScriptClassInheritance, ASubclassExtendsItsScriptParentsLayout)
{
    CScriptClass* Parent = FScriptableRegistry::Mint("Inherit_GTestParent", "CEntityScript");
    ASSERT_NE(Parent, nullptr);
    ASSERT_GT(Scripting::AppendScriptPropertiesToClass(Parent, IntSchema("ParentValue")), 0u);

    CScriptClass* Child = FScriptableRegistry::Mint("Inherit_GTestChild", "Inherit_GTestParent");
    ASSERT_NE(Child, nullptr);
    ASSERT_GT(Scripting::AppendScriptPropertiesToClass(Child, IntSchema("ChildValue")), 0u);
    RegisterPendingTypes();
    Parent->GetDefaultObject();
    Child->GetDefaultObject();

    EXPECT_EQ(Child->GetSuperClass(), Parent);
    EXPECT_TRUE(Child->IsChildOf(Parent));

    FProperty* ParentValue = Parent->GetProperty(FName("ParentValue"));
    FProperty* ChildValue = Child->GetProperty(FName("ChildValue"));
    ASSERT_NE(ParentValue, nullptr);
    ASSERT_NE(ChildValue, nullptr);
    EXPECT_EQ(Child->GetProperty(FName("ParentValue")), ParentValue) << "the child reaches the parent's field through its super";
    EXPECT_GE(ChildValue->Offset, ParentValue->Offset + sizeof(int32)) << "the child's block must start after the parent's";

    *ParentValue->GetValuePtr<int32>(Parent->GetDefaultObject()) = 0;
    *ParentValue->GetValuePtr<int32>(Child->GetDefaultObject()) = 41;
    *ChildValue->GetValuePtr<int32>(Child->GetDefaultObject()) = 42;

    CObject* Instance = NewObject(Child, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Instance, nullptr);
    EXPECT_EQ(*ParentValue->GetValuePtr<int32>(Instance), 41) << "an instance takes its inherited defaults from its own class default object";
    EXPECT_EQ(*ChildValue->GetValuePtr<int32>(Instance), 42);
}

TEST(ScriptClassInheritance, AnAbstractClassIsNeverInstantiated)
{
    CScriptClass* Abstract = FScriptableRegistry::Mint("Inherit_GTestAbstract", "CEntityScript");
    ASSERT_NE(Abstract, nullptr);
    Abstract->bAbstract = true;
    RegisterPendingTypes();

    EXPECT_TRUE(Abstract->IsAbstract());
    EXPECT_FALSE(CEntityScript::StaticClass()->IsAbstract()) << "a native class is never abstract by this flag";
    EXPECT_EQ(NewObject(Abstract, nullptr, NAME_None, FGuid::New(), OF_Transient), nullptr);

    CScriptClass* Concrete = FScriptableRegistry::Mint("Inherit_GTestConcrete", "Inherit_GTestAbstract");
    ASSERT_NE(Concrete, nullptr);
    RegisterPendingTypes();
    EXPECT_NE(NewObject(Concrete, nullptr, NAME_None, FGuid::New(), OF_Transient), nullptr);
}

namespace
{
    Scripting::FManagedTypeDefinition ScriptClassDefinition(const char* Name, const char* Base, const char* FieldName, bool bAbstract = false)
    {
        Scripting::FManagedTypeDefinition Definition;
        Definition.Kind           = Scripting::EManagedTypeKind::ScriptableClass;
        Definition.TypeName       = FName(Name);
        Definition.NativeBaseName = Base;
        Definition.bAbstract      = bAbstract;
        Definition.Schema         = IntSchema(FieldName);
        Definition.bHasSchema     = true;
        return Definition;
    }
}

namespace
{
    // The two class stages of a reload, shells first so other types can name a class before it is laid out.
    void ReloadClasses(const TVector<Scripting::FManagedTypeDefinition>& Definitions)
    {
        FScriptableRegistry::MintNewClasses(Definitions);
        FScriptableRegistry::RefreshMintedClasses(Definitions);
        RegisterPendingTypes();
    }

    uint32 FieldOffset(const char* ClassName, const char* FieldName)
    {
        CClass* Class = FindObject<CClass>(FName(ClassName));
        FProperty* Property = Class != nullptr ? Class->GetProperty(FName(FieldName)) : nullptr;
        return Property != nullptr ? Property->Offset : 0;
    }
}

// A child shell is minted before its parent's block exists, so its own block has to start past the parent's once laid out.
TEST(ScriptClassInheritance, ANewChildIsLaidOutPastANewParent)
{
    TVector<Scripting::FManagedTypeDefinition> Definitions;
    Definitions.Add(ScriptClassDefinition("Shell_GTestParent", "CEntityScript", "ParentValue"));
    Definitions.Add(ScriptClassDefinition("Shell_GTestChild", "Shell_GTestParent", "ChildValue"));
    ReloadClasses(Definitions);

    CClass* Parent = FindObject<CClass>(FName("Shell_GTestParent"));
    ASSERT_NE(Parent, nullptr);
    EXPECT_GE(FieldOffset("Shell_GTestChild", "ChildValue"), Parent->GetSize());
    EXPECT_NE(FieldOffset("Shell_GTestChild", "ChildValue"), FieldOffset("Shell_GTestParent", "ParentValue"));

    // A parent reshaped in the same reload that adds a child under it hands that child to the replacement.
    TVector<Scripting::FManagedTypeDefinition> Second;
    Second.Add(ScriptClassDefinition("Shell_GTestParent", "CEntityScript", "WiderParentValue"));
    Second.Add(ScriptClassDefinition("Shell_GTestChild", "Shell_GTestParent", "ChildValue"));
    Second.Add(ScriptClassDefinition("Shell_GTestLateChild", "Shell_GTestParent", "LateValue"));
    ReloadClasses(Second);

    CClass* Replacement = FindObject<CClass>(FName("Shell_GTestParent"));
    ASSERT_NE(Replacement, nullptr);
    CClass* LateChild = FindObject<CClass>(FName("Shell_GTestLateChild"));
    ASSERT_NE(LateChild, nullptr);
    EXPECT_EQ(LateChild->GetSuperClass(), Replacement);
    EXPECT_EQ(FindObject<CClass>(FName("Shell_GTestChild"))->GetSuperClass(), Replacement);
    EXPECT_GE(FieldOffset("Shell_GTestLateChild", "LateValue"), Replacement->GetSize());
}

// The stress harness's crash. A removed parent with no instances of its own was freed while a subclass still linked to it.
TEST(ScriptClassInheritance, ARemovedParentOutlivesTheSubclassStillOnIt)
{
    TVector<Scripting::FManagedTypeDefinition> First;
    First.Add(ScriptClassDefinition("Retire_GTestParent", "CEntityScript", "ParentValue", /*bAbstract*/ true));
    First.Add(ScriptClassDefinition("Retire_GTestChild", "Retire_GTestParent", "ChildValue"));
    FScriptableRegistry::RefreshMintedClasses(First);

    CClass* OldChild = FindObject<CClass>(FName("Retire_GTestChild"));
    ASSERT_NE(OldChild, nullptr);
    ASSERT_EQ(OldChild->GetSuperClass(), FindObject<CClass>(FName("Retire_GTestParent")));

    // Held by a strong reference no walk can reach, so the reload cannot move it onto the replacement.
    TStrongObjectPtr<CEntityScript> Script(static_cast<CEntityScript*>(NewObject(OldChild, nullptr, NAME_None, FGuid::New(), OF_Transient)));
    ASSERT_NE(Script.Get(), nullptr);
    *OldChild->GetProperty(FName("ChildValue"))->GetValuePtr<int32>(Script.Get()) = 77;

    // The parent goes and the child moves up to the native base, the way the stress run reshaped them.
    TVector<Scripting::FManagedTypeDefinition> Second;
    Second.Add(ScriptClassDefinition("Retire_GTestChild", "CEntityScript", "ChildValue"));
    FScriptableRegistry::RefreshMintedClasses(Second);

    CClass* NewChild = FindObject<CClass>(FName("Retire_GTestChild"));
    CClass* Parent = FindObject<CClass>(FName("Retire_GTestParent"));
    ASSERT_NE(NewChild, nullptr);
    EXPECT_NE(NewChild, OldChild);
    EXPECT_EQ(NewChild->GetSuperClass(), CEntityScript::StaticClass());

    ASSERT_NE(Parent, nullptr) << "a parent with a live subclass must not be freed";
    EXPECT_EQ(OldChild->GetSuperClass(), Parent);
    EXPECT_TRUE(Script->GetClass()->IsChildOf(CEntityScript::StaticClass()));
    EXPECT_EQ(*OldChild->GetProperty(FName("ChildValue"))->GetValuePtr<int32>(Script.Get()), 77);

    // With the instance gone, the next refresh retires the old subclass and then the parent it was holding.
    Script.Reset();
    FScriptableRegistry::RefreshMintedClasses(Second);
    EXPECT_EQ(FindObject<CClass>(FName("Retire_GTestParent")), nullptr);
    EXPECT_EQ(FindObject<CClass>(FName("Retire_GTestChild")), NewChild);
}

// The third stress crash. A parent minted with no fields gained one after a subclass was laid out at its old size.
TEST(ScriptClassInheritance, AParentThatGainsItsFirstFieldRebuildsItsSubclasses)
{
    Scripting::FManagedTypeDefinition EmptyParent = ScriptClassDefinition("Grow_GTestParent", "CEntityScript", "Unused");
    EmptyParent.Schema = Scripting::FScriptExportSchema();

    TVector<Scripting::FManagedTypeDefinition> First;
    First.Add(EmptyParent);
    First.Add(ScriptClassDefinition("Grow_GTestChild", "Grow_GTestParent", "ChildValue"));
    FScriptableRegistry::RefreshMintedClasses(First);

    CClass* OldChild = FindObject<CClass>(FName("Grow_GTestChild"));
    ASSERT_NE(OldChild, nullptr);

    TVector<Scripting::FManagedTypeDefinition> Second;
    Second.Add(ScriptClassDefinition("Grow_GTestParent", "CEntityScript", "ParentValue"));
    Second.Add(ScriptClassDefinition("Grow_GTestChild", "Grow_GTestParent", "ChildValue"));
    FScriptableRegistry::RefreshMintedClasses(Second);

    CClass* Parent = FindObject<CClass>(FName("Grow_GTestParent"));
    CClass* Child = FindObject<CClass>(FName("Grow_GTestChild"));
    ASSERT_NE(Parent, nullptr);
    ASSERT_NE(Child, nullptr);
    EXPECT_NE(Child, OldChild) << "the subclass has to be rebuilt after its parent's new block";
    EXPECT_EQ(Child->GetSuperClass(), Parent);

    FProperty* ParentValue = Child->GetProperty(FName("ParentValue"));
    FProperty* ChildValue = Child->GetProperty(FName("ChildValue"));
    ASSERT_NE(ParentValue, nullptr);
    ASSERT_NE(ChildValue, nullptr);
    EXPECT_GE(ChildValue->Offset, ParentValue->Offset + sizeof(int32)) << "the two blocks must not overlap";
}

// A class under a C# parent is minted before that parent registers, and must still find its events on the native ancestor.
TEST(ScriptClassInheritance, AClassUnderAScriptParentKeepsItsOverrides)
{
    Scripting::FManagedTypeDefinition Parent = ScriptClassDefinition("Override_GTestParent", "CEntityScript", "ParentValue");
    Scripting::FManagedTypeDefinition Child = ScriptClassDefinition("Override_GTestChild", "Override_GTestParent", "ChildValue");
    Parent.OverriddenEvents.Add("OnReady");
    Child.OverriddenEvents.Add("OnReady");
    Child.OverriddenEvents.Add("OnUpdate");
    ReloadClasses({ Parent, Child });

    const CScriptClass* ChildClass = Cast<CScriptClass>(FindObject<CClass>(FName("Override_GTestChild")));
    ASSERT_NE(ChildClass, nullptr);
    EXPECT_NE(ChildClass->FindScriptOverride(FName("OnReady")), nullptr) << "an event overridden in C# under a C# parent must dispatch to it";
    EXPECT_NE(ChildClass->FindScriptOverride(FName("OnUpdate")), nullptr);
}

namespace
{
    // Added the way a loaded scene arrives, unattached, so the tick's lifecycle pass is what attaches it.
    CEntityScriptTest* AddUnattached(ECS::FRegistry& Registry, ECS::FEntity Entity, const char* Label, TVector<FString>& Log)
    {
        auto* Script = static_cast<CEntityScriptTest*>(NewObject(CEntityScriptTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient));
        Script->Label = Label;
        Script->EventLog = &Log;
        Registry.GetOrEmplace<SEntityScriptComponent>(Entity).Scripts.Add(Script);
        return Script;
    }

    // Reparenting keeps world placement, so it needs a transform to work with.
    ECS::FEntity CreatePlaced(ECS::FRegistry& Registry)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<STransformComponent>(Entity);
        return Entity;
    }

    FString Joined(const TVector<FString>& Log)
    {
        FString Out;
        for (const FString& Entry : Log)
        {
            Out += Out.IsEmpty() ? Entry : " " + Entry;
        }
        return Out;
    }
}

// Godot's order. Every attach runs parents first, then every ready runs children first, scripts top to bottom within an entity.
TEST(EntityScriptUnification, LifecycleRunsAttachInPreorderThenReadyInPostorder)
{
    RegisterPendingTypes();
    ECS::FRegistry Registry{};
    TVector<FString> Log;

    const ECS::FEntity Root   = CreatePlaced(Registry);
    const ECS::FEntity First  = CreatePlaced(Registry);
    const ECS::FEntity Second = CreatePlaced(Registry);
    const ECS::FEntity Leaf   = CreatePlaced(Registry);
    const ECS::FEntity Loner  = CreatePlaced(Registry);
    ECS::Utils::ReparentEntity(Registry, First, Root);
    ECS::Utils::ReparentEntity(Registry, Second, Root);
    ECS::Utils::ReparentEntity(Registry, Leaf, First);

    // Added out of tree order, so the result cannot come from insertion order.
    AddUnattached(Registry, Loner, "loner", Log);
    AddUnattached(Registry, Leaf, "leaf", Log);
    AddUnattached(Registry, Second, "second", Log);
    AddUnattached(Registry, Root, "root1", Log);
    AddUnattached(Registry, Root, "root2", Log);
    AddUnattached(Registry, First, "first", Log);

    EntityScripts::Tick(Registry, 0.1f);

    EXPECT_EQ(Joined(Log), FString("A:root1 A:root2 A:first A:leaf A:second A:loner "
                                   "R:leaf R:first R:second R:root1 R:root2 R:loner"));
}

// A script added while the tick runs joins the next lifecycle round in the same order, rather than readying ahead of its parent.
TEST(EntityScriptUnification, LifecycleOrderHoldsForAScriptAddedLater)
{
    RegisterPendingTypes();
    ECS::FRegistry Registry{};
    TVector<FString> Log;

    const ECS::FEntity Parent = CreatePlaced(Registry);
    const ECS::FEntity Child  = CreatePlaced(Registry);
    ECS::Utils::ReparentEntity(Registry, Child, Parent);

    AddUnattached(Registry, Parent, "parent", Log);
    EntityScripts::Tick(Registry, 0.1f);
    Log.Reset();

    AddUnattached(Registry, Child, "child", Log);
    AddUnattached(Registry, Parent, "late", Log);
    EntityScripts::Tick(Registry, 0.1f);

    EXPECT_EQ(Joined(Log), FString("A:late A:child R:child R:late"));
}
