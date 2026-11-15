#include <gtest/gtest.h>

#include "AssetRegistryTestAccess.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/Algorithm.h"
#include "Cooker/AssetCooker.h"
#include "Cooker/Graph/CookGraph.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Pak/PakArchive.h"
#include "Pak/PakWriter.h"
#include "ScopedTempMountTestUtil.h"
#include "World/World.h"
#include "World/ExternalEntities/ExternalEntities.h"
#include "World/Entity/Components/EntityGuidComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr FStringView kAlias = "/ExternalEntityCookTests";
    constexpr FStringView kWorldPath = "/ExternalEntityCookTests/Level.lasset";
    constexpr FStringView kCookedPath = "/ExternalEntityCookTests/Cooked/Level.lasset";

    class ExternalEntityCook : public testing::Test
    {
    protected:

        void SetUp() override
        {
            RegisterPendingTypes();

            // Saving and loading skip storages they cannot name, and these register on first use.
            STransformComponent::StaticStruct();
            SNameComponent::StaticStruct();
            SEntityGuidComponent::StaticStruct();
            SDefaultWorldSettings::StaticStruct();
            SBoxColliderComponent::StaticStruct();

            World = NewObject<CWorld>(CPackage::CreatePackage(kWorldPath), "Level");
            WorldGuid = World->GetGUID();
            FWorldTestAccess::SetWorldType(*World, EWorldType::Editor);
            FWorldTestAccess::CreateSingletonEntity(*World);
            for (int32 Index = 0; Index < 3; ++Index)
            {
                World->GetEntityRegistry().Emplace<SBoxColliderComponent>(World->ConstructEntity("Crate")).HalfExtent = FVector3(1.0f + Index, 1.0f, 1.0f);
            }
        }

        void SaveWorld(bool bExternalEntities) const
        {
            World->GetDefaultWorldSettings().bExternalEntities = bExternalEntities;
            ASSERT_TRUE(CPackage::SavePackage(World->GetPackage(), kWorldPath));
            FAssetRegistry::Get().AssetSaved(World);
        }

        void TearDown() override
        {
            FAssetRegistry& Registry = FAssetRegistry::Get();
            for (const FAssetDataPtr& File : Registry.GetEntityFilesOf(kWorldPath))
            {
                Registry.AssetDeleted(File->AssetGUID);
            }
            Registry.AssetDeleted(WorldGuid);
            Unload(kWorldPath);
            Unload(kCookedPath);
        }

        static void Unload(FStringView Path)
        {
            if (CPackage* Package = CPackage::FindPackageByPath(Path))
            {
                Package->Unload();
            }
        }

        // Unloaded first, so the cook loads the world and its entity files from disk as a cook in a fresh editor does.
        void UnloadWorld()
        {
            Unload(kWorldPath);
            World = nullptr;
        }

        // Saved away from the entity folder, as in a pak, so the cooked world can only find entities it holds itself.
        static TVector<FString> LoadCookedCrates(const TVector<uint8>& Cooked)
        {
            EXPECT_TRUE(VFS::WriteFile(kCookedPath, TSpan<const uint8>(Cooked.GetData(), Cooked.Num())));
            CPackage* Package = CPackage::LoadPackage(kCookedPath);
            CWorld* CookedWorld = Package != nullptr ? Cast<CWorld>(Package->LoadObjectByName("Level")) : nullptr;
            EXPECT_NE(CookedWorld, nullptr);
            return CookedWorld != nullptr ? Crates(FWorldTestAccess::PendingRegistry(*CookedWorld)) : TVector<FString>();
        }

        // The world's bytes as a pak ships them, read back from a written pak.
        TVector<uint8> Cook() const
        {
            FCookNode Node;
            Node.Path = kWorldPath;
            Node.AssetClass = CWorld::StaticClass()->GetName();
            FPakWriter Writer;
            FCookResult Result;
            BundleChunkAssets(Writer, { &Node }, Result, {});
            Unload(kWorldPath);

            const FString PakPath = Mount.Dir + "/Cooked.pak";
            EXPECT_TRUE(Writer.Finalize(PakPath));
            const TSharedPtr<FPakArchive> Pak = FPakArchive::Open(PakPath);
            const TSpan<const uint8> Bytes = Pak != nullptr ? Pak->ReadEntry(kWorldPath) : TSpan<const uint8>();
            return TVector<uint8>(Bytes.begin(), Bytes.end());
        }

        // Each crate's guid and half extent, sorted so two worlds compare whatever order their entities load in.
        static TVector<FString> Crates(ECS::FRegistry& Registry)
        {
            TVector<FString> Out;
            Registry.View<SBoxColliderComponent>().ForEach([&](ECS::FEntity Entity, const SBoxColliderComponent& Box)
            {
                Out.Add(Format("{} {}", ExternalEntities::GuidOf(Registry, Entity), Box.HalfExtent.x));
            });
            Algo::Sort(Out, [](const FString& A, const FString& B) { return A.View() < B.View(); });
            return Out;
        }

        TestUtil::FScopedTempMount Mount{ kAlias, "LuminaExternalEntityCookTests" };
        CWorld* World = nullptr;
        FGuid WorldGuid;
    };
}

TEST_F(ExternalEntityCook, TheCookedWorldLoadsWithEveryEntity)
{
    SaveWorld(true);
    const TVector<FString> Expected = Crates(World->GetEntityRegistry());
    ASSERT_EQ(Expected.Num(), 3);
    UnloadWorld();

    EXPECT_EQ(LoadCookedCrates(Cook()), Expected);
}

TEST_F(ExternalEntityCook, ASingleFileWorldCooksAndListsAsBefore)
{
    SaveWorld(false);
    const TVector<FString> Expected = Crates(World->GetEntityRegistry());
    UnloadWorld();

    EXPECT_FALSE(VFS::Exists(ExternalEntities::EntitiesDirectory(kWorldPath)));
    EXPECT_TRUE(FAssetRegistry::Get().GetEntityFilesOf(kWorldPath).IsEmpty());
    EXPECT_NE(FAssetRegistry::Get().GetAssetByPath(kWorldPath), nullptr) << "the content browser lists the world";

    TVector<uint8> Stripped;
    ASSERT_TRUE(CPackage::StripPackageForCook(kWorldPath, Stripped));
    const TVector<uint8> Cooked = Cook();
    EXPECT_EQ(Cooked, Stripped) << "a single-file world still cooks from its file without a load";
    EXPECT_EQ(LoadCookedCrates(Cooked), Expected);
}

TEST_F(ExternalEntityCook, EditingOneEntityFileMissesTheCookCache)
{
    SaveWorld(true);
    const TVector<FAssetDataPtr> Files = FAssetRegistry::Get().GetEntityFilesOf(kWorldPath);
    ASSERT_EQ(Files.Num(), 3);
    TVector<TVector<uint8>> Original;
    for (const FAssetDataPtr& File : Files)
    {
        ASSERT_TRUE(VFS::ReadFile(Original.EmplaceGetRef(), File->Path));
    }

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Crate = *Registry.View<SBoxColliderComponent>().begin();
    Registry.Get<SBoxColliderComponent>(Crate).HalfExtent = FVector3(9.0f, 9.0f, 9.0f);
    ExternalEntities::MarkChanged(World, { Crate });
    ASSERT_TRUE(ExternalEntities::SaveChanges(World));
    UnloadWorld();
    const TVector<uint8> Edited = Cook();
    ASSERT_FALSE(Edited.IsEmpty());

    // The world's own file never changed, so only the entity files can tell the two cooks apart.
    for (int32 Index = 0; Index < Files.Num(); ++Index)
    {
        ASSERT_TRUE(VFS::WriteFile(Files[Index]->Path, TSpan<const uint8>(Original[Index].GetData(), Original[Index].Num())));
        FAssetRegistryTestAccess::ProcessPackagePath(FAssetRegistry::Get(), Files[Index]->Path);
    }
    EXPECT_NE(Cook(), Edited);
}

TEST_F(ExternalEntityCook, EntityFilesAreNeverPackaged)
{
    SaveWorld(true);
    const TVector<FAssetDataPtr> Files = FAssetRegistry::Get().GetEntityFilesOf(kWorldPath);
    ASSERT_FALSE(Files.IsEmpty());
    for (const FAssetDataPtr& File : Files)
    {
        EXPECT_TRUE(IsDevelopmentOnlyGameFile(File->Path, false)) << *File->Path;
    }
    EXPECT_FALSE(IsDevelopmentOnlyGameFile(kWorldPath, false));
}
