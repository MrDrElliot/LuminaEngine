#include <gtest/gtest.h>
#include <imgui.h>

#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"
#include "Platform/Time/PlatformTime.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableTest.h"
#include "UI/EditorUI.h"
#include "UI/Tools/WorldEditorTool.h"
#include "World/World.h"
#include "World/ExternalEntities/ExternalEntities.h"
#include "World/Entity/Components/CameraFollowComponent.h"
#include "World/Entity/Components/EntityGuidComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr const char* kAlias = "/ExternalEntityDirtyTests";

    // The real world editor with no windows, so undo and redo run its own post-apply hook.
    class FDirtyTestWorldTool : public FWorldEditorTool
    {
    public:
        explicit FDirtyTestWorldTool(CWorld* InWorld)
            : FWorldEditorTool(nullptr, InWorld)
        {}

        void OnInitialize() override {}

        using FWorldEditorTool::BeginComponentTransaction;
        using FWorldEditorTool::BeginCreationTransaction;
        using FWorldEditorTool::BeginDestroyTransaction;
        using FWorldEditorTool::BeginRelationshipTransaction;
        using FWorldEditorTool::BeginTransformTransaction;
        using FWorldEditorTool::ConvertEntityStorage;
        using FWorldEditorTool::EndTransaction;
        using FWorldEditorTool::RenameEntityTransacted;
    };

    using FStamps = THashMap<FString, int64>;

    class ExternalEntityDirty : public testing::Test
    {
    protected:

        void SetUp() override
        {
            RegisterPendingTypes();
            VFS::Unmount(kAlias);
            FileSystem = &VFS::Mount<VFS::FMemoryFileSystem>(kAlias);

            // Saving and loading skip storages they cannot name, and these register on first use.
            STransformComponent::StaticStruct();
            SNameComponent::StaticStruct();
            SEntityGuidComponent::StaticStruct();
            SDefaultWorldSettings::StaticStruct();
            SCameraFollowComponent::StaticStruct();

        }

        void TearDown() override
        {
            Tool.Reset();
            if (World != nullptr)
            {
                World->GetPackage()->Unload();
            }
            VFS::Unmount(kAlias);
        }

        static FString Directory()
        {
            return Format("{}/{}", kAlias, testing::UnitTest::GetInstance()->current_test_info()->name());
        }

        static FString WorldPath()
        {
            return Format("{}/Level.lasset", Directory());
        }

        static FString EntityPath(const FName& Guid)
        {
            const FStringView Id(*Guid);
            return Format("{}/__ExternalEntities__/Level/{}/{}.lasset", Directory(), Id.Mid(0, 2), Id);
        }

        void OpenEditorWorld(bool bExternal)
        {
            World = NewObject<CWorld>(CPackage::CreatePackage(WorldPath()), "Level");
            FWorldTestAccess::SetWorldType(*World, EWorldType::Editor);
            FWorldTestAccess::CreateSingletonEntity(*World);
            World->GetDefaultWorldSettings().bExternalEntities = bExternal;
            Tool = MakeUnique<FDirtyTestWorldTool>(World);
        }

        // Unloads the world and loads it back the way the editor opens one.
        void Reload()
        {
            const FName Name = World->GetName();
            Tool.Reset();
            World->GetPackage()->Unload();
            World = nullptr;
            CPackage* Package = CPackage::LoadPackage(WorldPath());
            ASSERT_NE(Package, nullptr);
            World = Cast<CWorld>(Package->LoadObjectByName(Name));
            ASSERT_NE(World, nullptr);
            FWorldTestAccess::AdoptLoadedEntities(*World);
            FWorldTestAccess::SetWorldType(*World, EWorldType::Editor);
            Tool = MakeUnique<FDirtyTestWorldTool>(World);
        }

        bool IsExternal() const
        {
            return ExternalEntities::IsEnabled(World->GetEntityRegistry());
        }

        // The ids of every entity but the world's own settings entity.
        TVector<FName> EntityGuids() const
        {
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            TVector<FName> Guids;
            Registry.View<SEntityGuidComponent>().ForEach([&](ECS::FEntity Entity, const SEntityGuidComponent& Guid)
            {
                if (!Registry.HasAny<SDefaultWorldSettings>(Entity))
                {
                    Guids.Add(Guid.Guid);
                }
            });
            return Sorted(Move(Guids));
        }

        static TVector<FString> EntityFiles()
        {
            TVector<FString> Paths;
            for (const auto& [Path, Time] : Stamps())
            {
                if (Path != WorldPath())
                {
                    Paths.Add(Path);
                }
            }
            return Sorted(Move(Paths));
        }

        bool SaveWhole() const
        {
            return CPackage::SavePackage(World->GetPackage(), WorldPath());
        }

        static FStamps Stamps()
        {
            FStamps Stamps;
            VFS::RecursiveDirectoryIterator(Directory(), [&](VFS::FFileInfo& Info)
            {
                if (!Info.IsDirectory())
                {
                    Stamps[Info.VirtualPath] = Info.LastModifyTime;
                }
            });
            return Stamps;
        }

        // Saves once the clock has passed every stamp in Before, so any file the save writes gets a new one.
        bool SaveAfter(const FStamps& Before) const
        {
            int64 Latest = 0;
            for (const auto& [Path, Time] : Before)
            {
                Latest = Math::Max(Latest, Time);
            }
            while (PlatformTime::UtcNanoseconds() <= Latest)
            {
            }
            return ExternalEntities::SaveChanges(World);
        }

        // Every path written, added or removed between the two listings.
        static TVector<FString> Touched(const FStamps& Before, const FStamps& After)
        {
            TVector<FString> Paths;
            for (const auto& [Path, Time] : After)
            {
                const int64* Old = Before.Find(Path);
                if (Old == nullptr || *Old != Time)
                {
                    Paths.Add(Path);
                }
            }
            for (const auto& [Path, Time] : Before)
            {
                if (!After.Contains(Path))
                {
                    Paths.Add(Path);
                }
            }
            return Sorted(Move(Paths));
        }

        template<typename T>
        static TVector<T> Sorted(TVector<T> Values)
        {
            Algo::Sort(Values, [](const T& A, const T& B) { return FStringView(*A) < FStringView(*B); });
            return Values;
        }

        // Dirty and deleted together, since an undo can turn one into the other.
        TVector<FName> Marked() const
        {
            const FExternalEntityChanges& Changes = World->GetExternalEntityChanges();
            TVector<FName> Guids;
            for (const FName& Guid : Changes.Dirty)
            {
                Guids.Add(Guid);
            }
            for (const FName& Guid : Changes.Deleted)
            {
                Guids.Add(Guid);
            }
            return Sorted(Move(Guids));
        }

        FName GuidOf(ECS::FEntity Entity) const
        {
            return ExternalEntities::GuidOf(World->GetEntityRegistry(), Entity);
        }

        // Runs Edit as one step, then undoes and redoes it, expecting each pass to mark exactly Expected and leave the world package clean.
        void ExpectEveryPassMarks(const TFunction<void()>& Edit, const TVector<FName>& Expected)
        {
            const TFunction<void()> Passes[] = { Edit, [&] { ASSERT_TRUE(Tool->RunUndo()); }, [&] { ASSERT_TRUE(Tool->RunRedo()); } };
            for (const TFunction<void()>& Pass : Passes)
            {
                World->GetExternalEntityChanges() = {};
                Pass();
                EXPECT_EQ(Marked(), Sorted(Expected));
                EXPECT_FALSE(World->GetPackage()->IsDirty()) << "an entity edit must leave the world package clean";
            }
        }

        VFS::FMemoryFileSystem* FileSystem = nullptr;
        CWorld* World = nullptr;
        TUniquePtr<FDirtyTestWorldTool> Tool;
    };
}

TEST_F(ExternalEntityDirty, MovingOneEntityRewritesOnlyItsFile)
{
    OpenEditorWorld(true);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Moved = World->ConstructEntity("Moved");
    World->ConstructEntity("Still");
    World->ConstructEntity("AlsoStill");
    ASSERT_TRUE(SaveWhole());
    const FStamps Before = Stamps();

    Tool->BeginTransformTransaction({ Moved });
    Registry.Get<STransformComponent>(Moved).SetLocalLocation(FVector3(4.0f, 5.0f, 6.0f));
    Tool->EndTransaction("Move");
    EXPECT_FALSE(World->GetPackage()->IsDirty());
    EXPECT_EQ(Marked(), TVector<FName>{ GuidOf(Moved) });

    ASSERT_TRUE(SaveAfter(Before));
    EXPECT_EQ(Touched(Before, Stamps()), TVector<FString>{ EntityPath(GuidOf(Moved)) })
        << "the world package and every other entity file must stay as they were";
    EXPECT_TRUE(World->GetExternalEntityChanges().IsEmpty());

    Reload();
    bool bFoundMoved = false;
    World->GetEntityRegistry().View<SNameComponent, STransformComponent>().ForEach([&](ECS::FEntity, const SNameComponent& EntityName, const STransformComponent& Transform)
    {
        if (EntityName.Name == FName("Moved"))
        {
            bFoundMoved = true;
            EXPECT_EQ(Transform.GetLocalLocation(), FVector3(4.0f, 5.0f, 6.0f));
        }
    });
    EXPECT_TRUE(bFoundMoved) << "the manifest still lists the entity and its file holds the move";
}

TEST_F(ExternalEntityDirty, DeletingRemovesTheFileAndUndoingFirstKeepsIt)
{
    OpenEditorWorld(true);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Doomed = World->ConstructEntity("Doomed");
    World->ConstructEntity("Kept");
    const FName DoomedGuid = GuidOf(Doomed);
    ASSERT_TRUE(SaveWhole());

    Tool->BeginDestroyTransaction({ Doomed });
    Registry.Destroy(Doomed);
    Tool->EndTransaction("Delete");
    EXPECT_TRUE(World->GetExternalEntityChanges().Deleted.Contains(DoomedGuid));

    ASSERT_TRUE(Tool->RunUndo());
    EXPECT_TRUE(World->GetExternalEntityChanges().Deleted.IsEmpty()) << "an undone delete must not remove the file";
    EXPECT_TRUE(World->GetExternalEntityChanges().Dirty.Contains(DoomedGuid));

    const FStamps BeforeUndoneSave = Stamps();
    ASSERT_TRUE(SaveAfter(BeforeUndoneSave));
    EXPECT_EQ(Touched(BeforeUndoneSave, Stamps()), TVector<FString>{ EntityPath(DoomedGuid) }) << "the restored entity keeps its file";

    ASSERT_TRUE(Tool->RunRedo());
    EXPECT_TRUE(World->GetExternalEntityChanges().Deleted.Contains(DoomedGuid));
    const FStamps BeforeDeleteSave = Stamps();
    ASSERT_TRUE(SaveAfter(BeforeDeleteSave));
    EXPECT_FALSE(VFS::Exists(EntityPath(DoomedGuid)));
    EXPECT_EQ(Touched(BeforeDeleteSave, Stamps()), Sorted<FString>({ WorldPath(), EntityPath(DoomedGuid) }))
        << "only the manifest is rewritten and the deleted file goes";
}

TEST_F(ExternalEntityDirty, EachEditMarksOnlyItsEntitiesThroughUndoAndRedo)
{
    OpenEditorWorld(true);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Parent = World->ConstructEntity("Parent");
    const ECS::FEntity Child = World->ConstructEntity("Child");
    const ECS::FEntity Camera = World->ConstructEntity("Camera");
    World->ConstructEntity("Bystander");
    Registry.Emplace<SCameraFollowComponent>(Camera);
    ASSERT_TRUE(SaveWhole());

    // Filled by the first pass, which is when the created entity gets its id.
    TVector<FName> CreatedGuid;
    ExpectEveryPassMarks([&]
    {
        Tool->BeginCreationTransaction();
        CreatedGuid = { GuidOf(World->ConstructEntity("Created")) };
        Tool->EndTransaction("Create");
    }, CreatedGuid);

    ExpectEveryPassMarks([&]
    {
        Tool->BeginComponentTransaction({ Child }, SNameComponent::StaticStruct());
        Registry.Get<SNameComponent>(Child).Name = "Renamed";
        Tool->EndTransaction("Rename");
    }, { GuidOf(Child) });

    ExpectEveryPassMarks([&]
    {
        Tool->BeginRelationshipTransaction({ Child }, Parent);
        World->SetParent(Child, Parent);
        Tool->EndTransaction("Reparent");
    }, { GuidOf(Child), GuidOf(Parent) });

    ExpectEveryPassMarks([&]
    {
        Tool->BeginComponentTransaction({ Camera }, SCameraFollowComponent::StaticStruct());
        Registry.Get<SCameraFollowComponent>(Camera).Offset = FVector3(1.0f, 2.0f, 3.0f);
        Tool->EndTransaction("Edit Component");
    }, { GuidOf(Camera) });
}

TEST_F(ExternalEntityDirty, TagAndScriptEditsMarkOnlyTheEditedEntity)
{
    OpenEditorWorld(true);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Tagged = World->ConstructEntity("Tagged");
    const ECS::FEntity Scripted = World->ConstructEntity("Scripted");
    World->ConstructEntity("Bystander");
    EntityScripts::Attach(Registry, Scripted, CEntityScriptTest::StaticClass());
    ASSERT_TRUE(SaveWhole());

    const FName Tag("Enemy");
    ExpectEveryPassMarks([&]
    {
        Tool->RunTransacted("Add Tag", [&] { Registry.NamedStorage<STagComponent>(Tag).Emplace(Tagged).Tag = Tag; }, { Tagged });
    }, { GuidOf(Tagged) });
    EXPECT_TRUE(ECS::Utils::EntityHasTag(Tag, Registry, Tagged));

    ExpectEveryPassMarks([&]
    {
        Tool->RunTransacted("Remove Tag", [&] { Registry.NamedStorage<STagComponent>(Tag).RemoveEntity(Tagged); }, { Tagged });
    }, { GuidOf(Tagged) });
    EXPECT_FALSE(ECS::Utils::EntityHasTag(Tag, Registry, Tagged));

    // The same record entity.set_property makes for a script property.
    ExpectEveryPassMarks([&]
    {
        Tool->RunComponentTransacted("Set Property", Scripted, SEntityScriptComponent::StaticStruct(), [&]
        {
            Cast<CEntityScriptTest>(Registry.Get<SEntityScriptComponent>(Scripted).Scripts[0].Get())->NetValue = 7;
        });
    }, { GuidOf(Scripted) });
    EXPECT_EQ(Cast<CEntityScriptTest>(Registry.Get<SEntityScriptComponent>(Scripted).Scripts[0].Get())->NetValue, 7) << "redo restores the edited value";
}

TEST_F(ExternalEntityDirty, VisualizerHandleEditsMarkOnlyTheEditedEntity)
{
    OpenEditorWorld(true);
    const ECS::FEntity Edited = World->ConstructEntity("Edited");
    World->ConstructEntity("Bystander");
    ASSERT_TRUE(SaveWhole());

    Tool->MarkVisualizerSceneDirty(Edited);
    EXPECT_FALSE(World->GetPackage()->IsDirty()) << "a dragged handle must leave the world package clean";
    EXPECT_EQ(Marked(), TVector<FName>{ GuidOf(Edited) });
}

TEST_F(ExternalEntityDirty, WorldSettingsEditsStillDirtyTheWorldPackage)
{
    OpenEditorWorld(true);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Kept = World->ConstructEntity("Kept");
    ASSERT_TRUE(SaveWhole());
    const ECS::FEntity Settings = *Registry.View<SDefaultWorldSettings>().begin();

    Tool->BeginComponentTransaction({ Settings }, SDefaultWorldSettings::StaticStruct());
    Registry.Get<SDefaultWorldSettings>(Settings).WorldKillHeight = -10.0f;
    Tool->EndTransaction("Edit Settings");
    EXPECT_TRUE(World->GetPackage()->IsDirty());
    EXPECT_TRUE(World->GetExternalEntityChanges().IsEmpty());

    const FStamps Before = Stamps();
    ASSERT_TRUE(SaveAfter(Before));
    EXPECT_EQ(Touched(Before, Stamps()), TVector<FString>{ WorldPath() }) << "the settings live in the world package, and the entity file stays";
    EXPECT_TRUE(VFS::Exists(EntityPath(GuidOf(Kept))));
}

TEST_F(ExternalEntityDirty, AnInlineWorldDirtiesItsPackageAsBefore)
{
    OpenEditorWorld(false);
    const ECS::FEntity Moved = World->ConstructEntity("Moved");
    ASSERT_TRUE(SaveWhole());

    Tool->BeginTransformTransaction({ Moved });
    World->GetEntityRegistry().Get<STransformComponent>(Moved).SetLocalLocation(FVector3(1.0f, 0.0f, 0.0f));
    Tool->EndTransaction("Move");
    EXPECT_TRUE(World->GetPackage()->IsDirty());
    EXPECT_TRUE(World->GetExternalEntityChanges().IsEmpty());

    const FStamps Before = Stamps();
    ASSERT_TRUE(SaveAfter(Before));
    EXPECT_EQ(Touched(Before, Stamps()), TVector<FString>{ WorldPath() });
    EXPECT_FALSE(World->GetPackage()->IsDirty());
}

TEST_F(ExternalEntityDirty, RenamingThroughTheWorldEditorKeepsTheWorldPackageClean)
{
    OpenEditorWorld(true);
    const ECS::FEntity Renamed = World->ConstructEntity("Before");
    World->ConstructEntity("Bystander");
    ASSERT_TRUE(SaveWhole());
    const FStamps Before = Stamps();

    Tool->RenameEntityTransacted(Renamed, "After");
    EXPECT_FALSE(World->GetPackage()->IsDirty());
    EXPECT_EQ(Marked(), TVector<FName>{ GuidOf(Renamed) });
    EXPECT_TRUE(Tool->IsUnsavedDocument());
    EXPECT_EQ(Tool->GetUnsavedItemLabels(), TVector<FString>{ "After" });

    World->GetExternalEntityChanges() = {};
    ASSERT_TRUE(Tool->RunUndo());
    EXPECT_FALSE(World->GetPackage()->IsDirty()) << "the world editor's undo hook must not dirty the world package";
    EXPECT_EQ(Tool->GetUnsavedItemLabels(), TVector<FString>{ "Before" });

    ASSERT_TRUE(SaveAfter(Before));
    EXPECT_EQ(Touched(Before, Stamps()), TVector<FString>{ EntityPath(GuidOf(Renamed)) });
    EXPECT_FALSE(Tool->IsUnsavedDocument());
}

TEST_F(ExternalEntityDirty, AnEntityFileThatFailsToWriteStaysDirty)
{
    OpenEditorWorld(true);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Moved = World->ConstructEntity("Moved");
    ASSERT_TRUE(SaveWhole());

    Tool->BeginTransformTransaction({ Moved });
    Registry.Get<STransformComponent>(Moved).SetLocalLocation(FVector3(1.0f, 2.0f, 3.0f));
    Tool->EndTransaction("Move");

    FileSystem->SetReadOnly(true);
    EXPECT_FALSE(ExternalEntities::SaveChanges(World));
    EXPECT_TRUE(World->GetExternalEntityChanges().Dirty.Contains(GuidOf(Moved))) << "a save of only entity files must keep the one it could not write";

    // Through the world package too, where Write is the one writing entity files.
    World->GetPackage()->MarkDirty();
    EXPECT_FALSE(ExternalEntities::SaveChanges(World));
    EXPECT_TRUE(World->GetExternalEntityChanges().Dirty.Contains(GuidOf(Moved))) << "a world save must keep the entity it could not write";

    FileSystem->SetReadOnly(false);
    EXPECT_TRUE(ExternalEntities::SaveChanges(World));
    EXPECT_TRUE(World->GetExternalEntityChanges().IsEmpty());
}

TEST_F(ExternalEntityDirty, SavePromptListsDirtyEntitiesOfACleanWorldPackage)
{
    OpenEditorWorld(true);
    const ECS::FEntity First = World->ConstructEntity("First");
    const ECS::FEntity Second = World->ConstructEntity("Second");
    World->ConstructEntity("Bystander");
    ASSERT_TRUE(SaveWhole());

    Tool->RenameEntityTransacted(First, "FirstRenamed");
    Tool->RenameEntityTransacted(Second, "SecondRenamed");
    ASSERT_FALSE(World->GetPackage()->IsDirty());

    const TVector<FString> Lines = FEditorUI::DescribeUnsavedItems(Tool->GetUnsavedItemLabels());
    ASSERT_EQ(Lines.Num(), 3);
    EXPECT_EQ(Lines[0], "2 entities changed");
    EXPECT_TRUE(Algo::Contains(Lines, FString("FirstRenamed")));
    EXPECT_TRUE(Algo::Contains(Lines, FString("SecondRenamed")));
}

TEST(UnsavedItemsDescription, CapsTheListAndCountsTheRest)
{
    EXPECT_TRUE(FEditorUI::DescribeUnsavedItems({}).IsEmpty());
    EXPECT_EQ(FEditorUI::DescribeUnsavedItems({ "Only" }), (TVector<FString>{ "1 entity changed", "Only" }));

    TVector<FString> Labels;
    for (int32 Index = 0; Index < FEditorUI::MaxListedUnsavedItems + 3; ++Index)
    {
        Labels.Add(Format("Entity{}", Index));
    }
    const TVector<FString> Lines = FEditorUI::DescribeUnsavedItems(Labels);
    ASSERT_EQ(Lines.Num(), FEditorUI::MaxListedUnsavedItems + 2);
    EXPECT_EQ(Lines[0], Format("{} entities changed", Labels.Num()));
    EXPECT_EQ(Lines[1], "Entity0");
    EXPECT_EQ(Lines.Last(), "+3 more");
}

TEST_F(ExternalEntityDirty, ConvertingBothWaysKeepsEveryEntityThroughSaveAndReload)
{
    OpenEditorWorld(false);
    World->ConstructEntity("First");
    World->ConstructEntity("Second");
    World->ConstructEntity("Third");
    ASSERT_TRUE(SaveWhole());
    const TVector<FName> Guids = EntityGuids();
    ASSERT_EQ(Guids.Num(), 3);
    TVector<FString> Files;
    for (const FName& Guid : Guids)
    {
        Files.Add(EntityPath(Guid));
    }

    Tool->ConvertEntityStorage();
    EXPECT_TRUE(IsExternal());
    EXPECT_EQ(Marked(), Guids);
    ASSERT_TRUE(ExternalEntities::SaveChanges(World));
    EXPECT_EQ(EntityFiles(), Sorted(Files)) << "each entity gets its own file";

    Reload();
    EXPECT_TRUE(IsExternal());
    EXPECT_EQ(EntityGuids(), Guids);

    Tool->ConvertEntityStorage();
    EXPECT_FALSE(IsExternal());
    ASSERT_TRUE(ExternalEntities::SaveChanges(World));
    EXPECT_TRUE(EntityFiles().IsEmpty()) << "the entities are back in the world package, so their files go";
    EXPECT_TRUE(World->GetExternalEntityChanges().IsEmpty());

    Reload();
    EXPECT_FALSE(IsExternal());
    EXPECT_EQ(EntityGuids(), Guids);
}

TEST_F(ExternalEntityDirty, UndoingAConvertBeforeSavingRestoresTheModeAndKeepsTheFiles)
{
    OpenEditorWorld(true);
    const ECS::FEntity Moved = World->ConstructEntity("Moved");
    World->ConstructEntity("Kept");
    ASSERT_TRUE(SaveWhole());
    Tool->BeginTransformTransaction({ Moved });
    World->GetEntityRegistry().Get<STransformComponent>(Moved).SetLocalLocation(FVector3(1.0f, 2.0f, 3.0f));
    Tool->EndTransaction("Move");
    const FStamps Before = Stamps();

    Tool->ConvertEntityStorage();
    EXPECT_FALSE(IsExternal());
    EXPECT_TRUE(World->GetPackage()->IsDirty());

    ASSERT_TRUE(Tool->RunUndo());
    EXPECT_TRUE(IsExternal());
    EXPECT_EQ(Marked(), TVector<FName>{ GuidOf(Moved) }) << "the edit made before the convert is still the only one owed";
    EXPECT_FALSE(World->GetPackage()->IsDirty());

    ASSERT_TRUE(SaveAfter(Before));
    EXPECT_EQ(Touched(Before, Stamps()), TVector<FString>{ EntityPath(GuidOf(Moved)) }) << "no file the convert would have removed is touched";
}

TEST_F(ExternalEntityDirty, AnUnsavedWorldIgnoresTheConvert)
{
    CWorld* Unsaved = NewObject<CWorld>(nullptr, "Unsaved", FGuid::New(), OF_Transient);
    FWorldTestAccess::SetWorldType(*Unsaved, EWorldType::Editor);
    FWorldTestAccess::CreateSingletonEntity(*Unsaved);
    FDirtyTestWorldTool UnsavedTool(Unsaved);

    UnsavedTool.ConvertEntityStorage();
    EXPECT_FALSE(ExternalEntities::IsEnabled(Unsaved->GetEntityRegistry()));
    EXPECT_FALSE(UnsavedTool.RunUndo()) << "nothing was recorded to undo";
}
