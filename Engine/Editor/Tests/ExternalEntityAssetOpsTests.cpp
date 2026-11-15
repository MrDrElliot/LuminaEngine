#include <gtest/gtest.h>

#include "Asset/AssetOps.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"
#include "UI/Tools/EditorAssetActions.h"
#include "World/World.h"
#include "World/ExternalEntities/ExternalEntities.h"
#include "World/Entity/Components/CameraFollowComponent.h"
#include "World/Entity/Components/EntityGuidComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr const char* kAlias = "/ExternalEntityAssetOpsTests";

    class ExternalEntityAssetOps : public testing::Test
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
            SCameraFollowComponent::StaticStruct();
        }

        void TearDown() override
        {
            VFS::Unmount(kAlias);
        }

        static FString PathOf(FStringView Relative)
        {
            return Format("{}/{}/{}", kAlias, testing::UnitTest::GetInstance()->current_test_info()->name(), Relative);
        }

        // Saves an unloaded world at Path whose camera entity follows its target, which sits in another entity file.
        static TVector<FName> SaveWorldWithEntities(FStringView Path)
        {
            CWorld* World = NewObject<CWorld>(CPackage::CreatePackage(Path), FName(VFS::FileName(Path, true)));
            FWorldTestAccess::SetWorldType(*World, EWorldType::Editor);
            FWorldTestAccess::CreateSingletonEntity(*World);
            World->GetDefaultWorldSettings().bExternalEntities = true;
            const ECS::FEntity Target = World->ConstructEntity("Target");
            World->GetEntityRegistry().Emplace<SCameraFollowComponent>(World->ConstructEntity("Camera")).Target = Target;
            World->ConstructEntity("Other");
            EXPECT_TRUE(CPackage::SavePackage(World->GetPackage(), Path));

            const TVector<FName> Guids = EntityGuids(World->GetEntityRegistry());
            World->GetPackage()->Unload();
            return Guids;
        }

        static TVector<FName> EntityGuids(ECS::FRegistry& Registry)
        {
            TVector<FName> Guids;
            Registry.View<SEntityGuidComponent>().ForEach([&](ECS::FEntity Entity, const SEntityGuidComponent& Guid)
            {
                if (!Registry.HasAny<SDefaultWorldSettings>(Entity))
                {
                    Guids.Add(Guid.Guid);
                }
            });
            Algo::Sort(Guids, [](const FName& A, const FName& B) { return FStringView(*A) < FStringView(*B); });
            return Guids;
        }

        // The ids named by the entity files in the folder of the world at WorldPath.
        static TVector<FName> EntityFileGuids(FStringView WorldPath)
        {
            TVector<FName> Guids;
            const FFixedString Directory = ExternalEntities::EntitiesDirectory(WorldPath);
            if (VFS::IsDirectory(Directory))
            {
                VFS::RecursiveDirectoryIterator(Directory, [&](VFS::FFileInfo& Info)
                {
                    if (!Info.IsDirectory())
                    {
                        Guids.Add(FName(VFS::FileName(Info.VirtualPath, true)));
                    }
                });
            }
            Algo::Sort(Guids, [](const FName& A, const FName& B) { return FStringView(*A) < FStringView(*B); });
            return Guids;
        }

        // Loads the world at Path, expects its camera to still follow its target, and returns its entity ids.
        static TVector<FName> LoadAndCheckWorld(FStringView Path)
        {
            CPackage* Package = CPackage::LoadPackage(Path);
            CWorld* World = Package != nullptr ? Cast<CWorld>(Package->LoadObjectByName(FName(VFS::FileName(Path, true)))) : nullptr;
            EXPECT_NE(World, nullptr) << *FString(Path);
            if (World == nullptr)
            {
                return {};
            }

            ECS::FRegistry& Registry = FWorldTestAccess::PendingRegistry(*World);
            ECS::FEntity Target = ECS::NullEntity;
            ECS::FEntity Camera = ECS::NullEntity;
            Registry.View<SNameComponent>().ForEach([&](ECS::FEntity Entity, const SNameComponent& Name)
            {
                if (Name.Name == FName("Target"))
                {
                    Target = Entity;
                }
                else if (Name.Name == FName("Camera"))
                {
                    Camera = Entity;
                }
            });
            EXPECT_TRUE(Registry.IsValid(Target));
            EXPECT_TRUE(Registry.IsValid(Camera) && Registry.Get<SCameraFollowComponent>(Camera).Target == Target) << "a reference across entity files must survive";

            const TVector<FName> Guids = EntityGuids(Registry);
            World->GetPackage()->Unload();
            return Guids;
        }

        void ExpectMovedWithItsEntities(FStringView OldPath, FStringView NewPath)
        {
            const TVector<FName> Guids = SaveWorldWithEntities(OldPath);
            ASSERT_EQ(Guids.Num(), 3);

            const AssetOps::FPathOpResult Result = AssetOps::MovePath(OldPath, NewPath);
            ASSERT_TRUE(Result.bSucceeded) << *Result.Error;

            EXPECT_FALSE(VFS::Exists(ExternalEntities::EntitiesDirectory(OldPath))) << "the old entity folder must not be left behind";
            EXPECT_EQ(EntityFileGuids(NewPath), Guids);
            EXPECT_EQ(LoadAndCheckWorld(NewPath), Guids);
        }
    };
}

TEST_F(ExternalEntityAssetOps, RenamingAWorldRenamesItsEntityFolder)
{
    ExpectMovedWithItsEntities(PathOf("Level.lasset"), PathOf("Renamed.lasset"));
}

TEST_F(ExternalEntityAssetOps, MovingAWorldMovesItsEntityFolder)
{
    ASSERT_TRUE(VFS::CreateDir(PathOf("Sub")));
    ExpectMovedWithItsEntities(PathOf("Level.lasset"), PathOf("Sub/Level.lasset"));
}

TEST_F(ExternalEntityAssetOps, AMoveOntoAnExistingEntityFolderLeavesTheWorldWhereItWas)
{
    const FString OldPath = PathOf("Level.lasset");
    const FString NewPath = PathOf("Renamed.lasset");
    const TVector<FName> Guids = SaveWorldWithEntities(OldPath);
    ASSERT_TRUE(VFS::CreateDir(ExternalEntities::EntitiesDirectory(NewPath)));

    EXPECT_FALSE(AssetOps::MovePath(OldPath, NewPath).bSucceeded);
    EXPECT_FALSE(VFS::Exists(NewPath));
    EXPECT_EQ(EntityFileGuids(OldPath), Guids);
    EXPECT_EQ(LoadAndCheckWorld(OldPath), Guids);
}

TEST_F(ExternalEntityAssetOps, DeletingAWorldDeletesItsEntityFolder)
{
    const FString Path = PathOf("Level.lasset");
    ASSERT_EQ(SaveWorldWithEntities(Path).Num(), 3);

    const AssetOps::FDeleteAssetResult Result = AssetOps::DeleteAsset(Path, nullptr);
    ASSERT_TRUE(Result.bDeleted) << *Result.Error;
    EXPECT_TRUE(Result.Error.IsEmpty()) << *Result.Error;
    EXPECT_FALSE(VFS::Exists(Path));
    EXPECT_FALSE(VFS::Exists(ExternalEntities::EntitiesDirectory(Path)));
}

TEST_F(ExternalEntityAssetOps, DuplicatingAWorldGivesTheCopyItsOwnEntities)
{
    const FString SourcePath = PathOf("Level.lasset");
    const FString CopyPath = PathOf("Level_Copy.lasset");
    const TVector<FName> SourceGuids = SaveWorldWithEntities(SourcePath);
    ASSERT_EQ(SourceGuids.Num(), 3);

    CPackage* Package = CPackage::LoadPackage(SourcePath);
    ASSERT_NE(Package, nullptr);
    CObject* Source = Package->LoadObjectByName("Level");
    ASSERT_NE(Source, nullptr);
    CObject* Copy = DuplicateAssetPackage(Source, CopyPath);
    ASSERT_NE(Copy, nullptr);
    Copy->GetPackage()->Unload();
    Package->Unload();

    const TVector<FName> CopyGuids = LoadAndCheckWorld(CopyPath);
    ASSERT_EQ(CopyGuids.Num(), 3);
    EXPECT_EQ(EntityFileGuids(CopyPath), CopyGuids);
    for (const FName& Guid : CopyGuids)
    {
        EXPECT_FALSE(Algo::Contains(SourceGuids, Guid)) << "the copy shares entity " << *Guid << " with its source";
    }
    EXPECT_EQ(EntityFileGuids(SourcePath), SourceGuids) << "duplicating must leave the source's entity files alone";
}
