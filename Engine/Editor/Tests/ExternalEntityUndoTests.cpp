#include <gtest/gtest.h>
#include <imgui.h>

#include "Config/EngineSettings.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"
#include "UI/Tools/FSceneEditorTool.h"
#include "World/World.h"
#include "World/Entity/Components/EntityGuidComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr const char* kAlias = "/ExternalEntityUndoTests";
    constexpr const char* kDirectory = "/ExternalEntityUndoTests/World";

    class FExternalEntityUndoTestSceneTool : public FSceneEditorTool
    {
    public:
        LUMINA_EDITOR_TOOL(FExternalEntityUndoTestSceneTool)

        explicit FExternalEntityUndoTestSceneTool(CWorld* InWorld)
            : FSceneEditorTool(nullptr, "ExternalEntityUndoTest", InWorld)
        {}

        void OnInitialize() override {}
    };
}

TEST(ExternalEntityUndo, AWholeRegistryUndoKeepsAnUnreadableEntityFileThroughTheNextSave)
{
    STransformComponent::StaticStruct();
    SNameComponent::StaticStruct();
    SEntityGuidComponent::StaticStruct();
    SDefaultWorldSettings::StaticStruct();
    RegisterPendingTypes();
    if (!VFS::DoesAliasExists(kAlias))
    {
        VFS::Mount<VFS::FMemoryFileSystem>(kAlias);
    }
    const bool bSavedProjectMode = GetDefault<CProjectSettings>()->bNewWorldsUseExternalEntities;
    GetMutableDefault<CProjectSettings>()->bNewWorldsUseExternalEntities = true;

    const FString WorldPath = Format("{}/Level.lasset", kDirectory);
    CWorld* World = NewObject<CWorld>(CPackage::CreatePackage(WorldPath), FName("Level"));
    World->ConstructEntity("Kept");
    const FName CorruptGuid = World->GetEntityRegistry().Get<SEntityGuidComponent>(World->ConstructEntity("Corrupt")).Guid;
    ASSERT_TRUE(CPackage::SavePackage(World->GetPackage(), WorldPath));

    const FStringView Id(*CorruptGuid);
    const FString CorruptPath = Format("{}/__ExternalEntities__/Level/{}/{}.lasset", kDirectory, Id.Mid(0, 2), Id);
    const FStringView Garbage("this is not a package");
    ASSERT_TRUE(VFS::WriteFile(CorruptPath, Garbage));

    World->GetPackage()->Unload();
    CPackage* Package = CPackage::LoadPackage(WorldPath);
    ASSERT_NE(Package, nullptr);
    CWorld* Loaded = Cast<CWorld>(Package->LoadObjectByName("Level"));
    ASSERT_NE(Loaded, nullptr);

    // Stands in for InitializeWorld, which moves the loaded entities in and keeps the settings entity through a registry restore.
    FWorldTestAccess::AdoptLoadedEntities(*Loaded);
    FWorldTestAccess::SetWorldType(*Loaded, EWorldType::Editor);

    // Holds the world's only strong reference, so it outlives the save.
    FExternalEntityUndoTestSceneTool Tool(Loaded);
    Tool.BeginOutsideChange();
    Loaded->ConstructEntity("Added");
    Tool.EndOutsideChange("Add");
    ASSERT_TRUE(Tool.RunUndo());

    ASSERT_TRUE(CPackage::SavePackage(Package, WorldPath));
    TVector<uint8> Bytes;
    ASSERT_TRUE(VFS::ReadFile(Bytes, CorruptPath)) << "an undo must not let the next save delete a file the load could not read";
    EXPECT_EQ(FStringView((const char*)Bytes.GetData(), Bytes.Num()), Garbage);

    Package->Unload();
    VFS::RemoveAll(kDirectory);
    GetMutableDefault<CProjectSettings>()->bNewWorldsUseExternalEntities = bSavedProjectMode;
}
