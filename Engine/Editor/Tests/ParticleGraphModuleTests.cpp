#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Particles/ParticleGraphModules.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"
#include "UI/Tools/NodeGraph/Particle/Nodes/ParticleGraphNodes.h"

using namespace Lumina;

namespace
{
    template<typename T>
    T* MakeModule(const char* PackagePath)
    {
        return NewObject<T>(CPackage::CreatePackage(PackagePath));
    }

    template<typename T>
    T* FindNode(CParticleNodeGraph* Graph, int32 Skip = 0)
    {
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
        {
            if (T* Match = Cast<T>(Node.Get()); Match != nullptr && Skip-- == 0)
            {
                return Match;
            }
        }
        return nullptr;
    }

    void Wire(CParticleNodeGraph* Graph, CEdGraphNode* From, int32 Output, CEdGraphNode* To, int32 Input)
    {
        ASSERT_TRUE(Graph->ConnectPins(From->GetOutputPins()[Output].Get(), To->GetInputPins()[Input].Get()));
    }

    FString Compile(CParticleModule* Module, FParticleCompiler& Compiler)
    {
        Module->Generate(Compiler, 0);
        return Compiler.ExpandTemplate("$PARTICLE_SPAWN_FUNC\n$PARTICLE_UPDATE_FUNC\n$PARTICLE_GRID_STAGE_FUNCS\n$PARTICLE_GRID_OUTPUT_FUNC\n");
    }

    bool Contains(const FString& Source, const char* Text)
    {
        return Source.find(Text) != FString::npos;
    }
}

// An unwired pin on Set Particle leaves that value alone, so a graph touching velocity never resets position.
TEST(ParticleGraphModule, OnlyWiredPinsAreWritten)
{
    CParticleModule_GraphUpdate* Module = MakeModule<CParticleModule_GraphUpdate>("/ParticleGraphModule/Wired");
    CParticleNodeGraph* Graph = Module->GetGraph();
    Wire(Graph, FindNode<CParticleNode_Particle>(Graph), 1, FindNode<CParticleNode_SetParticle>(Graph), 1);

    FParticleCompiler Compiler;
    const FString Source = Compile(Module, Compiler);
    ASSERT_FALSE(Compiler.HasErrors());
    EXPECT_TRUE(Contains(Source, "P.Velocity = "));
    EXPECT_FALSE(Contains(Source, "P.Position = "));
}

// Every sink reads its inputs before any sink writes, so one write cannot leak into another sink's read.
TEST(ParticleGraphModule, SinksReadBeforeAnyWrites)
{
    CParticleModule_GraphUpdate* Module = MakeModule<CParticleModule_GraphUpdate>("/ParticleGraphModule/Order");
    CParticleNodeGraph* Graph = Module->GetGraph();
    CParticleNode_Particle* Particle = FindNode<CParticleNode_Particle>(Graph);
    CParticleNode_SetParticle* First = FindNode<CParticleNode_SetParticle>(Graph);
    CParticleNode_SetParticle* Second = Cast<CParticleNode_SetParticle>(Graph->CreateNode(CParticleNode_SetParticle::StaticClass()));
    Second->SetGridPos(400.0f, 300.0f);
    Wire(Graph, Particle, 0, First, 1);
    Wire(Graph, Particle, 1, Second, 0);

    FParticleCompiler Compiler;
    const FString Source = Compile(Module, Compiler);
    ASSERT_FALSE(Compiler.HasErrors());

    const size_t FirstWrite = Math::Min(Source.find("P.Velocity = "), Source.find("P.Position = "));
    const size_t LastRead   = Source.rfind("_in5 = ");
    ASSERT_NE(FirstWrite, FString::npos);
    EXPECT_LT(LastRead, FirstWrite);
}

// A grid graph's cell nodes declare their channel, which is what gives the emitter its grid.
TEST(ParticleGraphModule, AGridGraphDeclaresItsChannel)
{
    CParticleModule_GraphGrid* Module = MakeModule<CParticleModule_GraphGrid>("/ParticleGraphModule/Grid");
    FindNode<CParticleNode_Cell>(Module->GetGraph())->Channel = FName("Heat");

    FParticleCompiler Compiler;
    const FString Source = Compile(Module, Compiler);
    ASSERT_FALSE(Compiler.HasErrors());
    EXPECT_EQ(Compiler.GetGridChannelCount(), 2u);
    EXPECT_TRUE(Contains(Source, "Cell[GRID_Density] = "));
    EXPECT_TRUE(Contains(Source, "Cell[GRID_Heat]"));
}

// A node that has nothing to work on in its stage says so instead of producing code that cannot compile.
TEST(ParticleGraphModule, NodesRefuseAStageWithoutTheirData)
{
    CParticleModule_GraphUpdate* Module = MakeModule<CParticleModule_GraphUpdate>("/ParticleGraphModule/Stage");
    CParticleNodeGraph* Graph = Module->GetGraph();
    CEdGraphNode* SetCell = Graph->CreateNode(CParticleNode_SetCell::StaticClass());
    Wire(Graph, FindNode<CParticleNode_Particle>(Graph), 3, SetCell, 0);

    FParticleCompiler Compiler;
    Compile(Module, Compiler);
    EXPECT_TRUE(Compiler.HasErrors());
}
