#include <gtest/gtest.h>
#include <imgui.h>

#include "Core/Object/ObjectCore.h"
#include "UI/Tools/FSceneEditorTool.h"
#include "World/World.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/SceneFolderComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    class FFolderUndoTestSceneTool : public FSceneEditorTool
    {
    public:
        LUMINA_EDITOR_TOOL(FFolderUndoTestSceneTool)

        explicit FFolderUndoTestSceneTool(CWorld* InWorld)
            : FSceneEditorTool(nullptr, "FolderUndoTest", InWorld)
        {}

        void OnInitialize() override {}
        bool SupportsSceneFolders() const override { return true; }

        using FSceneEditorTool::CreateSceneFolder;
        using FSceneEditorTool::DeleteSceneFolder;
        using FSceneEditorTool::MoveEntitiesToFolder;
        using FSceneEditorTool::GetSceneFolders;
    };
}

TEST(SceneFolderUndo, DeletingAFolderWithContentsIsOneUndoStep)
{
    STransformComponent::StaticStruct();
    SNameComponent::StaticStruct();
    SSceneFolderComponent::StaticStruct();
    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    FWorldTestAccess::SetWorldType(*World, EWorldType::Editor);
    FWorldTestAccess::CreateSingletonEntity(*World);
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    FFolderUndoTestSceneTool Tool(World);
    const uint32 Folder = Tool.CreateSceneFolder("Props", SSceneFolderComponent::NoFolder);
    ASSERT_NE(Folder, SSceneFolderComponent::NoFolder);

    const ECS::FEntity First = World->ConstructEntity("First");
    const ECS::FEntity Second = World->ConstructEntity("Second");
    Tool.MoveEntitiesToFolder({ First, Second }, Folder);
    ASSERT_EQ(Tool.GetSceneFolders()->FindEntityFolder(First), Folder);

    Tool.DeleteSceneFolder(Folder, true);
    EXPECT_EQ(Tool.GetSceneFolders()->Find(Folder), nullptr);
    EXPECT_FALSE(Registry.IsValid(First));
    EXPECT_FALSE(Registry.IsValid(Second));

    ASSERT_TRUE(Tool.RunUndo());
    ASSERT_NE(Tool.GetSceneFolders()->Find(Folder), nullptr);
    EXPECT_TRUE(Registry.IsValid(First));
    EXPECT_TRUE(Registry.IsValid(Second));
    EXPECT_EQ(Tool.GetSceneFolders()->FindEntityFolder(First), Folder);
    EXPECT_EQ(Tool.GetSceneFolders()->FindEntityFolder(Second), Folder);

    // The second undo takes back the filing, and the folder and entities stay.
    ASSERT_TRUE(Tool.RunUndo());
    EXPECT_NE(Tool.GetSceneFolders()->Find(Folder), nullptr);
    EXPECT_TRUE(Registry.IsValid(First));
    EXPECT_TRUE(Registry.IsValid(Second));
    EXPECT_EQ(Tool.GetSceneFolders()->FindEntityFolder(First), SSceneFolderComponent::NoFolder);
}
