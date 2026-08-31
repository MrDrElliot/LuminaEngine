#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Material/MaterialParameterCollection.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/MaterialTypes.h"

// A collection is addressed by position, so what the graph compiles has to match what the asset declares.

namespace Lumina
{
    namespace
    {
        CMaterialParameterCollection* MakeCollection()
        {
            CMaterialParameterCollection* Collection = NewObject<CMaterialParameterCollection>();
            if (Collection == nullptr)
            {
                return nullptr;
            }

            FCollectionScalarParameter Wetness;
            Wetness.ParameterName = "Wetness";
            Wetness.DefaultValue  = 0.25f;
            Collection->ScalarParameters.push_back(Wetness);

            FCollectionScalarParameter Snow;
            Snow.ParameterName = "SnowCoverage";
            Snow.DefaultValue  = 0.5f;
            Collection->ScalarParameters.push_back(Snow);

            FCollectionVectorParameter Tint;
            Tint.ParameterName = "SeasonTint";
            Tint.DefaultValue  = FVector4(0.1f, 0.2f, 0.3f, 1.0f);
            Collection->VectorParameters.push_back(Tint);

            // Headless, so this only replays the declared defaults into the block.
            Collection->PostLoad();
            return Collection;
        }
    }

    // Position is the index the shader compiles in, so a lookup that disagrees samples another parameter.
    TEST(MaterialParameterCollection, LookupReturnsTheDeclarationOrder)
    {
        CMaterialParameterCollection* Collection = MakeCollection();
        ASSERT_NE(Collection, nullptr);

        EXPECT_EQ(Collection->FindScalarIndex("Wetness"), 0);
        EXPECT_EQ(Collection->FindScalarIndex("SnowCoverage"), 1);
        EXPECT_EQ(Collection->FindVectorIndex("SeasonTint"), 0);

        EXPECT_EQ(Collection->FindScalarIndex("NotDeclared"), INDEX_NONE);
        EXPECT_EQ(Collection->FindVectorIndex("Wetness"), INDEX_NONE) << "a scalar is not a vector";
    }

    TEST(MaterialParameterCollection, DeclaredDefaultsAreReadableBeforeAnythingSetsThem)
    {
        CMaterialParameterCollection* Collection = MakeCollection();
        ASSERT_NE(Collection, nullptr);

        EXPECT_FLOAT_EQ(Collection->GetScalarValue("Wetness"), 0.25f);
        EXPECT_FLOAT_EQ(Collection->GetScalarValue("SnowCoverage"), 0.5f);
        EXPECT_EQ(Collection->GetVectorValue("SeasonTint"), FVector4(0.1f, 0.2f, 0.3f, 1.0f));
    }

    TEST(MaterialParameterCollection, SettingAValueSticksAndLeavesTheRestAlone)
    {
        CMaterialParameterCollection* Collection = MakeCollection();
        ASSERT_NE(Collection, nullptr);

        EXPECT_TRUE(Collection->SetScalarValue("Wetness", 0.9f));
        EXPECT_FLOAT_EQ(Collection->GetScalarValue("Wetness"), 0.9f);
        EXPECT_FLOAT_EQ(Collection->GetScalarValue("SnowCoverage"), 0.5f);

        EXPECT_TRUE(Collection->SetVectorValue("SeasonTint", FVector4(1.0f, 0.0f, 0.0f, 1.0f)));
        EXPECT_EQ(Collection->GetVectorValue("SeasonTint"), FVector4(1.0f, 0.0f, 0.0f, 1.0f));
    }

    TEST(MaterialParameterCollection, AnUndeclaredNameIsRefusedRatherThanCreated)
    {
        CMaterialParameterCollection* Collection = MakeCollection();
        ASSERT_NE(Collection, nullptr);

        EXPECT_FALSE(Collection->SetScalarValue("NotDeclared", 1.0f));
        EXPECT_FALSE(Collection->SetVectorValue("NotDeclared", FVector4(1.0f)));
        EXPECT_FLOAT_EQ(Collection->GetScalarValue("NotDeclared", 7.0f), 7.0f);
        EXPECT_FALSE(Collection->HasScalarParameter("NotDeclared"));
        EXPECT_TRUE(Collection->HasScalarParameter("Wetness"));
        EXPECT_TRUE(Collection->HasVectorParameter("SeasonTint"));
    }

    // An edit reorders or renames, and the block has to describe the declaration order after it.
    TEST(MaterialParameterCollection, EditingTheDeclarationReplaysTheDefaults)
    {
        CMaterialParameterCollection* Collection = MakeCollection();
        ASSERT_NE(Collection, nullptr);

        ASSERT_TRUE(Collection->SetScalarValue("Wetness", 0.9f));

        Collection->ScalarParameters[0].DefaultValue = 0.75f;
        Collection->PostPropertyChange(nullptr);

        EXPECT_FLOAT_EQ(Collection->GetScalarValue("Wetness"), 0.75f)
            << "a declaration edit is authoritative over a live value";
    }

    // The two words the material block reserved are what carries a binding, so the cap follows from them.
    TEST(MaterialParameterCollection, TheMaterialBlockCarriesExactlyTheBudgetedBindings)
    {
        EXPECT_EQ((uint32)MAX_MATERIAL_COLLECTIONS, 2u);
        EXPECT_EQ(sizeof(FMaterialUniforms), 592u) << "the collection indices reused reserved words";
        static_assert(sizeof(FMaterialUniforms) % 16 == 0, "block stride must stay 16-byte aligned");

        CMaterial* Material = NewObject<CMaterial>();
        ASSERT_NE(Material, nullptr);
        EXPECT_TRUE(Material->ParameterCollections.empty());

        // Slot 0 is the reserved zero collection, so an unbound material reads zeros with no sentinel.
        for (uint32 i = 0; i < MAX_MATERIAL_COLLECTIONS; ++i)
        {
            EXPECT_EQ(Material->MaterialUniforms.CollectionIndices[i], 0u);
        }
    }

    TEST(MaterialParameterCollection, TheCollectionBlockMatchesTheDeclaredBudget)
    {
        EXPECT_EQ(sizeof(FMaterialCollectionUniforms),
                  sizeof(FVector4) * MAX_COLLECTION_VECTORS + sizeof(float) * MAX_COLLECTION_SCALARS);
        EXPECT_EQ(CollectionVectorFieldOffset(0), 0u);
        EXPECT_EQ(CollectionVectorFieldOffset(3), 3u * sizeof(FVector4));
        EXPECT_EQ(CollectionScalarFieldOffset(0), sizeof(FVector4) * MAX_COLLECTION_VECTORS);
        EXPECT_EQ(CollectionScalarFieldOffset(2),
                  sizeof(FVector4) * MAX_COLLECTION_VECTORS + 2u * sizeof(float));
    }
}
