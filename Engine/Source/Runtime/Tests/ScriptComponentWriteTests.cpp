#include <gtest/gtest.h>

#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
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

    SStaticMeshComponent Component;
    Component.CachedEntryState = 0;
    const uint32 Before = FMeshResolveCache::GetPendingGeneration();

    Write(&Component);

    EXPECT_FALSE(Component.bCastShadow);
    EXPECT_EQ(Component.CachedEntryState, MESH_RESOLVE_STATE_STALE);
    EXPECT_GE(FMeshResolveCache::GetPendingGeneration() - Before, 2u);
}
