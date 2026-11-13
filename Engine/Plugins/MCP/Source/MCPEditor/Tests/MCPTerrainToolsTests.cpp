#include "MCPTerrainTools.h"
#include "MCPToolTestUtil.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPTerrainToolsTestsInternal
{
    class MCPTerrainToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPTerrainToolsTest() : FRegisteredToolsTest("MCPTerrainToolsTests", &RegisterTerrainTools) {}
    };
}

using namespace MCPTerrainToolsTestsInternal;

TEST_F(MCPTerrainToolsTest, CreateRefusesAnOversizedResolutionInsteadOfCrashing)
{
    ExpectRefused("terrain.create", R"({"Resolution":32769})", "Resolution must be a power of two plus one");
}

TEST_F(MCPTerrainToolsTest, CreateRefusesAResolutionThatIsNotAPowerOfTwoPlusOne)
{
    ExpectRefused("terrain.create", R"({"Resolution":100})", "Resolution must be a power of two plus one");
}

TEST_F(MCPTerrainToolsTest, CreateRefusesTooManyLayerWeightmaps)
{
    ExpectRefused("terrain.create",
        R"({"Resolution":513,"LayerWeightmapPaths":["a.png","b.png","c.png","d.png","e.png"]})",
        "terrain supports at most");
}

TEST_F(MCPTerrainToolsTest, CreateRefusesNonPositiveTileWorldSizeOrMaxHeight)
{
    ExpectRefused("terrain.create", R"({"Resolution":513,"TileWorldSize":0})", "must be positive");
    ExpectRefused("terrain.create", R"({"Resolution":513,"MaxHeight":0})", "must be positive");
}

TEST_F(MCPTerrainToolsTest, CreateWithValidBoundsReachesTheSceneCheckInsteadOfTheBoundCheck)
{
    ExpectRefused("terrain.create", R"({"Resolution":513,"HeightmapPath":"nope.png"})", "No world editor is open");
}

TEST_F(MCPTerrainToolsTest, ImportFoliageRefusesAFileThatIsNotLFOL)
{
    ExpectRefused("terrain.import_foliage", R"({"Path":"nope.lfol"})", "Could not read");
}
