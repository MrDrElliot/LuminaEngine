#include <gtest/gtest.h>

#include "Containers/Span.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"

using namespace Lumina;

namespace
{
    using FWriteComponentFn = void(*)(void*);

    class FScriptComponentWriteTest : public ::testing::Test
    {
    protected:

        static void SetUpTestSuite()
        {
            Paths::InitializePaths();
            DotNet::Initialize();
        }

        static void TearDownTestSuite()
        {
            if (DotNet::IsInitialized())
            {
                DotNet::Shutdown();
            }
        }
    };
}

// The retained scene only re-reads a mesh component that reports a change, as an editor edit does.
TEST_F(FScriptComponentWriteTest, AScriptWriteInvalidatesTheMeshResolve)
{
    // The harness registers reflected types on first touch, and the C# accessors resolve them by name.
    ASSERT_NE(SMeshComponent::StaticStruct(), nullptr);
    ASSERT_NE(SStaticMeshComponent::StaticStruct(), nullptr);

    auto* Write = (FWriteComponentFn)DotNet::ResolveManagedExport("Test_WriteStaticMeshComponent");
    ASSERT_NE(Write, nullptr);

    // Only a pooled component knows its entity, so the write is checked through the registry's resolve queue.
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = FMeshResolveQueue::Ensure(Registry);
    const ECS::FEntity Entity = Registry.Create();
    SStaticMeshComponent& Component = Registry.Emplace<SStaticMeshComponent>(Entity);

    TVector<ECS::FEntity> Dirty;
    Queue.BeginFrame();
    (void)Queue.ConsumeFullSweep(EMeshResolveSource::StaticMesh);
    Queue.TakeDirty(EMeshResolveSource::StaticMesh, Dirty);
    Component.ResolveBinding.ClearQueued();
    Component.CachedEntryState = 0;

    Write(&Component);

    EXPECT_FALSE(Component.bCastShadow);
    EXPECT_EQ(Component.CachedEntryState, MESH_RESOLVE_STATE_STALE);
    EXPECT_TRUE(Component.ResolveBinding.IsQueued());

    Queue.BeginFrame();
    Queue.TakeDirty(EMeshResolveSource::StaticMesh, Dirty);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entity);
}
