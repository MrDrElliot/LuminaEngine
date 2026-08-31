#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Containers/HashTable.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"

// An instance selects a shader set by static-switch key, and the master owns every set.

namespace Lumina
{
    namespace
    {
        TVector<uint32> MakeBlob(uint32 Seed, uint32 Words = 8)
        {
            TVector<uint32> Blob;
            Blob.reserve(Words);
            for (uint32 i = 0; i < Words; ++i)
            {
                Blob.push_back(Seed * 2246822519u + i);
            }
            return Blob;
        }

        struct FScopedShaderLibrary
        {
            FShaderLibrary  Library;
            FShaderLibrary* Previous = nullptr;

            FScopedShaderLibrary()
            {
                Previous       = GShaderLibrary;
                GShaderLibrary = &Library;
            }
            ~FScopedShaderLibrary()
            {
                GShaderLibrary = Previous;
            }
        };

        FMaterialStaticSwitch MakeSwitch(const char* Name, bool bDefault, uint8 Bit)
        {
            FMaterialStaticSwitch Switch;
            Switch.ParameterName = Name;
            Switch.bDefaultValue = bDefault;
            Switch.BitIndex      = Bit;
            return Switch;
        }

        // Detail defaults off and Parallax on, so the default key is 0b010.
        CMaterial* MakeSwitchedMaterial()
        {
            CMaterial* Material = NewObject<CMaterial>();
            if (Material == nullptr)
            {
                return nullptr;
            }
            Material->StaticSwitches.push_back(MakeSwitch("Detail", false, 0));
            Material->StaticSwitches.push_back(MakeSwitch("Parallax", true, 1));
            return Material;
        }

        // Parented by assignment, since SetParentMaterial refreshes a GPU slot this fixture has none of.
        CMaterialInstance* MakeInstanceOf(CMaterialInterface* Parent)
        {
            CMaterialInstance* Instance = NewObject<CMaterialInstance>();
            if (Instance != nullptr)
            {
                Instance->Material = Parent;
            }
            return Instance;
        }
    }

    TEST(MaterialPermutation, TheDefaultKeyResolvesToTheMaterialsOwnStages)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const TVector<uint32> Blob = MakeBlob(1);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(Blob.data(), Blob.size()));

        const uint64 DefaultKey = Material->GetDefaultStaticSwitchKey();
        EXPECT_TRUE(Material->HasPermutation(DefaultKey));
        EXPECT_EQ(Material->GetStageForKey(EMaterialShaderStage::Pixel, DefaultKey), Material->GetPixelShader());
    }

    TEST(MaterialPermutation, ACommittedPermutationResolvesItsOwnStageInsteadOfTheMasters)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const TVector<uint32> Base = MakeBlob(2);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(Base.data(), Base.size()));

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(3);
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.data(), Perm.size()));

        ASSERT_TRUE(Material->HasPermutation(Key));
        EXPECT_NE(Material->GetStageForKey(EMaterialShaderStage::Pixel, Key), Material->GetPixelShader());
        EXPECT_EQ(Material->GetStageForKey(EMaterialShaderStage::Pixel, Material->GetDefaultStaticSwitchKey()),
                  Material->GetPixelShader());
    }

    // The fallback is what lets an instance draw at all while its permutation is still compiling.
    TEST(MaterialPermutation, AnUncompiledKeyFallsBackToTheMastersStage)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const TVector<uint32> Base = MakeBlob(4);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(Base.data(), Base.size()));

        const uint64 MissingKey = Material->GetDefaultStaticSwitchKey() ^ 3ull;
        EXPECT_FALSE(Material->HasPermutation(MissingKey));
        EXPECT_EQ(Material->GetStageForKey(EMaterialShaderStage::Pixel, MissingKey), Material->GetPixelShader());
    }

    // A permutation that compiled only some stages falls back on the rest, never resolving null.
    TEST(MaterialPermutation, AStageThePermutationDidNotBuildFallsBackToTheMaster)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const TVector<uint32> BasePixel = MakeBlob(5);
        const TVector<uint32> BaseMesh  = MakeBlob(6);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(BasePixel.data(), BasePixel.size()));
        Material->CommitShaderStage(EMaterialShaderStage::MeshShadow, TSpan<const uint32>(BaseMesh.data(), BaseMesh.size()));

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(7);
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.data(), Perm.size()));

        EXPECT_NE(Material->GetStageForKey(EMaterialShaderStage::Pixel, Key), Material->GetPixelShader());
        EXPECT_EQ(Material->GetStageForKey(EMaterialShaderStage::MeshShadow, Key), Material->GetMeshShaderShadow());
    }

    TEST(MaterialPermutation, CommittingAPermutationMovesTheShaderRevision)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(8);

        const uint32 Before = Material->GetShaderRevision();
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.data(), Perm.size()));
        const uint32 AfterFirst = Material->GetShaderRevision();
        EXPECT_NE(Before, AfterFirst);

        // Content-keyed, so recommitting the same bytecode supersedes nothing.
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.data(), Perm.size()));
        EXPECT_EQ(AfterFirst, Material->GetShaderRevision());
    }

    TEST(MaterialPermutation, ClearingDropsThePermutationAndItsStageBinaries)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(9);
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.data(), Perm.size()));

        ASSERT_TRUE(Material->HasPermutation(Key));
        EXPECT_FALSE(Material->GetPermutationStageBinaries(Key, EMaterialShaderStage::Pixel).empty());

        Material->ClearPermutation(Key);
        EXPECT_FALSE(Material->HasPermutation(Key));
        EXPECT_TRUE(Material->GetPermutationStageBinaries(Key, EMaterialShaderStage::Pixel).empty());
    }

    // A recompile renumbers switch bits by name, so every key minted against the old manifest goes.
    TEST(MaterialPermutation, ClearPermutationsDropsEveryKey)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const uint64          KeyA  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const uint64          KeyB  = Material->GetDefaultStaticSwitchKey() ^ 2ull;
        const TVector<uint32> BlobA = MakeBlob(10);
        const TVector<uint32> BlobB = MakeBlob(11);
        Material->CommitPermutationStage(KeyA, EMaterialShaderStage::Pixel, TSpan<const uint32>(BlobA.data(), BlobA.size()));
        Material->CommitPermutationStage(KeyB, EMaterialShaderStage::Pixel, TSpan<const uint32>(BlobB.data(), BlobB.size()));
        ASSERT_EQ(Material->Permutations.size(), (size_t)2);

        Material->ClearPermutations();
        EXPECT_TRUE(Material->Permutations.empty());
        EXPECT_FALSE(Material->HasPermutation(KeyA));
        EXPECT_FALSE(Material->HasPermutation(KeyB));
    }

    // A permutation dispatched before a recompile must not land, since its key names the old numbering.
    TEST(MaterialPermutation, ACommitFromBeforeARecompileIsRefused)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const uint64 Key            = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const uint32 DispatchedUnder = Material->GetPermutationGeneration();
        const TVector<uint32> Perm  = MakeBlob(12);

        Material->ClearPermutations();
        EXPECT_NE(Material->GetPermutationGeneration(), DispatchedUnder);

        EXPECT_FALSE(Material->CommitPermutationStageIfCurrent(Key, DispatchedUnder, EMaterialShaderStage::Pixel,
            TSpan<const uint32>(Perm.data(), Perm.size())));
        EXPECT_FALSE(Material->HasPermutation(Key));

        EXPECT_TRUE(Material->CommitPermutationStageIfCurrent(Key, Material->GetPermutationGeneration(),
            EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.data(), Perm.size())));
        EXPECT_TRUE(Material->HasPermutation(Key));
    }

    // Two instances flipping the same switches land on one key, which is what collapses their draws.
    TEST(MaterialPermutation, TwoInstancesFlippingTheSameSwitchShareOneKey)
    {
        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        CMaterialInstance* A = MakeInstanceOf(Material);
        CMaterialInstance* B = MakeInstanceOf(Material);
        ASSERT_NE(A, nullptr);
        ASSERT_NE(B, nullptr);

        ASSERT_TRUE(A->SetStaticSwitchValue("Detail", true));
        ASSERT_TRUE(B->SetStaticSwitchValue("Detail", true));

        EXPECT_EQ(A->GetStaticSwitchKey(), B->GetStaticSwitchKey());
        EXPECT_NE(A->GetStaticSwitchKey(), Material->GetDefaultStaticSwitchKey());
    }

    TEST(MaterialPermutation, AnInstanceOverridingNothingSelectsTheDefaultKey)
    {
        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        CMaterialInstance* Instance = MakeInstanceOf(Material);
        ASSERT_NE(Instance, nullptr);

        EXPECT_EQ(Instance->GetStaticSwitchKey(), Material->GetDefaultStaticSwitchKey());
        EXPECT_FALSE(Instance->GetStaticSwitchValue("Detail"));
        EXPECT_TRUE(Instance->GetStaticSwitchValue("Parallax"));
    }

    TEST(MaterialPermutation, SettingASwitchTheRootDoesNotDeclareIsRefused)
    {
        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        CMaterialInstance* Instance = MakeInstanceOf(Material);
        ASSERT_NE(Instance, nullptr);

        EXPECT_FALSE(Instance->SetStaticSwitchValue("NotASwitchHere", true));
        EXPECT_FALSE(Instance->HasStaticSwitchOverride("NotASwitchHere"));
        EXPECT_EQ(Instance->GetStaticSwitchKey(), Material->GetDefaultStaticSwitchKey());
    }

    TEST(MaterialPermutation, RemovingAnOverrideReturnsTheInstanceToTheDefaultKey)
    {
        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        CMaterialInstance* Instance = MakeInstanceOf(Material);
        ASSERT_NE(Instance, nullptr);

        ASSERT_TRUE(Instance->SetStaticSwitchValue("Parallax", false));
        ASSERT_TRUE(Instance->HasStaticSwitchOverride("Parallax"));
        EXPECT_NE(Instance->GetStaticSwitchKey(), Material->GetDefaultStaticSwitchKey());

        Instance->RemoveStaticSwitchOverride("Parallax");
        EXPECT_FALSE(Instance->HasStaticSwitchOverride("Parallax"));
        EXPECT_EQ(Instance->GetStaticSwitchKey(), Material->GetDefaultStaticSwitchKey());
    }

    // A chained instance inherits what it does not override, and overrides what it does.
    TEST(MaterialPermutation, AChildInheritsItsParentsSwitchesAndOverridesOnTop)
    {
        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        CMaterialInstance* Parent = MakeInstanceOf(Material);
        ASSERT_NE(Parent, nullptr);
        CMaterialInstance* Child = MakeInstanceOf(Parent);
        ASSERT_NE(Child, nullptr);

        ASSERT_TRUE(Parent->SetStaticSwitchValue("Detail", true));
        ASSERT_TRUE(Parent->SetStaticSwitchValue("Parallax", false));
        ASSERT_TRUE(Child->SetStaticSwitchValue("Parallax", true));

        EXPECT_TRUE(Child->GetStaticSwitchValue("Detail")) << "Detail is inherited from the parent";
        EXPECT_TRUE(Child->GetStaticSwitchValue("Parallax")) << "Parallax is overridden at this level";

        THashMap<FName, bool> Expected;
        Expected["Detail"]   = true;
        Expected["Parallax"] = true;
        EXPECT_EQ(Child->GetStaticSwitchKey(), Material->MakeStaticSwitchKey(Expected));
        EXPECT_NE(Child->GetStaticSwitchKey(), Parent->GetStaticSwitchKey());
    }

    // A material with no switches must not pay for any of this, and keys as zero.
    TEST(MaterialPermutation, AMaterialWithNoSwitchesKeysAsZero)
    {
        CMaterial* Material = NewObject<CMaterial>();
        ASSERT_NE(Material, nullptr);

        CMaterialInstance* Instance = MakeInstanceOf(Material);
        ASSERT_NE(Instance, nullptr);

        EXPECT_EQ(Material->GetStaticSwitchKey(), 0ull);
        EXPECT_EQ(Instance->GetStaticSwitchKey(), 0ull);
        EXPECT_TRUE(Material->HasPermutation(0ull));
    }
}
