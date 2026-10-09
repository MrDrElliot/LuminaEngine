#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "SaveGame/SaveGameArchive.h"
#include "SaveGame/SaveGameComponent.h"
#include "SaveGame/SaveGameEvents.h"
#include "SaveGame/SaveGameSystem.h"
#include "SaveGame/SaveGameLibrary.h"
#include "SaveGame/SaveGameTestTypes.h"
#include "SaveGame/WorldSaveState.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaSaveGameTests
{
    using namespace Lumina;

    void BindTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    struct FLevel
    {
        CWorld*        World = nullptr;
        ECS::FEntity   Chest;
        ECS::FEntity   Door;
        ECS::FEntity   Crate;
        ECS::FEntity   Statue;
        ECS::FEntity   Wizard;
        ECS::FEntity   Machine;

        ECS::FRegistry& Registry() const { return ECS::GetWorldRegistry(*World); }
    };

    ECS::FEntity Spawn(ECS::FRegistry& Registry, const char* Name, ECS::FEntity Parent = ECS::NullEntity)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<SNameComponent>(Entity, FName(Name));
        Registry.Emplace<STransformComponent>(Entity);
        if (!Parent.IsNull())
        {
            Registry.AttachChild(Entity, Parent);
        }
        return Entity;
    }

    // Built the same way every time, which is what loading one level file twice amounts to.
    FLevel LoadLevel()
    {
        ProcessNewlyLoadedCObjects();
        FLevel Level;
        // One name for every load, since a save game keys a world's state by its level name.
        Level.World = NewObject<CWorld>(nullptr, FName("SaveGameTestLevel"), FGuid::New(), OF_Transient);
        ECS::FRegistry& R = Level.Registry();
        R.GetSignals<STransformComponent>().OnConstruct.Connect<&BindTransform>();

        Level.Chest = Spawn(R, "Chest");
        R.Emplace<SSaveGameTestComponent>(Level.Chest).Coins = 1;
        Level.Door   = Spawn(R, "Door");
        Level.Crate  = Spawn(R, "Crate");
        Level.Statue = Spawn(R, "Statue");
        R.Emplace<SSaveGameComponent>(Level.Statue);
        Level.Wizard = Spawn(R, "Wizard");
        EntityScripts::Attach(R, Level.Wizard, CSaveGameTestScript::StaticClass());

        SaveGame::RecordLevelEntities(R);

        // What a game builds at runtime, every run, which a save finds by its name path.
        const ECS::FEntity Arcade = Spawn(R, "Arcade");
        Level.Machine = Spawn(R, "Machine", Arcade);
        Spawn(R, "Machine", Arcade);
        R.Emplace<SSaveGameTestComponent>(Level.Machine);
        return Level;
    }

    CSaveGameTestScript* WizardScript(const FLevel& Level)
    {
        return static_cast<CSaveGameTestScript*>(EntityScripts::Find(Level.Registry(), Level.Wizard, CSaveGameTestScript::StaticClass()));
    }

    TVector<uint8> Capture(ECS::FRegistry& Registry)
    {
        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        EXPECT_TRUE(SaveGame::SerializeRegistry(Writer, Registry));
        return Bytes;
    }

    bool Restore(ECS::FRegistry& Registry, const TVector<uint8>& Bytes)
    {
        FMemoryReader Reader(Bytes);
        return SaveGame::SerializeRegistry(Reader, Registry);
    }

    TVector<ECS::FEntity> Respawned(ECS::FRegistry& Registry)
    {
        TVector<ECS::FEntity> Out;
        for (ECS::FEntity Entity : Registry.View<SSaveGameComponent>())
        {
            if (!SaveGame::IsLevelEntity(Registry, Entity))
            {
                Out.push_back(Entity);
            }
        }
        return Out;
    }

    // Plays a session on Level, which every round trip test then saves.
    ECS::FEntity Play(FLevel& Level)
    {
        ECS::FRegistry& R = Level.Registry();

        SSaveGameTestComponent& Chest = R.Get<SSaveGameTestComponent>(Level.Chest);
        Chest.Coins       = 50;
        Chest.Target      = Level.Door;
        Chest.History     = { 1, 2, 3 };
        Chest.RuntimeOnly = 7.5f;
        Chest.NotSaved    = 99;

        // Two objects that point at each other, so the inline writer has a cycle to keep.
        CSaveGameTestObject* Sword = NewObject<CSaveGameTestObject>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        CSaveGameTestObject* Sheath = NewObject<CSaveGameTestObject>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        Sword->Label = "Sword";
        Sword->Count = 2;
        Sword->Next = Sheath;
        Sheath->Label = "Sheath";
        Sheath->Next = Sword;
        Chest.Inventory = Sword;

        R.Destroy(Level.Crate);
        R.Get<STransformComponent>(Level.Statue).SetLocalLocation(FVector3(4.0f, 5.0f, 6.0f));

        WizardScript(Level)->Level = 12;
        WizardScript(Level)->NotSaved = 3;

        R.Get<SSaveGameTestComponent>(Level.Machine).Coins = 5;

        // A pickup the player dropped, with a child, pointing back at a level entity.
        const ECS::FEntity Pickup = Spawn(R, "Pickup");
        R.Emplace<SSaveGameComponent>(Pickup);
        R.Emplace<SSaveGameTestComponent>(Pickup).Target = Level.Chest;
        R.Get<STransformComponent>(Pickup).SetLocalLocation(FVector3(9.0f, 0.0f, 0.0f));
        const ECS::FEntity Gem = Spawn(R, "Gem", Pickup);
        R.Emplace<SSaveGameTestComponent>(Gem).Target = Pickup;
        return Pickup;
    }
}

using namespace LuminaSaveGameTests;

TEST(SaveGame, FlagsReachRuntimeProperties)
{
    const CStruct* Struct = SSaveGameTestComponent::StaticStruct();
    EXPECT_TRUE(Struct->GetProperty(FName("Coins"))->IsSaveGame());
    EXPECT_TRUE(Struct->GetProperty(FName("RuntimeOnly"))->IsSaveGame());
    EXPECT_FALSE(Struct->GetProperty(FName("RuntimeOnly"))->ShouldSerialize());
    EXPECT_FALSE(Struct->GetProperty(FName("NotSaved"))->IsSaveGame());
    EXPECT_TRUE(SaveGame::HasSaveGameProperties(Struct));
    EXPECT_FALSE(SaveGame::HasSaveGameProperties(SNameComponent::StaticStruct()));
}

TEST(SaveGame, WorldRoundTripIntoAFreshLevel)
{
    FLevel Session = LoadLevel();
    Play(Session);
    const TVector<uint8> Bytes = Capture(Session.Registry());

    FLevel Loaded = LoadLevel();
    ECS::FRegistry& R = Loaded.Registry();
    ASSERT_TRUE(Restore(R, Bytes));

    const SSaveGameTestComponent& Chest = R.Get<SSaveGameTestComponent>(Loaded.Chest);
    EXPECT_EQ(Chest.Coins, 50);
    EXPECT_EQ(Chest.Target, Loaded.Door);
    EXPECT_EQ(Chest.History, (TVector<int32>{ 1, 2, 3 }));
    EXPECT_FLOAT_EQ(Chest.RuntimeOnly, 7.5f);
    EXPECT_EQ(Chest.NotSaved, 0);

    ASSERT_NE(Chest.Inventory.Get(), nullptr);
    EXPECT_EQ(Chest.Inventory->Label, "Sword");
    EXPECT_EQ(Chest.Inventory->Count, 2);
    ASSERT_NE(Chest.Inventory->Next.Get(), nullptr);
    EXPECT_EQ(Chest.Inventory->Next->Label, "Sheath");
    EXPECT_EQ(Chest.Inventory->Next->Next.Get(), Chest.Inventory.Get());

    EXPECT_FALSE(R.IsValid(Loaded.Crate));
    EXPECT_EQ(R.Get<STransformComponent>(Loaded.Statue).GetLocalLocation(), FVector3(4.0f, 5.0f, 6.0f));

    CSaveGameTestScript* Wizard = WizardScript(Loaded);
    ASSERT_NE(Wizard, nullptr);
    EXPECT_EQ(Wizard->Level, 12);
    EXPECT_EQ(Wizard->NotSaved, 0);
    EXPECT_EQ(Wizard->RestoredCount, 1);

    EXPECT_EQ(R.Get<SSaveGameTestComponent>(Loaded.Machine).Coins, 5);

    const TVector<ECS::FEntity> Pickups = Respawned(R);
    ASSERT_EQ(Pickups.size(), 1u);
    const ECS::FEntity Pickup = Pickups[0];
    EXPECT_EQ(R.Get<SNameComponent>(Pickup).Name, FName("Pickup"));
    EXPECT_EQ(R.Get<SSaveGameTestComponent>(Pickup).Target, Loaded.Chest);
    EXPECT_EQ(R.Get<STransformComponent>(Pickup).GetLocalLocation(), FVector3(9.0f, 0.0f, 0.0f));

    const ECS::FEntity Gem = R.GetHierarchy().GetFirstChild(Pickup);
    ASSERT_FALSE(Gem.IsNull());
    EXPECT_EQ(R.Get<SNameComponent>(Gem).Name, FName("Gem"));
    EXPECT_EQ(R.Get<SSaveGameTestComponent>(Gem).Target, Pickup);
}

TEST(SaveGame, RestoringInPlaceReplacesRuntimeSpawnsInsteadOfDoublingThem)
{
    FLevel Session = LoadLevel();
    Play(Session);
    ECS::FRegistry& R = Session.Registry();
    const TVector<uint8> Bytes = Capture(R);

    R.Get<SSaveGameTestComponent>(Session.Chest).Coins = 1000;
    ASSERT_TRUE(Restore(R, Bytes));
    ASSERT_TRUE(Restore(R, Bytes));

    EXPECT_EQ(R.Get<SSaveGameTestComponent>(Session.Chest).Coins, 50);
    EXPECT_EQ(Respawned(R).size(), 1u);

    uint32 Gems = 0;
    for (ECS::FEntity Entity : R.View<SNameComponent>())
    {
        Gems += R.Get<SNameComponent>(Entity).Name == FName("Gem") ? 1u : 0u;
    }
    EXPECT_EQ(Gems, 1u);
}

TEST(SaveGame, ADamagedWorldStateFailsWithoutTouchingTheWorld)
{
    FLevel Session = LoadLevel();
    Play(Session);
    TVector<uint8> Bytes = Capture(Session.Registry());
    Bytes[0] ^= 0xFF;

    FLevel Loaded = LoadLevel();
    EXPECT_FALSE(Restore(Loaded.Registry(), Bytes));
    EXPECT_TRUE(Loaded.Registry().IsValid(Loaded.Crate));
    EXPECT_EQ(Loaded.Registry().Get<SSaveGameTestComponent>(Loaded.Chest).Coins, 1);
}

TEST(SaveGame, SaveGameObjectRoundTripsWithItsWorlds)
{
    FLevel Session = LoadLevel();
    Play(Session);

    CSaveGameTest* Game = NewObject<CSaveGameTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    Game->Score = 4200;
    Game->PlayerName = "Ada";
    Game->SessionOnly = 9;
    Game->Item = NewObject<CSaveGameTestObject>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    Game->Item->Label = "Lamp";
    ASSERT_TRUE(Game->CaptureRegistry("Level", Session.Registry()));

    TVector<uint8> Bytes;
    ASSERT_TRUE(CSaveGameLibrary::SaveGameToBytes(Game, Bytes));
    EXPECT_EQ(Game->BeforeSaveCount, 1);
    EXPECT_GT(Game->SavedUnixTime, 0);

    CSaveGame* LoadedBase = CSaveGameLibrary::LoadGameFromBytes(Bytes);
    ASSERT_NE(LoadedBase, nullptr);
    ASSERT_EQ(LoadedBase->GetClass(), CSaveGameTest::StaticClass());
    CSaveGameTest* Loaded = static_cast<CSaveGameTest*>(LoadedBase);
    EXPECT_EQ(Loaded->Score, 4200);
    EXPECT_EQ(Loaded->PlayerName, "Ada");
    EXPECT_EQ(Loaded->SessionOnly, 9);
    EXPECT_EQ(Loaded->SavedUnixTime, Game->SavedUnixTime);
    EXPECT_EQ(Loaded->AfterLoadCount, 1);
    ASSERT_NE(Loaded->Item.Get(), nullptr);
    EXPECT_NE(Loaded->Item.Get(), Game->Item.Get());
    EXPECT_EQ(Loaded->Item->Label, "Lamp");

    FLevel Fresh = LoadLevel();
    EXPECT_FALSE(Loaded->RestoreRegistry("Elsewhere", Fresh.Registry()));
    ASSERT_TRUE(Loaded->RestoreRegistry("Level", Fresh.Registry()));
    EXPECT_EQ(Fresh.Registry().Get<SSaveGameTestComponent>(Fresh.Chest).Coins, 50);
    EXPECT_FALSE(Fresh.Registry().IsValid(Fresh.Crate));
}

TEST(SaveGame, BytesThatAreNotASaveAreRefused)
{
    TVector<uint8> Garbage(64, 0xAB);
    EXPECT_EQ(CSaveGameLibrary::LoadGameFromBytes(Garbage), nullptr);
    EXPECT_EQ(CSaveGameLibrary::LoadGameFromBytes(TVector<uint8>()), nullptr);

    CSaveGameTest* Game = NewObject<CSaveGameTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    TVector<uint8> Bytes;
    ASSERT_TRUE(CSaveGameLibrary::SaveGameToBytes(Game, Bytes));
    Bytes.resize(Bytes.size() / 2);
    EXPECT_EQ(CSaveGameLibrary::LoadGameFromBytes(Bytes), nullptr);
}

TEST(SaveGame, SlotNamesStayInsideTheSaveDirectory)
{
    const FString Directory = CSaveGameLibrary::GetSaveGameDirectory(0);
    const FString Path = CSaveGameLibrary::GetSlotFilePath("../../Windows/evil:name", 0);
    ASSERT_GT(Path.size(), Directory.size() + 1);
    EXPECT_EQ(Path.substr(0, Directory.size() + 1), Directory + "/");
    const FString FileName = Path.substr(Directory.size() + 1);
    EXPECT_EQ(FileName.find('/'), FString::npos);
    EXPECT_EQ(FileName.find('\\'), FString::npos);
    EXPECT_EQ(FileName.find(':'), FString::npos);
    EXPECT_TRUE(CSaveGameLibrary::GetSlotFilePath("...", 0).empty());
    EXPECT_NE(CSaveGameLibrary::GetSaveGameDirectory(2), Directory);
}

namespace LuminaSaveGameTests
{
    struct FEventLog
    {
        int32 Saving = 0;
        int32 Restored = 0;
        TVector<SSaveGameCompletedEvent> Completed;

        void OnSaving(const SWorldSavingEvent&) { ++Saving; }
        void OnRestored(const SWorldRestoredEvent&) { ++Restored; }
        void OnCompleted(const SSaveGameCompletedEvent& Event) { Completed.push_back(Event); }
    };

    // The parts of InitializeWorld the save path leans on, a dispatcher in the context and running systems.
    struct FRunningWorld
    {
        FLevel                 Level = LoadLevel();
        ECS::FEventDispatcher  Dispatcher;
        FEventLog              Log;
        CSaveGameTestSystem*   Listener = nullptr;
        SSaveGameSystem*       Saver = nullptr;

        FRunningWorld()
        {
            Level.Registry().Ctx().Emplace<ECS::FEventDispatcher*>(&Dispatcher);
            Dispatcher.Sink<SWorldSavingEvent>().Connect<&FEventLog::OnSaving>(&Log);
            Dispatcher.Sink<SWorldRestoredEvent>().Connect<&FEventLog::OnRestored>(&Log);
            Dispatcher.Sink<SSaveGameCompletedEvent>().Connect<&FEventLog::OnCompleted>(&Log);

            Listener = NewObject<CSaveGameTestSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Saver = NewObject<SSaveGameSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            for (CEntitySystem* System : { (CEntitySystem*)Listener, (CEntitySystem*)Saver })
            {
                System->SetOwningWorld(Level.World);
                Level.World->Systems.push_back(System);
                System->OnStartup();
            }
        }

        ~FRunningWorld()
        {
            Saver->OnTeardown();
        }

        void EndFrame()
        {
            if (Saver->HasWork(EUpdateStage::FrameEnd))
            {
                Saver->OnUpdate();
            }
        }
    };
}

TEST(SaveGame, RequestsRunAtTheSystemUpdateAndReportThroughEventsAndSystems)
{
    const FString Slot = Format("LuminaUnitTest_{}", FGuid::New().ToString());

    FRunningWorld Session;
    Play(Session.Level);
    CSaveGameTest* Game = NewObject<CSaveGameTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    Game->Score = 77;

    CSaveGameLibrary::RequestSaveGame(Session.Level.World, Game, Slot, 0);
    EXPECT_EQ(Session.Log.Saving, 0);
    EXPECT_TRUE(Session.Saver->HasWork(EUpdateStage::FrameEnd));
    Session.EndFrame();
    EXPECT_FALSE(Session.Saver->HasWork(EUpdateStage::FrameEnd));

    EXPECT_EQ(Session.Log.Saving, 1);
    EXPECT_EQ(Session.Listener->SavingCount, 1);
    EXPECT_EQ(Session.Listener->LastSaveGame, Game);
    EXPECT_EQ(WizardScript(Session.Level)->SavingCount, 1);
    ASSERT_EQ(Session.Log.Completed.size(), 1u);
    EXPECT_TRUE(Session.Log.Completed[0].bSucceeded);
    EXPECT_EQ(Session.Log.Completed[0].Action, ESaveGameAction::Save);
    EXPECT_TRUE(CSaveGameLibrary::DoesSaveGameExist(Slot, 0));

    FRunningWorld Fresh;
    CSaveGameLibrary::RequestLoadGame(Fresh.Level.World, Slot, 0);
    Fresh.EndFrame();
    ASSERT_EQ(Fresh.Log.Completed.size(), 1u);
    const SSaveGameCompletedEvent& Loaded = Fresh.Log.Completed[0];
    EXPECT_TRUE(Loaded.bSucceeded);
    EXPECT_EQ(Loaded.Action, ESaveGameAction::Load);
    ASSERT_NE(Loaded.SaveGame.Get(), nullptr);
    EXPECT_EQ(static_cast<CSaveGameTest*>(Loaded.SaveGame.Get())->Score, 77);
    EXPECT_EQ(Fresh.Log.Restored, 1);
    EXPECT_EQ(Fresh.Listener->RestoredCount, 1);
    EXPECT_EQ(Fresh.Listener->LastSaveGame, Loaded.SaveGame.Get());
    EXPECT_EQ(Fresh.Level.Registry().Get<SSaveGameTestComponent>(Fresh.Level.Chest).Coins, 50);

    EXPECT_TRUE(CSaveGameLibrary::DeleteGameInSlot(Slot, 0));
    EXPECT_FALSE(CSaveGameLibrary::DoesSaveGameExist(Slot, 0));

    CSaveGameLibrary::RequestLoadGame(Fresh.Level.World, Slot, 0);
    Fresh.EndFrame();
    ASSERT_EQ(Fresh.Log.Completed.size(), 2u);
    EXPECT_FALSE(Fresh.Log.Completed[1].bSucceeded);
}
