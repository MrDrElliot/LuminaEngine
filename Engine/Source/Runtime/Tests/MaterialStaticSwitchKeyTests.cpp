#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/MaterialTypes.h"

namespace Lumina
{
    namespace
    {
        FMaterialStaticSwitch MakeSwitch(const char* Name, bool bDefault, uint8 Bit)
        {
            FMaterialStaticSwitch Switch;
            Switch.ParameterName = Name;
            Switch.bDefaultValue = bDefault;
            Switch.BitIndex      = Bit;
            return Switch;
        }

        CMaterial* MaterialWithSwitches()
        {
            CMaterial* Material = NewObject<CMaterial>();
            Material->StaticSwitches.push_back(MakeSwitch("Detail", false, 0));
            Material->StaticSwitches.push_back(MakeSwitch("Parallax", true, 1));
            Material->StaticSwitches.push_back(MakeSwitch("Tint", false, 2));
            return Material;
        }
    }

    TEST(MaterialStaticSwitchKey, TheDefaultKeySetsABitPerSwitchThatDefaultsOn)
    {
        CMaterial* Material = MaterialWithSwitches();

        // Only Parallax defaults true, and it owns bit 1.
        EXPECT_EQ(Material->GetDefaultStaticSwitchKey(), 0b010ull);
    }

    TEST(MaterialStaticSwitchKey, AnOverrideMovesOnlyItsOwnBit)
    {
        CMaterial* Material = MaterialWithSwitches();

        THashMap<FName, bool> Overrides;
        Overrides["Detail"] = true;
        EXPECT_EQ(Material->MakeStaticSwitchKey(Overrides), 0b011ull);

        Overrides["Parallax"] = false;
        EXPECT_EQ(Material->MakeStaticSwitchKey(Overrides), 0b001ull);
    }

    TEST(MaterialStaticSwitchKey, AnOverrideNamingNoSwitchOfThisMaterialIsIgnored)
    {
        CMaterial* Material = MaterialWithSwitches();

        THashMap<FName, bool> Overrides;
        Overrides["NotASwitchHere"] = true;

        EXPECT_EQ(Material->MakeStaticSwitchKey(Overrides), Material->GetDefaultStaticSwitchKey());
        EXPECT_EQ(Material->FindStaticSwitchBit("NotASwitchHere"), INDEX_NONE);
        EXPECT_EQ(Material->FindStaticSwitchBit("Tint"), 2);
    }

    TEST(MaterialStaticSwitchKey, AMaterialWithNoSwitchesHasOneEmptyPermutation)
    {
        CMaterial* Material = NewObject<CMaterial>();

        THashMap<FName, bool> Overrides;
        Overrides["Anything"] = true;

        EXPECT_EQ(Material->GetDefaultStaticSwitchKey(), 0ull);
        EXPECT_EQ(Material->MakeStaticSwitchKey(Overrides), 0ull);
    }
}
