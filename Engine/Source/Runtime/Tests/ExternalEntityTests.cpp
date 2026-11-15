#include <gtest/gtest.h>

#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Config/EngineSettings.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"
#include "LogCountTestHelpers.h"
#include "World/World.h"
#include "World/ExternalEntities/ExternalEntities.h"
#include "World/Entity/Components/CameraFollowComponent.h"
#include "World/Entity/Components/EntityGuidComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr const char* kAlias = "/ExternalEntityTests";

    class ExternalEntityStorage : public testing::Test
    {
    protected:

        void SetUp() override
        {
            RegisterPendingTypes();
            if (!VFS::DoesAliasExists(kAlias))
            {
                VFS::Mount<VFS::FMemoryFileSystem>(kAlias);
            }

            // Saving and loading skip storages they cannot name, and these register on first use.
            STransformComponent::StaticStruct();
            SNameComponent::StaticStruct();
            SEntityGuidComponent::StaticStruct();
            SDefaultWorldSettings::StaticStruct();
            SCameraFollowComponent::StaticStruct();
            SBoxColliderComponent::StaticStruct();

            bSavedProjectMode = GetDefault<CProjectSettings>()->bNewWorldsUseExternalEntities;
        }

        void TearDown() override
        {
            GetMutableDefault<CProjectSettings>()->bNewWorldsUseExternalEntities = bSavedProjectMode;
            VFS::RemoveAll(Directory());
        }

        static void SetProjectMode(bool bExternal)
        {
            GetMutableDefault<CProjectSettings>()->bNewWorldsUseExternalEntities = bExternal;
        }

        static FString Directory()
        {
            return Format("{}/{}", kAlias, testing::UnitTest::GetInstance()->current_test_info()->name());
        }

        static FString WorldPath(FStringView Name = "Level")
        {
            return Format("{}/{}.lasset", Directory(), Name);
        }

        static FString EntityPath(const FName& Guid, FStringView WorldName = "Level")
        {
            const FStringView Id(*Guid);
            return Format("{}/__ExternalEntities__/{}/{}/{}.lasset", Directory(), WorldName, Id.Mid(0, 2), Id);
        }

        static CWorld* NewWorld(FStringView Name = "Level")
        {
            return NewObject<CWorld>(CPackage::CreatePackage(WorldPath(Name)), FName(Name));
        }

        static bool Save(CWorld* World)
        {
            return CPackage::SavePackage(World->GetPackage(), World->GetPackage()->GetPackagePath());
        }

        static CWorld* Reload(CWorld* World)
        {
            const FName Name = World->GetName();
            const FFixedString Path = World->GetPackage()->GetPackagePath();
            World->GetPackage()->Unload();
            CPackage* Package = CPackage::LoadPackage(Path);
            return Package != nullptr ? Cast<CWorld>(Package->LoadObjectByName(Name)) : nullptr;
        }

        static TVector<FString> SavedFiles()
        {
            TVector<FString> Files;
            VFS::RecursiveDirectoryIterator(Directory(), [&](VFS::FFileInfo& Info)
            {
                if (!Info.IsDirectory())
                {
                    Files.Add(Info.VirtualPath);
                }
            });
            return Sorted(Move(Files));
        }

        static TVector<FString> Sorted(TVector<FString> Paths)
        {
            Algo::Sort(Paths, [](const FString& A, const FString& B) { return A.View() < B.View(); });
            return Paths;
        }

        static TVector<TVector<uint8>> SavedBytes()
        {
            TVector<TVector<uint8>> Bytes;
            for (const FString& Path : SavedFiles())
            {
                EXPECT_TRUE(VFS::ReadFile(Bytes.EmplaceGetRef(), Path)) << *Path;
            }
            return Bytes;
        }

        static FName GuidOf(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            return Registry.Get<SEntityGuidComponent>(Entity).Guid;
        }

        static ECS::FEntity FindNamed(ECS::FRegistry& Registry, const FName& Name)
        {
            ECS::FEntity Found = ECS::NullEntity;
            Registry.View<SNameComponent>().ForEach([&](ECS::FEntity Entity, const SNameComponent& Component)
            {
                if (Component.Name == Name)
                {
                    Found = Entity;
                }
            });
            return Found;
        }

        bool bSavedProjectMode = false;
    };
}

TEST_F(ExternalEntityStorage, WithTheSettingOffANewWorldSavesAsOneFile)
{
    SetProjectMode(false);
    CWorld* World = NewWorld();
    World->ConstructEntity("A");
    World->ConstructEntity("B");

    ASSERT_TRUE(Save(World));
    EXPECT_EQ(SavedFiles(), TVector<FString>{ WorldPath() });
    EXPECT_TRUE(World->GetEntityRegistry().View<SDefaultWorldSettings>().IsEmpty()) << "the setting off must leave the save as it was";

    CWorld* Loaded = Reload(World);
    ASSERT_NE(Loaded, nullptr);
    ECS::FRegistry& Pending = FWorldTestAccess::PendingRegistry(*Loaded);
    EXPECT_TRUE(Pending.IsValid(FindNamed(Pending, "A")));
    EXPECT_TRUE(Pending.IsValid(FindNamed(Pending, "B")));
    Loaded->GetPackage()->Unload();
}

TEST_F(ExternalEntityStorage, WithTheSettingOnEachEntityGetsAFileNamedByItsGuid)
{
    SetProjectMode(true);
    CWorld* World = NewWorld();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity A = World->ConstructEntity("A");
    const ECS::FEntity B = World->ConstructEntity("B");
    const ECS::FEntity C = World->ConstructEntity("C");

    ASSERT_TRUE(Save(World));
    EXPECT_EQ(SavedFiles(), Sorted({ WorldPath(), EntityPath(GuidOf(Registry, A)), EntityPath(GuidOf(Registry, B)), EntityPath(GuidOf(Registry, C)) }))
        << "one world package plus one package per entity, the settings staying in the world";

    Registry.Destroy(B);
    ASSERT_TRUE(Save(World));
    EXPECT_EQ(SavedFiles(), Sorted({ WorldPath(), EntityPath(GuidOf(Registry, A)), EntityPath(GuidOf(Registry, C)) }))
        << "a destroyed entity's file must go with it";
    World->GetPackage()->Unload();
}

TEST_F(ExternalEntityStorage, ReloadKeepsComponentsHierarchyAndReferencesAcrossFiles)
{
    SetProjectMode(true);
    const FString MaterialPath = Format("{}/Material.lasset", Directory());
    CPackage* MaterialPackage = CPackage::CreatePackage(MaterialPath);
    // Held here, since unloading the world drops the only other reference to it.
    TStrongObjectPtr<CPhysicsMaterial> Material(NewObject<CPhysicsMaterial>(MaterialPackage, "Material"));
    ASSERT_TRUE(CPackage::SavePackage(MaterialPackage, MaterialPath));

    CWorld* World = NewWorld();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Parent = World->ConstructEntity("Parent");
    const ECS::FEntity Child = World->ConstructEntity("Child");
    const ECS::FEntity Camera = World->ConstructEntity("Camera");
    World->SetParent(Child, Parent);
    Registry.Emplace<SCameraFollowComponent>(Camera).Target = Child;
    Registry.Emplace<SCameraFollowComponent>(Parent).Target = Parent;
    Registry.Emplace<SBoxColliderComponent>(Child).PhysicsMaterial = Material.Get();
    Registry.Get<SBoxColliderComponent>(Child).HalfExtent = FVector3(2.0f, 3.0f, 4.0f);
    const FName ParentGuid = GuidOf(Registry, Parent);
    const FName ChildGuid = GuidOf(Registry, Child);
    const FName CameraGuid = GuidOf(Registry, Camera);

    ASSERT_TRUE(Save(World));

    CPackage* ChildPackage = CPackage::LoadPackage(EntityPath(ChildGuid));
    ASSERT_NE(ChildPackage, nullptr);
    EXPECT_TRUE(Algo::AnyOf(ChildPackage->ImportTable, [&](const FObjectImport& Import) { return Import.ObjectGUID == Material->GetGUID(); }))
        << "the entity's asset reference must go through its own package's import table";
    ChildPackage->Unload();

    CWorld* Loaded = Reload(World);
    ASSERT_NE(Loaded, nullptr);
    ECS::FRegistry& Pending = FWorldTestAccess::PendingRegistry(*Loaded);
    const ECS::FEntity LoadedParent = FindNamed(Pending, "Parent");
    const ECS::FEntity LoadedChild = FindNamed(Pending, "Child");
    const ECS::FEntity LoadedCamera = FindNamed(Pending, "Camera");
    ASSERT_TRUE(Pending.IsValid(LoadedParent));
    ASSERT_TRUE(Pending.IsValid(LoadedChild));
    ASSERT_TRUE(Pending.IsValid(LoadedCamera));

    EXPECT_EQ(GuidOf(Pending, LoadedParent), ParentGuid);
    EXPECT_EQ(GuidOf(Pending, LoadedChild), ChildGuid);
    EXPECT_EQ(GuidOf(Pending, LoadedCamera), CameraGuid);
    EXPECT_EQ(Pending.GetHierarchy().GetParent(LoadedChild), LoadedParent);
    EXPECT_EQ(Pending.Get<SCameraFollowComponent>(LoadedCamera).Target, LoadedChild) << "a reference into another file";
    EXPECT_EQ(Pending.Get<SCameraFollowComponent>(LoadedParent).Target, LoadedParent) << "a reference to the entity's own file";
    EXPECT_EQ(Pending.Get<SBoxColliderComponent>(LoadedChild).PhysicsMaterial.Get(), Material.Get());
    EXPECT_EQ(Pending.Get<SBoxColliderComponent>(LoadedChild).HalfExtent, FVector3(2.0f, 3.0f, 4.0f));
    EXPECT_TRUE(ExternalEntities::IsEnabled(Pending));

    Loaded->GetPackage()->Unload();
    MaterialPackage->Unload();
}

TEST_F(ExternalEntityStorage, SavingAnUnchangedWorldRewritesIdenticalBytes)
{
    SetProjectMode(true);
    CWorld* World = NewWorld();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Parent = World->ConstructEntity("Parent");
    const ECS::FEntity Child = World->ConstructEntity("Child");
    World->SetParent(Child, Parent);
    Registry.Emplace<SCameraFollowComponent>(World->ConstructEntity("Camera")).Target = Child;
    Registry.Emplace<SBoxColliderComponent>(Child);

    ASSERT_TRUE(Save(World));
    const TVector<FString> Files = SavedFiles();
    const TVector<TVector<uint8>> First = SavedBytes();
    ASSERT_EQ(Files.Num(), 4);

    ASSERT_TRUE(Save(World));
    EXPECT_EQ(SavedFiles(), Files);
    EXPECT_TRUE(SavedBytes() == First) << "a second save of the same world must write the same bytes";

    CWorld* Loaded = Reload(World);
    ASSERT_NE(Loaded, nullptr);
    ASSERT_TRUE(Save(Loaded));
    EXPECT_EQ(SavedFiles(), Files);
    EXPECT_TRUE(SavedBytes() == First) << "loading and saving again must not change a byte either";
    Loaded->GetPackage()->Unload();
}

TEST_F(ExternalEntityStorage, AMissingOrCorruptEntityFileWarnsAndTheRestLoads)
{
    SetProjectMode(true);
    CWorld* World = NewWorld();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Kept = World->ConstructEntity("Kept");
    const ECS::FEntity Missing = World->ConstructEntity("Missing");
    const ECS::FEntity Corrupt = World->ConstructEntity("Corrupt");
    Registry.Emplace<SCameraFollowComponent>(Kept).Target = Missing;
    const FName MissingGuid = GuidOf(Registry, Missing);
    const FName CorruptGuid = GuidOf(Registry, Corrupt);
    ASSERT_TRUE(Save(World));

    const FString MissingPath = EntityPath(MissingGuid);
    const FString CorruptPath = EntityPath(CorruptGuid);
    ASSERT_TRUE(VFS::Remove(MissingPath));
    ASSERT_TRUE(VFS::WriteFile(CorruptPath, FStringView("this is not a package")));

    const int32 MissingWarnings = CountLoggedMessages(ELogLevel::Warn, *MissingPath, *MissingGuid);
    const int32 CorruptWarnings = CountLoggedMessages(ELogLevel::Warn, *CorruptPath, *CorruptGuid);

    CWorld* Loaded = Reload(World);
    ASSERT_NE(Loaded, nullptr);
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, *MissingPath, *MissingGuid), MissingWarnings + 1);
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, *CorruptPath, *CorruptGuid), CorruptWarnings + 1);

    ECS::FRegistry& Pending = FWorldTestAccess::PendingRegistry(*Loaded);
    const ECS::FEntity LoadedKept = FindNamed(Pending, "Kept");
    ASSERT_TRUE(Pending.IsValid(LoadedKept));
    EXPECT_FALSE(Pending.IsValid(FindNamed(Pending, "Missing")));
    EXPECT_FALSE(Pending.IsValid(FindNamed(Pending, "Corrupt")));
    EXPECT_TRUE(Pending.Get<SCameraFollowComponent>(LoadedKept).Target.IsNull()) << "a reference into a missing file must be cleared";
    EXPECT_EQ(Pending.NumEntities(), 2) << "the kept entity and the world settings";

    ASSERT_TRUE(Save(Loaded));
    TVector<uint8> CorruptBytes;
    ASSERT_TRUE(VFS::ReadFile(CorruptBytes, CorruptPath)) << "a save must never delete a file it could not read";
    EXPECT_EQ(FStringView((const char*)CorruptBytes.GetData(), CorruptBytes.Num()), FStringView("this is not a package"));
    EXPECT_FALSE(VFS::Exists(MissingPath));

    const int32 RetriedWarnings = CountLoggedMessages(ELogLevel::Warn, *CorruptPath, *CorruptGuid);
    Loaded = Reload(Loaded);
    ASSERT_NE(Loaded, nullptr);
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Warn, *CorruptPath, *CorruptGuid), RetriedWarnings + 1) << "the next load tries the file again";
    Loaded->GetPackage()->Unload();
}

TEST_F(ExternalEntityStorage, AWorldKeepsItsSavedModeWhenTheProjectSettingChanges)
{
    SetProjectMode(true);
    CWorld* External = NewWorld("External");
    External->ConstructEntity("A");
    ASSERT_TRUE(Save(External));

    SetProjectMode(false);
    CWorld* Monolithic = NewWorld("Monolithic");
    // An opened world has settings, which is where it keeps its mode.
    FWorldTestAccess::CreateSingletonEntity(*Monolithic);
    Monolithic->GetDefaultWorldSettings();
    Monolithic->ConstructEntity("B");
    ASSERT_TRUE(Save(Monolithic));
    ASSERT_EQ(SavedFiles().Num(), 3);

    SetProjectMode(true);
    CWorld* ReloadedMonolithic = Reload(Monolithic);
    ASSERT_NE(ReloadedMonolithic, nullptr);
    ASSERT_TRUE(Save(ReloadedMonolithic));

    SetProjectMode(false);
    CWorld* ReloadedExternal = Reload(External);
    ASSERT_NE(ReloadedExternal, nullptr);
    EXPECT_TRUE(ExternalEntities::IsEnabled(FWorldTestAccess::PendingRegistry(*ReloadedExternal)));
    ASSERT_TRUE(Save(ReloadedExternal));

    EXPECT_EQ(SavedFiles().Num(), 3) << "neither world may change its storage when the project setting flips";
    ReloadedMonolithic->GetPackage()->Unload();
    ReloadedExternal->GetPackage()->Unload();
}

TEST_F(ExternalEntityStorage, DuplicatingKeepsEveryEntity)
{
    SetProjectMode(true);
    CWorld* World = NewWorld();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Parent = World->ConstructEntity("Parent");
    const ECS::FEntity Child = World->ConstructEntity("Child");
    World->SetParent(Child, Parent);
    Registry.Emplace<SCameraFollowComponent>(World->ConstructEntity("Camera")).Target = Child;

    auto ExpectEveryEntity = [](CWorld* Duplicate)
    {
        ASSERT_NE(Duplicate, nullptr);
        ECS::FRegistry& Pending = FWorldTestAccess::PendingRegistry(*Duplicate);
        const ECS::FEntity DuplicateParent = FindNamed(Pending, "Parent");
        const ECS::FEntity DuplicateChild = FindNamed(Pending, "Child");
        const ECS::FEntity DuplicateCamera = FindNamed(Pending, "Camera");
        ASSERT_TRUE(Pending.IsValid(DuplicateParent));
        ASSERT_TRUE(Pending.IsValid(DuplicateChild));
        ASSERT_TRUE(Pending.IsValid(DuplicateCamera));
        EXPECT_EQ(Pending.GetHierarchy().GetParent(DuplicateChild), DuplicateParent);
        EXPECT_EQ(Pending.Get<SCameraFollowComponent>(DuplicateCamera).Target, DuplicateChild);
    };

    ASSERT_TRUE(Save(World));
    TStrongObjectPtr<CWorld> FromEdited(CWorld::DuplicateWorld(World));
    ExpectEveryEntity(FromEdited.Get());

    CWorld* Loaded = Reload(World);
    ASSERT_NE(Loaded, nullptr);
    TStrongObjectPtr<CWorld> FromLoaded(CWorld::DuplicateWorld(Loaded));
    ExpectEveryEntity(FromLoaded.Get());
    Loaded->GetPackage()->Unload();
}
