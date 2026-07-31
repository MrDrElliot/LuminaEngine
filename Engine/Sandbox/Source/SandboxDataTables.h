#pragma once

#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "SandboxDataTables.generated.h"

/**
 * Row structs for testing data tables.
 *
 * A row type is any reflected struct with fields. Note what is NOT here: a struct-typed field such as
 * FVector3 or FColor. A game module's reflection run only parses its own headers, so a property typed
 * as an engine reflected struct emits a call to a Construct_CStruct symbol it never declared and the
 * module fails to link. Primitives, strings, names, enums, object references and arrays of those all
 * work; engine structs do not.
 */
namespace Lumina
{
    REFLECT()
    enum class SANDBOX_API EItemRarity : uint8
    {
        Common,
        Uncommon,
        Rare,
        Epic,
        Legendary,
    };

    REFLECT()
    enum class SANDBOX_API EItemSlot : uint8
    {
        None,
        Head,
        Chest,
        Hands,
        Legs,
        Feet,
        MainHand,
        OffHand,
    };

    /** An item definition, keyed by the row name (its item id).
     *
     *  Deliberately covers every kind of field a grid cell can hold -- text, numbers, a bool, two
     *  enums and asset references -- plus one array, which no cell can represent and which therefore
     *  only appears in the row details panel. That last field is the point: it is what proves the
     *  grid hiding a column does not make it uneditable.
     */
    REFLECT()
    struct SANDBOX_API SItemRow : public SDataTableRowBase
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Display")
        FString DisplayName;

        PROPERTY(Editable, Category = "Display")
        FString Description;

        PROPERTY(Editable, Category = "Display")
        EItemRarity Rarity = EItemRarity::Common;

        PROPERTY(Editable, Category = "Equipment")
        EItemSlot Slot = EItemSlot::None;

        /** How many fit in one inventory slot. 1 means the item does not stack. */
        PROPERTY(Editable, Category = "Inventory")
        int32 MaxStack = 1;

        PROPERTY(Editable, Category = "Inventory")
        float Weight = 1.0f;

        /** Base merchant price, before any modifiers. */
        PROPERTY(Editable, Category = "Inventory")
        int32 Value = 0;

        PROPERTY(Editable, Category = "Inventory")
        bool bConsumable = false;

        PROPERTY(Editable, Category = "Combat")
        float Damage = 0.0f;

        PROPERTY(Editable, Category = "Combat")
        float Armor = 0.0f;

        PROPERTY(Editable, Category = "Visuals")
        TObjectPtr<CTexture> Icon;

        PROPERTY(Editable, Category = "Visuals")
        TObjectPtr<CStaticMesh> WorldMesh;

        /** Offset from the holder's hand socket when equipped. */
        PROPERTY(Editable, Category = "Visuals")
        FVector3 PickupOffset;

        /** Free-form tags for filtering and loot rules. An array, so it has no grid column -- edit it
         *  in the row details panel. */
        PROPERTY(Editable, Category = "Gameplay")
        TVector<FName> Tags;
    };

    /** Per-level stats, keyed by the row name.
     *
     *  Deliberately all-numeric: this is the shape people actually author in a spreadsheet, so it is
     *  the honest test of CSV import and of numeric column sorting.
     */
    REFLECT()
    struct SANDBOX_API SLevelStatsRow : public SDataTableRowBase
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Progression")
        int32 Level = 1;

        PROPERTY(Editable, Category = "Progression")
        int32 ExperienceToNext = 100;

        PROPERTY(Editable, Category = "Stats")
        float MaxHealth = 100.0f;

        PROPERTY(Editable, Category = "Stats")
        float MaxStamina = 100.0f;

        PROPERTY(Editable, Category = "Stats")
        float BaseDamage = 10.0f;

        PROPERTY(Editable, Category = "Stats")
        float MoveSpeed = 5.0f;
    };
}
