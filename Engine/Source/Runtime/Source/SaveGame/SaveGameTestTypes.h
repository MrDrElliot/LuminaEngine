#pragma once

#include "SaveGame.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Scripting/EntityScript.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/World.h"
#include "SaveGameTestTypes.generated.h"

namespace Lumina
{
    // Throwaway types the save game tests read and write, since no engine type flags a SaveGame field yet.

    REFLECT()
    class RUNTIME_API CSaveGameTestObject : public CObject
    {
        GENERATED_BODY()

    public:

        PROPERTY(Hidden)
        FString Label;

        PROPERTY(Hidden)
        int32 Count = 0;

        PROPERTY(Hidden)
        TStrongObjectPtr<CSaveGameTestObject> Next;
    };

    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SSaveGameTestComponent
    {
        GENERATED_BODY()

        PROPERTY(Hidden, SaveGame)
        int32 Coins = 0;

        PROPERTY(Hidden, SaveGame)
        ECS::FEntity Target = ECS::NullEntity;

        PROPERTY(Hidden, SaveGame)
        TVector<int32> History;

        PROPERTY(Hidden, SaveGame)
        TStrongObjectPtr<CSaveGameTestObject> Inventory;

        PROPERTY(Hidden, SaveGame, NoSerialize)
        float RuntimeOnly = 0.0f;

        PROPERTY(Hidden)
        int32 NotSaved = 0;
    };

    REFLECT()
    class RUNTIME_API CSaveGameTestScript : public CEntityScript
    {
        GENERATED_BODY()

    public:

        void OnAttach() override
        {
            if (CWorld* World = GetWorld())
            {
                World->GetSaveGames()->OnRestored.AddMember(this, &CSaveGameTestScript::CountRestored);
                World->GetSaveGames()->OnSaving.AddMember(this, &CSaveGameTestScript::CountSaving);
            }
        }

        void CountRestored() { ++RestoredCount; }
        void CountSaving() { ++SavingCount; }

        PROPERTY(Hidden, SaveGame)
        int32 Level = 0;

        PROPERTY(Hidden)
        int32 NotSaved = 0;

        int32 RestoredCount = 0;
        int32 SavingCount = 0;
    };

    // Gated off, so discovery never puts one in a real world.
    REFLECT()
    class RUNTIME_API CSaveGameTestSystem : public CEntitySystem
    {
        GENERATED_BODY()

    public:

        bool ShouldCreate() override { return false; }
        void OnWorldSaving(CSaveGame* SaveGame) override { ++SavingCount; LastSaveGame = SaveGame; }
        void OnWorldRestored(CSaveGame* SaveGame) override { ++RestoredCount; LastSaveGame = SaveGame; }

        int32      SavingCount = 0;
        int32      RestoredCount = 0;
        CSaveGame* LastSaveGame = nullptr;
    };

    REFLECT()
    class RUNTIME_API CSaveGameTest : public CSaveGame
    {
        GENERATED_BODY()

    public:

        void OnBeforeSave() override { ++BeforeSaveCount; }
        void OnAfterLoad() override { ++AfterLoadCount; }

        PROPERTY(Hidden)
        int32 Score = 0;

        PROPERTY(Hidden)
        FString PlayerName;

        PROPERTY(Hidden)
        TStrongObjectPtr<CSaveGameTestObject> Item;

        PROPERTY(Hidden, SaveGame, NoSerialize)
        int32 SessionOnly = 0;

        int32 BeforeSaveCount = 0;
        int32 AfterLoadCount = 0;
    };
}
