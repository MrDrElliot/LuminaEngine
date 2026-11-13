#include <gtest/gtest.h>
#include <imgui.h>

#include "Core/Object/ObjectCore.h"
#include "UI/Tools/FSceneEditorTool.h"
#include "UI/Tools/TerrainEditMode.h"
#include "UI/Tools/WorldEditorMode.h"
#include "World/World.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    class FTerrainUndoTestSceneTool : public FSceneEditorTool
    {
    public:
        LUMINA_EDITOR_TOOL(FTerrainUndoTestSceneTool)

        explicit FTerrainUndoTestSceneTool(CWorld* InWorld)
            : FSceneEditorTool(nullptr, "TerrainUndoTest", InWorld)
        {}

        void OnInitialize() override {}

        using FSceneEditorTool::BeginTransaction;
        using FSceneEditorTool::BeginCreationTransaction;
        using FSceneEditorTool::EndTransaction;
    };

    // Mirrors FWorldEditorTool's own forwarding, so the test drives the mode's real transaction calls.
    class FTestEditorModeContext final : public IEditorModeContext
    {
    public:
        explicit FTestEditorModeContext(FTerrainUndoTestSceneTool& InTool) : Tool(InTool) {}

        void BeginModeTransaction() override { Tool.BeginTransaction(); }
        void EndModeTransaction(const char* Name) override { Tool.EndTransaction(FName(Name)); }
        void BeginModeCreationTransaction() override { Tool.BeginCreationTransaction(); }
        void BeginModeObjectTransaction(CObject*, const FObjectSnapshotCommand::FSerializer&) override {}
        void RunModeObjectTransaction(const char*, CObject*, const FObjectSnapshotCommand::FSerializer&, const TFunction<void()>& Edit) override
        {
            if (Edit)
            {
                Edit();
            }
        }
        ECS::FEntity GetModeSelectionFocus() const override { return ECS::NullEntity; }
        void SetModeSelection(ECS::FEntity) override {}

    private:
        FTerrainUndoTestSceneTool& Tool;
    };
}

TEST(TerrainEditModeUndo, UndoAfterCreatingDefaultTerrainRemovesTheEntity)
{
    STransformComponent::StaticStruct();
    SNameComponent::StaticStruct();
    STerrainComponent::StaticStruct();
    RegisterPendingTypes();

    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    FWorldTestAccess::SetWorldType(*World, EWorldType::Editor);
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    FTerrainUndoTestSceneTool Tool(World);
    FTestEditorModeContext ModeContext(Tool);

    FTerrainEditMode Mode;
    Mode.SetContext(&ModeContext);

    const ECS::FEntity Created = Mode.CreateTerrainTransacted(World);

    ASSERT_TRUE(Registry.IsValid(Created));
    ASSERT_TRUE(Tool.RunUndo());
    EXPECT_FALSE(Registry.IsValid(Created));

    ASSERT_TRUE(Tool.RunRedo());
    EXPECT_TRUE(Registry.IsValid(Created));
}
