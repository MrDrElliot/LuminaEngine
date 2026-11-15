#include <gtest/gtest.h>

#include "AssetRegistryTestAccess.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Containers/Algorithm.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"
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
    constexpr const char* kAlias = "/ExternalEntityRegistryTests";

    class ExternalEntityRegistry : public testing::Test
    {
    protected:

        void SetUp() override
        {
            RegisterPendingTypes();
            VFS::Unmount(kAlias);
            VFS::Mount<VFS::FMemoryFileSystem>(kAlias);

            // Saving and loading skip storages they cannot name, and these register on first use.
            STransformComponent::StaticStruct();
            SNameComponent::StaticStruct();
            SEntityGuidComponent::StaticStruct();
            SDefaultWorldSettings::StaticStruct();
            SBoxColliderComponent::StaticStruct();

            World = NewObject<CWorld>(CPackage::CreatePackage(PackagePath("Level")), "Level");
            FWorldTestAccess::SetWorldType(*World, EWorldType::Editor);
            FWorldTestAccess::CreateSingletonEntity(*World);
            World->GetDefaultWorldSettings().bExternalEntities = true;
        }

        void TearDown() override
        {
            FAssetRegistry& Registry = FAssetRegistry::Get();
            Registry.AssetDeleted(World->GetGUID());
            for (const FString& File : EntityFiles())
            {
                Registry.AssetDeleted(FGuid::NewDeterministic(VFS::FileName(File, true)));
            }
            World->GetPackage()->Unload();
            for (const TStrongObjectPtr<CPhysicsMaterial>& Material : Materials)
            {
                Material->GetPackage()->Unload();
            }
            VFS::Unmount(kAlias);
        }

        static FString PackagePath(FStringView Name)
        {
            return Format("{}/{}/{}.lasset", kAlias, testing::UnitTest::GetInstance()->current_test_info()->name(), Name);
        }

        CPhysicsMaterial* NewMaterial(FStringView Name)
        {
            const FString Path = PackagePath(Name);
            CPackage* Package = CPackage::CreatePackage(Path);
            CPhysicsMaterial* Material = Materials.EmplaceGetRef(NewObject<CPhysicsMaterial>(Package, FName(Name))).Get();
            EXPECT_TRUE(CPackage::SavePackage(Package, Path));
            return Material;
        }

        ECS::FEntity NewCrate(CPhysicsMaterial* Material)
        {
            const ECS::FEntity Crate = World->ConstructEntity("Crate");
            World->GetEntityRegistry().Emplace<SBoxColliderComponent>(Crate).PhysicsMaterial = Material;
            return Crate;
        }

        // A first save writes the world package with every entity file, then lists the world as the world editor does.
        bool SaveWhole() const
        {
            const bool bSaved = CPackage::SavePackage(World->GetPackage(), PackagePath("Level"));
            FAssetRegistry::Get().AssetSaved(World);
            return bSaved;
        }

        static TVector<FString> EntityFiles()
        {
            TVector<FString> Files;
            VFS::RecursiveDirectoryIterator(kAlias, [&](VFS::FFileInfo& Info)
            {
                if (!Info.IsDirectory() && !ExternalEntities::OwningWorldPath(Info.VirtualPath).IsEmpty())
                {
                    Files.Add(Info.VirtualPath);
                }
            });
            return Files;
        }

        TVector<FGuid> ReferencersOf(const CObject* Asset) const
        {
            TVector<FGuid> Guids;
            for (const FAssetDataPtr& Referencer : FAssetRegistry::Get().GetReferencersOf(Asset->GetGUID()))
            {
                Guids.Add(Referencer->AssetGUID);
            }
            return Guids;
        }

        bool WorldDependsOn(const CObject* Asset) const
        {
            const FAssetDataPtr Listed = FAssetRegistry::Get().GetAssetByGUID(World->GetGUID());
            return Listed != nullptr && Algo::AnyOf(Listed->Dependencies, [&](const FAssetDependency& Dependency)
            {
                return Dependency.TargetGUID == Asset->GetGUID();
            });
        }

        CWorld* World = nullptr;
        TVector<TStrongObjectPtr<CPhysicsMaterial>> Materials;
    };
}

TEST_F(ExternalEntityRegistry, EntityFilesAreNotListedAsAssets)
{
    NewCrate(NewMaterial("Material"));
    ASSERT_TRUE(SaveWhole());
    const TVector<FString> Files = EntityFiles();
    ASSERT_EQ(Files.Num(), 1);

    // Discovery reads every package under a content root, entity files included.
    FAssetRegistry& Registry = FAssetRegistry::Get();
    FAssetRegistryTestAccess::ProcessPackagePath(Registry, Files[0]);
    Registry.AssetSaved(World);

    EXPECT_EQ(Registry.GetAssetByPath(Files[0]), nullptr);
    EXPECT_EQ(Registry.GetAssetByGUID(FGuid::NewDeterministic(VFS::FileName(Files[0], true))), nullptr);
    EXPECT_TRUE(Registry.FindByPredicate([](const FAssetData& Data) { return !ExternalEntities::OwningWorldPath(Data.Path).IsEmpty(); }).IsEmpty());
    EXPECT_NE(Registry.GetAssetByPath(PackagePath("Level")), nullptr);
}

TEST_F(ExternalEntityRegistry, AnAssetReferencedOnlyFromAnEntityFileReportsTheWorld)
{
    CPhysicsMaterial* Material = NewMaterial("Material");
    NewCrate(Material);
    ASSERT_TRUE(SaveWhole());

    EXPECT_EQ(ReferencersOf(Material), TVector<FGuid>{ World->GetGUID() });
    EXPECT_TRUE(WorldDependsOn(Material)) << "the cook graph reads the world's dependencies";
}

TEST_F(ExternalEntityRegistry, SavingOneEntityFileUpdatesTheWorldEntry)
{
    CPhysicsMaterial* Edited = NewMaterial("Edited");
    CPhysicsMaterial* Added = NewMaterial("Added");
    const ECS::FEntity Crate = NewCrate(nullptr);
    ASSERT_TRUE(SaveWhole());
    ASSERT_FALSE(WorldDependsOn(Edited));

    World->GetEntityRegistry().Get<SBoxColliderComponent>(Crate).PhysicsMaterial = Edited;
    ExternalEntities::MarkChanged(World, { Crate });
    ASSERT_TRUE(ExternalEntities::SaveChanges(World));
    EXPECT_EQ(ReferencersOf(Edited), TVector<FGuid>{ World->GetGUID() }) << "an edited entity file";

    const ECS::FEntity Second = NewCrate(Added);
    ExternalEntities::MarkChanged(World, { Second });
    ASSERT_TRUE(ExternalEntities::SaveChanges(World));
    EXPECT_EQ(ReferencersOf(Added), TVector<FGuid>{ World->GetGUID() }) << "an added entity file";

    const FName CrateGuid = ExternalEntities::GuidOf(World->GetEntityRegistry(), Crate);
    World->GetEntityRegistry().Destroy(Crate);
    ExternalEntities::MarkChanged(World, { Crate }, { CrateGuid });
    ASSERT_TRUE(ExternalEntities::SaveChanges(World));
    EXPECT_TRUE(ReferencersOf(Edited).IsEmpty()) << "a deleted entity file";
    EXPECT_TRUE(WorldDependsOn(Added));
}
