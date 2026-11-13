#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Containers/HashTable.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Threading/Thread.h"
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
            Blob.Reserve(Words);
            for (uint32 i = 0; i < Words; ++i)
            {
                Blob.Add(Seed * 2246822519u + i);
            }
            return Blob;
        }

        struct FScopedShaderLibrary
        {
            FShaderLibrary  Library;
            FShaderLibrary* Previous = nullptr;

            FScopedShaderLibrary()
            {
                Previous       = Global::GShaderLibrary;
                Global::GShaderLibrary = &Library;
            }
            ~FScopedShaderLibrary()
            {
                Global::GShaderLibrary = Previous;
            }
        };

        SMaterialStaticSwitch MakeSwitch(const char* Name, bool bDefault, uint8 Bit)
        {
            SMaterialStaticSwitch Switch;
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
            Material->StaticSwitches.Add(MakeSwitch("Detail", false, 0));
            Material->StaticSwitches.Add(MakeSwitch("Parallax", true, 1));
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
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(Blob.GetData(), Blob.Num()));

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
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(Base.GetData(), Base.Num()));

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(3);
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.GetData(), Perm.Num()));

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
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(Base.GetData(), Base.Num()));

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
        Material->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(BasePixel.GetData(), BasePixel.Num()));
        Material->CommitShaderStage(EMaterialShaderStage::MeshShadow, TSpan<const uint32>(BaseMesh.GetData(), BaseMesh.Num()));

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(7);
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.GetData(), Perm.Num()));

        EXPECT_NE(Material->GetStageForKey(EMaterialShaderStage::Pixel, Key), Material->GetPixelShader());
        EXPECT_EQ(Material->GetStageForKey(EMaterialShaderStage::MeshShadow, Key), Material->GetStage(EMaterialShaderStage::MeshShadow));
    }

    TEST(MaterialPermutation, CommittingAPermutationMovesTheShaderRevision)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(8);

        const uint32 Before = Material->GetShaderRevision();
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.GetData(), Perm.Num()));
        const uint32 AfterFirst = Material->GetShaderRevision();
        EXPECT_NE(Before, AfterFirst);

        // Content-keyed, so recommitting the same bytecode supersedes nothing.
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.GetData(), Perm.Num()));
        EXPECT_EQ(AfterFirst, Material->GetShaderRevision());
    }

    TEST(MaterialPermutation, ClearingDropsThePermutationAndItsStageBinaries)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = MakeSwitchedMaterial();
        ASSERT_NE(Material, nullptr);

        const uint64          Key  = Material->GetDefaultStaticSwitchKey() ^ 1ull;
        const TVector<uint32> Perm = MakeBlob(9);
        Material->CommitPermutationStage(Key, EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.GetData(), Perm.Num()));

        ASSERT_TRUE(Material->HasPermutation(Key));
        EXPECT_FALSE(Material->GetPermutationStageBinaries(Key, EMaterialShaderStage::Pixel).IsEmpty());

        Material->ClearPermutation(Key);
        EXPECT_FALSE(Material->HasPermutation(Key));
        EXPECT_TRUE(Material->GetPermutationStageBinaries(Key, EMaterialShaderStage::Pixel).IsEmpty());
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
        Material->CommitPermutationStage(KeyA, EMaterialShaderStage::Pixel, TSpan<const uint32>(BlobA.GetData(), BlobA.Num()));
        Material->CommitPermutationStage(KeyB, EMaterialShaderStage::Pixel, TSpan<const uint32>(BlobB.GetData(), BlobB.Num()));
        ASSERT_EQ(Material->Permutations.Num(), 2);

        Material->ClearPermutations();
        EXPECT_TRUE(Material->Permutations.IsEmpty());
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
            TSpan<const uint32>(Perm.GetData(), Perm.Num())));
        EXPECT_FALSE(Material->HasPermutation(Key));

        EXPECT_TRUE(Material->CommitPermutationStageIfCurrent(Key, Material->GetPermutationGeneration(),
            EMaterialShaderStage::Pixel, TSpan<const uint32>(Perm.GetData(), Perm.Num())));
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

    // One compile dispatches a shader per stage, and every worker calls back on the same material.
    TEST(MaterialPermutation, ConcurrentStageCommitsAllLand)
    {
        FScopedShaderLibrary Scope;

        constexpr int32 kStageCount = (int32)EMaterialShaderStage::Count;

        CMaterial* Material = NewObject<CMaterial>();
        ASSERT_NE(Material, nullptr);

        TVector<TVector<uint32>> Blobs;
        for (size_t s = 0; s < kStageCount; ++s)
        {
            Blobs.Add(MakeBlob((uint32)s + 11u, 16u + (uint32)s));
        }

        TAtomic<uint32> Start{0};

        TVector<TUniquePtr<FThread>> Threads;
        for (int32 s = 0; s < kStageCount; ++s)
        {
            Threads.Add(MakeUnique<FThread>([&Material, &Blobs, &Start, s]()
            {
                while (Start.load(std::memory_order_acquire) == 0)
                {
                    Threading::ThreadYield();
                }
                Material->SetStageBinaries((EMaterialShaderStage)s,
                    TSpan<const uint32>(Blobs[s].GetData(), Blobs[s].Num()));
            }));
        }

        Start.store(1, std::memory_order_release);
        for (TUniquePtr<FThread>& Thread : Threads)
        {
            Thread->Join();
        }

        for (int32 s = 0; s < kStageCount; ++s)
        {
            const TVector<uint32>& Stored = Material->GetShaderStageBinaries((EMaterialShaderStage)s);
            EXPECT_EQ(Stored.Num(), Blobs[s].Num()) << "stage " << s << " lost its binaries";
            EXPECT_TRUE(Stored == Blobs[s]) << "stage " << s << " stored another stage's binaries";
        }
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
