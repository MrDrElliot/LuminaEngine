#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Particles/ParticleEmitterStack.h"
#include "Particles/ParticleGraphModules.h"
#include "Particles/ParticleOps.h"
#include "UI/Tools/AssetEditors/ParticleSystemEditor/ParticleSystemEditorTool.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"
#include "UI/Tools/NodeGraph/Particle/ParticleNodeGraph.h"

namespace Lumina
{
    struct FParticleSystemEditorToolTestAccess
    {
        static void SelectEmitter(FParticleSystemEditorTool& Tool, int32 EmitterIndex) { Tool.SelectEmitter(EmitterIndex); }
        static void SyncEmitterStacks(FParticleSystemEditorTool& Tool) { Tool.SyncEmitterStacks(); }
        static int32 GetSelectedEmitter(const FParticleSystemEditorTool& Tool) { return Tool.SelectedEmitter; }
        static void SelectModule(FParticleSystemEditorTool& Tool, CParticleModule* Module) { Tool.SelectModule(Module); }
        static void SelectObject(FParticleSystemEditorTool& Tool, CObject* Object) { Tool.SelectObject(Object); }
        static void DrawModuleGraph(FParticleSystemEditorTool& Tool) { Tool.DrawModuleGraph(); }
        static CObject* GetSelectedObject(const FParticleSystemEditorTool& Tool) { return Tool.SelectedObject; }
    };
}

using namespace Lumina;

// Removing an earlier emitter used to leave SelectedEmitter at its old raw index, now naming a different emitter.
TEST(ParticleSystemEditorTool, RemovingAnEarlierEmitterKeepsSelectionOnTheSameEmitter)
{
    CParticleSystem* PS = NewObject<CParticleSystem>();
    CParticleEmitter* First  = PS->AddEmitter();
    PS->AddEmitter();
    CParticleEmitter* Third  = PS->AddEmitter();

    FParticleSystemEditorTool Tool(nullptr, PS);
    FParticleSystemEditorToolTestAccess::SyncEmitterStacks(Tool);
    FParticleSystemEditorToolTestAccess::SelectEmitter(Tool, 2);

    PS->RemoveEmitter(First);
    FParticleSystemEditorToolTestAccess::SyncEmitterStacks(Tool);

    ASSERT_EQ(PS->Emitters.Num(), 2);
    const int32 SelectedEmitter = FParticleSystemEditorToolTestAccess::GetSelectedEmitter(Tool);
    ASSERT_GE(SelectedEmitter, 0);
    ASSERT_LT(SelectedEmitter, PS->Emitters.Num());
    EXPECT_EQ(PS->Emitters[SelectedEmitter].Get(), Third);
}

// The property table must never keep showing a graph node that was deleted while selected.
TEST(ParticleSystemEditorTool, DeletingTheSelectedGraphNodeClearsSelection)
{
    FImGuiTestHarness Harness;

    CParticleSystem* PS = NewObject<CParticleSystem>(CPackage::CreatePackage("/ParticleSystemEditorToolTests/Scratch"), "Scratch");
    PS->AddEmitter();

    FParticleSystemEditorTool Tool(nullptr, PS);
    FParticleSystemEditorToolTestAccess::SyncEmitterStacks(Tool);

    CClass* GraphModuleClass = FindObject<CClass>(FName("CParticleModule_GraphUpdate"));
    ASSERT_NE(GraphModuleClass, nullptr);

    CParticleModule* GraphModule = ParticleOps::AddModule(PS, 0, GraphModuleClass);
    ASSERT_NE(GraphModule, nullptr);

    FParticleSystemEditorToolTestAccess::SelectEmitter(Tool, 0);
    FParticleSystemEditorToolTestAccess::SelectModule(Tool, GraphModule);

    FImGuiTestHarness::BeginFrame();
    FParticleSystemEditorToolTestAccess::DrawModuleGraph(Tool);
    FImGuiTestHarness::EndFrame();

    CParticleNodeGraph* Graph = static_cast<CParticleModule_Graph*>(GraphModule)->GetGraph();
    ASSERT_NE(Graph, nullptr);

    // Node classes are not exported from the editor module, so the suite finds this one through reflection.
    CClass* RandomNodeClass = FindObject<CClass>(FName("CParticleNode_Random"));
    ASSERT_NE(RandomNodeClass, nullptr);

    CEdGraphNode* Node = Graph->CreateNode(RandomNodeClass);
    ASSERT_NE(Node, nullptr);

    FParticleSystemEditorToolTestAccess::SelectObject(Tool, Node);
    ASSERT_EQ(FParticleSystemEditorToolTestAccess::GetSelectedObject(Tool), (CObject*)Node);

    FString Error;
    ASSERT_TRUE(NodeGraphOps::RemoveNode(Graph, Node, Error));

    EXPECT_EQ(FParticleSystemEditorToolTestAccess::GetSelectedObject(Tool), nullptr);
    EXPECT_EQ(Tool.GetPropertyTableObject(), PS);
}
