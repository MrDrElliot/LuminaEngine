#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "ScriptTestUtil.h"

using namespace Lumina;

namespace
{
    Scripting::FManagedTypeDefinition DataAssetDefinition(const char* Name, std::initializer_list<const char*> Fields)
    {
        Scripting::FManagedTypeDefinition Definition;
        Definition.Kind = Scripting::EManagedTypeKind::ScriptableClass;
        Definition.TypeName = FName(Name);
        Definition.NativeBaseName = "CDataAsset";
        for (const char* Field : Fields)
        {
            Definition.Schema.Fields.Add(ScriptTest::ScalarField(Field, EPropertyTypeFlags::Int32));
        }
        Definition.bHasSchema = true;
        return Definition;
    }

    // The class stages of a reload and then its broadcast, which the host runs once the generation is live.
    void Reload(const TVector<Scripting::FManagedTypeDefinition>& Definitions, int32 Generation)
    {
        FScriptableRegistry::MintNewClasses(Definitions);
        FScriptableRegistry::RefreshMintedClasses(Definitions);
        RegisterPendingTypes();
        Scripting::FManagedTypeRegistry::Get().BroadcastReload(Generation, Definitions);
    }

    int32* FindInt(CObject* Object, const char* Field)
    {
        FProperty* Property = Object->GetClass()->GetProperty(FName(Field));
        return Property != nullptr ? Property->GetValuePtr<int32>(Object) : nullptr;
    }

    // Holds an asset the way an open editor does, and follows it the way every tool now can.
    struct FAssetHolder final : Scripting::IScriptReloadListener
    {
        TStrongObjectPtr<CObject> Held;
        int32 Heard = 0;

        void OnScriptsReloaded(const Scripting::FScriptReload& Reload) override
        {
            ++Heard;
            if (CObject* Replacement = Reload.FindReplacement(Held.Get()))
            {
                Held = Replacement;
            }
        }
    };
}

TEST(ScriptReload, AListenerFollowsADataAssetOntoItsReshapedClass)
{
    Reload({ DataAssetDefinition("Reload_GTestData", { "Health" }) }, 1);
    CClass* Before = FindObject<CClass>(FName("Reload_GTestData"));
    ASSERT_NE(Before, nullptr);

    CPackage* Package = CPackage::CreatePackage("/ScriptReload/DA_Hero");
    ASSERT_NE(Package, nullptr);
    CObject* Asset = NewObject(Before, Package, FName("DA_Hero"));
    ASSERT_NE(Asset, nullptr);
    *FindInt(Asset, "Health") = 42;

    FAssetHolder Holder;
    Holder.Held = Asset;

    Reload({ DataAssetDefinition("Reload_GTestData", { "Health", "Armor" }) }, 2);

    EXPECT_EQ(Holder.Heard, 1);
    CObject* Followed = Holder.Held.Get();
    ASSERT_NE(Followed, nullptr);
    EXPECT_NE(Followed, Asset) << "the holder moved onto the replacement instead of keeping the retired original";
    EXPECT_EQ(Followed->GetClass(), FindObject<CClass>(FName("Reload_GTestData")));
    ASSERT_NE(FindInt(Followed, "Health"), nullptr);
    EXPECT_EQ(*FindInt(Followed, "Health"), 42) << "the value survives the reshape";
    EXPECT_NE(FindInt(Followed, "Armor"), nullptr);
    EXPECT_EQ(Followed->GetPackage(), Package);
}

TEST(ScriptReload, ARetiredOriginalStillHeldNeverLandsInItsPackageSave)
{
    Reload({ DataAssetDefinition("Retired_GTestData", { "Value" }) }, 3);
    CClass* Before = FindObject<CClass>(FName("Retired_GTestData"));
    ASSERT_NE(Before, nullptr);

    CPackage* Package = CPackage::CreatePackage("/ScriptReload/DA_Retired");
    ASSERT_NE(Package, nullptr);
    CObject* Asset = NewObject(Before, Package, FName("DA_Retired"));
    ASSERT_NE(Asset, nullptr);

    // A holder that never heard of the reload keeps the original alive, as a stale tool would.
    TStrongObjectPtr<CObject> Stale(Asset);

    Reload({ DataAssetDefinition("Retired_GTestData", { "Value", "Extra" }) }, 4);

    TVector<CObject*> InPackage;
    GetObjectsWithPackage(Package, InPackage);
    ASSERT_EQ(InPackage.Num(), 1) << "only the replacement belongs to the package a save would write";
    EXPECT_NE(InPackage[0], Asset);
    EXPECT_EQ(InPackage[0]->GetName(), FName("DA_Retired"));
}

TEST(ScriptReload, AListenerGoneBeforeTheReloadIsNeverCalled)
{
    int32 Before = 0;
    {
        FAssetHolder Short;
        Scripting::FManagedTypeRegistry::Get().BroadcastReload(5, {});
        Before = Short.Heard;
    }
    EXPECT_EQ(Before, 1);

    FAssetHolder Survivor;
    Scripting::FManagedTypeRegistry::Get().BroadcastReload(6, {});
    EXPECT_EQ(Survivor.Heard, 1) << "only the live listener heard the second reload";
}
