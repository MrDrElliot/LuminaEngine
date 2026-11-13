#include <gtest/gtest.h>
#include <imgui.h>

#include "Core/Object/ObjectCore.h"
#include "UI/Tools/FSceneEditorTool.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    class FDuplicateTestSceneTool : public FSceneEditorTool
    {
    public:
        LUMINA_EDITOR_TOOL(FDuplicateTestSceneTool)

        explicit FDuplicateTestSceneTool(CWorld* InWorld)
            : FSceneEditorTool(nullptr, "DuplicateTest", InWorld)
        {}

        void OnInitialize() override {}

        using FSceneEditorTool::GetSelectedEntities;
    };

    int32 CountEntities(const ECS::FRegistry& Registry)
    {
        int32 Count = 0;
        Registry.ForEachEntity([&](ECS::FEntity) { ++Count; });
        return Count;
    }
}

// A child selected with its parent used to be copied twice, the second copy landing back under the original parent.
TEST(SceneEditorDuplicate, ParentAndChildSelectedTogetherCopyTheSubtreeOnce)
{
    STransformComponent::StaticStruct();
    SNameComponent::StaticStruct();
    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();

    const ECS::FEntity Parent = World->ConstructEntity("Parent");
    const ECS::FEntity Child = World->ConstructEntity("Child");
    ECS::Utils::ReparentEntity(Registry, Child, Parent, false);

    FDuplicateTestSceneTool Tool(World);
    const ECS::FEntity Selection[] = { Child, Parent };
    Tool.DuplicateAndSelect(TSpan<const ECS::FEntity>(Selection, 2));

    EXPECT_EQ(CountEntities(Registry), 4);
    EXPECT_EQ(Hierarchy.GetChildCount(Parent), 1u);

    ASSERT_EQ(Tool.GetSelectedEntities().Num(), 1);
    const ECS::FEntity ParentCopy = *Tool.GetSelectedEntities().begin();
    EXPECT_NE(ParentCopy, Parent);
    EXPECT_EQ(Hierarchy.GetParent(ParentCopy), ECS::NullEntity);
    ASSERT_EQ(Hierarchy.GetChildCount(ParentCopy), 1u);
    Hierarchy.ForEachChild(ParentCopy, [&](ECS::FEntity ChildCopy)
    {
        EXPECT_NE(ChildCopy, Child);
    });
}
