#include <gtest/gtest.h>
#include <iostream>

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_FloatConstant.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_StaticSwitch.h"

using namespace Lumina;

namespace
{
    // Found by name, as a paste finds them, since these classes are not exported from the editor module.
    CClass* FindTestClass(const char* Name)
    {
        CClass* Class = FindObject<CClass>(FName(Name));
        EXPECT_NE(Class, nullptr) << Name;
        return Class;
    }

    // The editor makes every default object at startup, which links each class's properties for serialization.
    void LinkTestClasses()
    {
        CEdNodeGraph::StaticClass()->GetDefaultObject();
        CMaterialExpression_StaticSwitch::StaticClass()->GetDefaultObject();
        CAnimationGraphNodeGraph::StaticClass()->GetDefaultObject();
        CAnimGraphNode_FloatConstant::StaticClass()->GetDefaultObject();
        for (const char* Name : { "CMaterialNodeGraph", "CMaterialExpression_TextureSample", "CMaterialExpression_MakeSurface",
                                  "CMaterialExpression_Multiplication", "CAnimStateMachineGraph", "CAnimGraphNode_State",
                                  "CAnimGraphNode_StateEntry", "CAnimGraphNode_StateMachine", "CAnimStateTransition" })
        {
            FindTestClass(Name)->GetDefaultObject();
        }
    }

    // Material nodes only build in a material graph, which needs no material behind it for these tests.
    CEdNodeGraph* NewMaterialGraph(CPackage* Package, const char* Name)
    {
        return NewObject<CEdNodeGraph>(FindTestClass("CMaterialNodeGraph"), Package, Name);
    }

    FJsonValue Parse(FStringView Text)
    {
        FJsonValue Parsed;
        EXPECT_TRUE(FJsonValue::Parse(Text, Parsed));
        return Parsed;
    }

    // What the clipboard round trip does, text out and parsed back in.
    FJsonValue ThroughText(const FJsonValue& Exported)
    {
        FJsonValue Parsed;
        EXPECT_TRUE(FJsonValue::Parse(Exported.ToString(2).View(), Parsed));
        return Parsed;
    }
}

// A copied pair pastes into another graph with its link, its settings and its layout around the paste point.
TEST(NodeGraphText, PastedNodesKeepLinksSettingsAndLayout)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Material");
    CEdNodeGraph* Source = NewMaterialGraph(Package, "Source");
    CMaterialExpression_StaticSwitch* Switch = NewObject<CMaterialExpression_StaticSwitch>(Package, "Switch");
    CMaterialExpression_StaticSwitch* Feed   = NewObject<CMaterialExpression_StaticSwitch>(Package, "Feed");
    Source->AddNode(Switch);
    Source->AddNode(Feed);
    Switch->SetGridPos(300.0f, 100.0f);
    Feed->SetGridPos(100.0f, 140.0f);
    Switch->ParameterName = FName("UseDetail");
    Switch->bDefaultValue = false;
    ASSERT_TRUE(Source->ConnectPins(Feed->Output, Switch->True));

    const FJsonValue Exported = Source->ExportNodes({ Feed, Switch }, ImVec2(200.0f, 120.0f));
    std::cout << *Exported.ToString(2) << '\n';

    CEdNodeGraph* Target = NewMaterialGraph(Package, "Target");
    const THashMap<int64, CEdGraphNode*> Created = Target->ImportNodes(ThroughText(Exported), ImVec2(1000.0f, 1000.0f), EGraphImport::Whole);
    ASSERT_EQ(Created.Num(), 2);

    auto* PastedSwitch = static_cast<CMaterialExpression_StaticSwitch*>(Created.FindChecked(Switch->GetNodeID()));
    auto* PastedFeed   = static_cast<CMaterialExpression_StaticSwitch*>(Created.FindChecked(Feed->GetNodeID()));
    EXPECT_EQ(PastedSwitch->GetOwningGraph(), Target);
    EXPECT_EQ(PastedSwitch->ParameterName, FName("UseDetail"));
    EXPECT_FALSE(PastedSwitch->bDefaultValue);
    EXPECT_EQ(PastedSwitch->GetNodeX(), 1100.0f);
    EXPECT_EQ(PastedSwitch->GetNodeY(), 980.0f);
    EXPECT_EQ(PastedFeed->GetNodeX(), 900.0f);
    ASSERT_TRUE(PastedSwitch->True->HasConnection());
    EXPECT_EQ(PastedSwitch->True->GetConnection(0), PastedFeed->Output);
}

// Only wires with both ends copied come along, so a paste never hangs off the original's neighbor.
TEST(NodeGraphText, LinksLeavingTheCopiedSetAreDropped)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Partial");
    CEdNodeGraph* Source = NewMaterialGraph(Package, "Source");
    CMaterialExpression_StaticSwitch* Switch = NewObject<CMaterialExpression_StaticSwitch>(Package, "Switch");
    CMaterialExpression_StaticSwitch* Feed   = NewObject<CMaterialExpression_StaticSwitch>(Package, "Feed");
    Source->AddNode(Switch);
    Source->AddNode(Feed);
    ASSERT_TRUE(Source->ConnectPins(Feed->Output, Switch->True));

    const FJsonValue Exported = Source->ExportNodes({ Switch }, ImVec2(0.0f, 0.0f));
    EXPECT_EQ(Exported["Links"].Num(), 0);

    const THashMap<int64, CEdGraphNode*> Created = Source->ImportNodes(ThroughText(Exported), ImVec2(0.0f, 0.0f), EGraphImport::Whole);
    ASSERT_EQ(Created.Num(), 1);
    EXPECT_FALSE(static_cast<CMaterialExpression_StaticSwitch*>(Created.FindChecked(Switch->GetNodeID()))->True->HasConnection());
    EXPECT_EQ(Switch->True->GetConnection(0), Feed->Output);
}

// Asset references travel as GUIDs, so a pasted texture sample still samples the same texture.
TEST(NodeGraphText, AssetReferencesSurviveAsGuids)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Texture");
    CTexture* Texture = NewObject<CTexture>(Package, "Checker");
    ASSERT_TRUE(Texture->GetGUID().IsValid());
    const FString Guid = Texture->GetGUID().ToString();

    CEdNodeGraph* Graph = NewMaterialGraph(Package, "Graph");
    const THashMap<int64, CEdGraphNode*> Created = Graph->ImportNodes(Parse((FString(R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CMaterialExpression_TextureSample","Id":1,"Properties":{"Texture":")")
        + Guid + R"("}}]})").View()),
        ImVec2(0.0f, 0.0f), EGraphImport::Whole);
    ASSERT_EQ(Created.Num(), 1);

    // An unresolved reference would export as an empty string, so the same GUID coming back means it resolved.
    const FJsonValue Exported = Graph->ExportNodes({ Created.FindChecked(1) }, ImVec2(0.0f, 0.0f));
    EXPECT_TRUE(Exported["Nodes"][0]["Properties"]["Texture"].AsString() == Guid.View());
}

// A pasted state pair gets its own blend tree, not the original's, and keeps the transition's settings.
TEST(NodeGraphText, PastedStatesCarryBlendTreesAndTransitions)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/States");
    CEdNodeGraph* Machine = NewObject<CEdNodeGraph>(FindTestClass("CAnimStateMachineGraph"), Package, "Machine");

    // Hand-written text, as someone would paste from a file, builds the source machine.
    const THashMap<int64, CEdGraphNode*> Built = Machine->ImportNodes(Parse(R"({
        "Type": "LuminaGraphNodes",
        "Nodes": [
            { "Class": "CAnimGraphNode_State", "Id": 1, "X": 0, "Y": 0, "Properties": { "StateName": "Idle" },
              "SubGraph": { "Type": "LuminaGraphNodes", "Nodes": [
                  { "Class": "CAnimGraphNode_FloatConstant", "Id": 7, "X": -200, "Y": 0, "Properties": { "Value": 2.5 } } ] } },
            { "Class": "CAnimGraphNode_State", "Id": 2, "X": 300, "Y": 0, "Properties": { "StateName": "Run" } }
        ],
        "Links": [ { "From": 1, "Output": "Out", "To": 2, "Input": "In" } ],
        "Extras": { "Transitions": [ { "FromStateNodeID": 1, "ToStateNodeID": 2, "bRequireAll": false,
                                       "Conditions": [ { "ParameterName": "Speed", "CompareValue": 0.75 } ] } ] }
    })"), ImVec2(0.0f, 0.0f), EGraphImport::Whole);
    ASSERT_EQ(Built.Num(), 2);
    CEdGraphNode* Idle = Built.FindChecked(1);
    CEdGraphNode* Run  = Built.FindChecked(2);
    ASSERT_NE(Idle->GetOwnedSubGraph(), nullptr);
    ASSERT_TRUE(Idle->GetOutputPins()[0]->HasConnection());

    const FJsonValue Exported = Machine->ExportNodes({ Idle, Run }, ImVec2(150.0f, 0.0f));
    std::cout << *Exported.ToString(2) << '\n';
    ASSERT_EQ(Exported["Extras"]["Transitions"].Num(), 1);
    EXPECT_EQ(Exported["Nodes"][0]["Properties"].Find("BlendTree"), nullptr);

    CEdNodeGraph* Other = NewObject<CEdNodeGraph>(FindTestClass("CAnimStateMachineGraph"), Package, "Other");
    const THashMap<int64, CEdGraphNode*> Created = Other->ImportNodes(ThroughText(Exported), ImVec2(0.0f, 0.0f), EGraphImport::Paste);
    ASSERT_EQ(Created.Num(), 2);

    CEdGraphNode* PastedIdle = Created.FindChecked(Idle->GetNodeID());
    CEdGraphNode* PastedRun  = Created.FindChecked(Run->GetNodeID());
    CEdNodeGraph* PastedTree = PastedIdle->GetOwnedSubGraph();
    ASSERT_NE(PastedTree, nullptr);
    EXPECT_NE(PastedTree, Idle->GetOwnedSubGraph());
    ASSERT_EQ(PastedTree->Nodes.Num(), 1);
    EXPECT_EQ(static_cast<CAnimGraphNode_FloatConstant*>(PastedTree->Nodes[0].Get())->Value, 2.5f);
    EXPECT_EQ(PastedRun->GetOwnedSubGraph(), nullptr);
    ASSERT_TRUE(PastedIdle->GetOutputPins()[0]->HasConnection());
    EXPECT_EQ(PastedIdle->GetOutputPins()[0]->GetConnection(0)->GetOwningNode(), PastedRun);

    // Read back through a second copy, which carries the pasted transition keyed to the pasted nodes.
    const FJsonValue Again = Other->ExportNodes({ PastedIdle, PastedRun }, ImVec2(0.0f, 0.0f));
    EXPECT_TRUE(Again["Nodes"][0]["Properties"]["StateName"].AsString() == FStringView("Idle"));
    const FJsonValue& Transition = Again["Extras"]["Transitions"][0];
    EXPECT_EQ(Transition["FromStateNodeID"].AsInt64(), PastedIdle->GetNodeID());
    EXPECT_EQ(Transition["ToStateNodeID"].AsInt64(), PastedRun->GetNodeID());
    EXPECT_FALSE(Transition["bRequireAll"].AsBool(true));
    ASSERT_EQ(Transition["Conditions"].Num(), 1);
    EXPECT_TRUE(Transition["Conditions"][0]["ParameterName"].AsString() == FStringView("Speed"));
    EXPECT_EQ(Transition["Conditions"][0]["CompareValue"].AsFloat(), 0.75f);
}

// Clipboard text can be anything, so only node classes in our own format are ever built.
TEST(NodeGraphText, ForeignTextAndClassesAreIgnored)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Foreign");
    CEdNodeGraph* Graph = NewMaterialGraph(Package, "Graph");

    FJsonValue Sentence;
    ASSERT_TRUE(FJsonValue::Parse(R"({"Type":"Notes","Nodes":[{"Class":"CMaterialExpression_StaticSwitch"}]})", Sentence));
    EXPECT_EQ(Graph->ImportNodes(Sentence, ImVec2(0.0f, 0.0f), EGraphImport::Whole).Num(), 0);

    FJsonValue NotANode;
    ASSERT_TRUE(FJsonValue::Parse(R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CTexture","Id":1}]})", NotANode));
    EXPECT_EQ(Graph->ImportNodes(NotANode, ImVec2(0.0f, 0.0f), EGraphImport::Whole).Num(), 0);
    EXPECT_EQ(Graph->Nodes.Num(), 0);
}

// Duplicate rides the same text format and hands back each source node's copy.
TEST(NodeGraphText, CloneNodesMapsSourcesToCopies)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Clone");
    CEdNodeGraph* Machine = NewObject<CEdNodeGraph>(FindTestClass("CAnimStateMachineGraph"), Package, "Machine");
    const THashMap<int64, CEdGraphNode*> Built = Machine->ImportNodes(Parse(
        R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CAnimGraphNode_State","Id":1,"X":10,"Y":20,"Properties":{"StateName":"Walk"}}]})"),
        ImVec2(0.0f, 0.0f), EGraphImport::Whole);
    ASSERT_EQ(Built.Num(), 1);
    CEdGraphNode* Walk = Built.FindChecked(1);

    const THashMap<CEdGraphNode*, CEdGraphNode*> Clones = Machine->CloneNodes({ Walk }, ImVec2(50.0f, 0.0f));
    ASSERT_EQ(Clones.Num(), 1);
    CEdGraphNode* Copy = Clones.FindChecked(Walk);
    EXPECT_NE(Copy, Walk);
    EXPECT_NE(Copy->GetNodeID(), Walk->GetNodeID());
    EXPECT_EQ(Copy->GetNodeX(), 60.0f);
    EXPECT_EQ(Machine->Nodes.Num(), 2);
    EXPECT_TRUE(Machine->ExportNodes({ Copy }, ImVec2(0.0f, 0.0f))["Nodes"][0]["Properties"]["StateName"].AsString() == FStringView("Walk"));
}

// Floats are written as the shortest decimal that reads back exactly, so the text says 0.1 and loses nothing.
TEST(NodeGraphText, FloatsAreWrittenInShortestForm)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Floats");
    CEdNodeGraph* Tree = NewObject<CAnimationGraphNodeGraph>(Package, "Tree");
    CAnimGraphNode_FloatConstant* Constant = NewObject<CAnimGraphNode_FloatConstant>(Package, "Constant");
    Tree->AddNode(Constant);
    Constant->Value = 0.1f;
    Constant->SetGridPos(0.3f, 0.0f);

    const FJsonValue Exported = Tree->ExportNodes({ Constant }, ImVec2(0.0f, 0.0f));
    EXPECT_EQ(Exported["Nodes"][0]["Properties"]["Value"].AsDouble(), 0.1);
    EXPECT_EQ(Exported["Nodes"][0]["X"].AsDouble(), 0.3);

    const THashMap<int64, CEdGraphNode*> Created = Tree->ImportNodes(ThroughText(Exported), ImVec2(0.0f, 0.0f), EGraphImport::Whole);
    ASSERT_EQ(Created.Num(), 1);
    EXPECT_EQ(static_cast<CAnimGraphNode_FloatConstant*>(Created.FindChecked(Constant->GetNodeID()))->Value, 0.1f);
}

// Reroutes and comments paste as nodes, and a zip tie comes along with the copied wires it bundles.
TEST(NodeGraphText, RerouteCommentAndZipTieTravelTogether)
{
    LinkTestClasses();
    FindTestClass("CMaterialReroute")->GetDefaultObject();
    FindTestClass("CEdNode_Comment")->GetDefaultObject();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Canvas");
    CEdNodeGraph* Source = NewMaterialGraph(Package, "Source");
    const THashMap<int64, CEdGraphNode*> Built = Source->ImportNodes(Parse(R"({
        "Type": "LuminaGraphNodes",
        "Nodes": [
            { "Class": "CMaterialExpression_StaticSwitch", "Id": 1, "X": 0, "Y": 0 },
            { "Class": "CMaterialReroute", "Id": 2, "X": 200, "Y": 0 },
            { "Class": "CMaterialExpression_StaticSwitch", "Id": 3, "X": 400, "Y": 0 },
            { "Class": "CEdNode_Comment", "Id": 4, "X": -50, "Y": -50, "Properties": { "Title": "Detail", "Width": 600, "Height": 200 } }
        ],
        "Links": [ { "From": 1, "Output": "", "To": 2, "Input": "" }, { "From": 2, "Output": "", "To": 3, "Input": "True" } ],
        "ZipTies": [ { "X": 300, "Y": 0, "Links": [ { "From": 2, "Output": "", "To": 3, "Input": "True" } ] } ]
    })"), ImVec2(0.0f, 0.0f), EGraphImport::Whole);
    ASSERT_EQ(Built.Num(), 4);
    ASSERT_EQ(Source->ZipTies.Num(), 1);

    TVector<CEdGraphNode*> All = { Built.FindChecked(1), Built.FindChecked(2), Built.FindChecked(3), Built.FindChecked(4) };
    const FJsonValue Exported = Source->ExportNodes(All, ImVec2(100.0f, 0.0f));
    std::cout << *Exported.ToString(2) << '\n';
    ASSERT_EQ(Exported["ZipTies"].Num(), 1);
    EXPECT_EQ(Exported["ZipTies"][0]["X"].AsDouble(), 200.0);

    CEdNodeGraph* Target = NewMaterialGraph(Package, "Target");
    const THashMap<int64, CEdGraphNode*> Created = Target->ImportNodes(ThroughText(Exported), ImVec2(1000.0f, 0.0f), EGraphImport::Whole);
    ASSERT_EQ(Created.Num(), 4);

    CEdGraphNode* Reroute = Created.FindChecked(Built.FindChecked(2)->GetNodeID());
    CEdGraphNode* Switch  = Created.FindChecked(Built.FindChecked(3)->GetNodeID());
    ASSERT_TRUE(Reroute->GetInputPins()[0]->HasConnection());
    EXPECT_EQ(Reroute->GetInputPins()[0]->GetConnection(0)->GetOwningNode(), Created.FindChecked(Built.FindChecked(1)->GetNodeID()));
    ASSERT_EQ(Target->ZipTies.Num(), 1);
    EXPECT_EQ(Target->ZipTies[0].X, 1200.0f);
    ASSERT_EQ(Target->ZipTies[0].Links.Num(), 2);
    EXPECT_EQ(Target->ZipTies[0].Links[0], Reroute->GetOutputPins()[0]->GetPinGUID());
    EXPECT_EQ(Target->ZipTies[0].Links[1], static_cast<CMaterialExpression_StaticSwitch*>(Switch)->True->GetPinGUID());

    const FJsonValue Comment = Target->ExportNodes({ Created.FindChecked(Built.FindChecked(4)->GetNodeID()) }, ImVec2(0.0f, 0.0f));
    EXPECT_TRUE(Comment["Nodes"][0]["Properties"]["Title"].AsString() == FStringView("Detail"));
    EXPECT_EQ(Comment["Nodes"][0]["Properties"]["Width"].AsDouble(), 600.0);

    // Copying the switch alone cuts the tied wire, so no tie comes along.
    EXPECT_EQ(Source->ExportNodes({ Built.FindChecked(3) }, ImVec2(0.0f, 0.0f)).Find("ZipTies"), nullptr);
}


// Each graph family only builds its own nodes, so a copy from one never lands in another.
TEST(NodeGraphText, PastesBetweenIncompatibleGraphsBuildNothing)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Incompatible");
    CEdNodeGraph* Material = NewMaterialGraph(Package, "Material");
    CEdNodeGraph* Tree     = NewObject<CAnimationGraphNodeGraph>(Package, "Tree");
    CEdNodeGraph* Machine  = NewObject<CEdNodeGraph>(FindTestClass("CAnimStateMachineGraph"), Package, "Machine");

    const FJsonValue MaterialText = Parse(R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CMaterialExpression_StaticSwitch","Id":1}]})");
    const FJsonValue BlendText    = Parse(R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CAnimGraphNode_FloatConstant","Id":1}]})");
    const FJsonValue StateText    = Parse(R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CAnimGraphNode_State","Id":1}]})");

    for (EGraphImport Mode : { EGraphImport::Paste, EGraphImport::Whole })
    {
        EXPECT_EQ(Tree->ImportNodes(MaterialText, ImVec2(0.0f, 0.0f), Mode).Num(), 0);
        EXPECT_EQ(Machine->ImportNodes(MaterialText, ImVec2(0.0f, 0.0f), Mode).Num(), 0);
        EXPECT_EQ(Material->ImportNodes(BlendText, ImVec2(0.0f, 0.0f), Mode).Num(), 0);
        EXPECT_EQ(Material->ImportNodes(StateText, ImVec2(0.0f, 0.0f), Mode).Num(), 0);
        EXPECT_EQ(Tree->ImportNodes(StateText, ImVec2(0.0f, 0.0f), Mode).Num(), 0);
        EXPECT_EQ(Machine->ImportNodes(BlendText, ImVec2(0.0f, 0.0f), Mode).Num(), 0);
    }
    EXPECT_EQ(Material->Nodes.Num(), 0);
    EXPECT_EQ(Tree->Nodes.Num(), 0);
    EXPECT_EQ(Machine->Nodes.Num(), 0);

    // The same texts do build where they belong.
    EXPECT_EQ(Material->ImportNodes(MaterialText, ImVec2(0.0f, 0.0f), EGraphImport::Paste).Num(), 1);
    EXPECT_EQ(Tree->ImportNodes(BlendText, ImVec2(0.0f, 0.0f), EGraphImport::Paste).Num(), 1);
    EXPECT_EQ(Machine->ImportNodes(StateText, ImVec2(0.0f, 0.0f), EGraphImport::Paste).Num(), 1);
}

// A canvas nested in pasted text is held to its own family, so edited text cannot smuggle material nodes into a blend tree.
TEST(NodeGraphText, NestedCanvasesOnlyBuildTheirOwnFamily)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Nested");
    CEdNodeGraph* Machine = NewObject<CEdNodeGraph>(FindTestClass("CAnimStateMachineGraph"), Package, "Machine");
    const THashMap<int64, CEdGraphNode*> Created = Machine->ImportNodes(Parse(R"({
        "Type": "LuminaGraphNodes",
        "Nodes": [ { "Class": "CAnimGraphNode_State", "Id": 1, "Properties": { "StateName": "Mixed" },
            "SubGraph": { "Type": "LuminaGraphNodes", "Nodes": [
                { "Class": "CAnimGraphNode_FloatConstant", "Id": 2 },
                { "Class": "CMaterialExpression_StaticSwitch", "Id": 3 },
                { "Class": "CAnimGraphNode_State", "Id": 4 } ] } } ]
    })"), ImVec2(0.0f, 0.0f), EGraphImport::Paste);
    ASSERT_EQ(Created.Num(), 1);
    CEdNodeGraph* BlendTree = Created.FindChecked(1)->GetOwnedSubGraph();
    ASSERT_NE(BlendTree, nullptr);
    ASSERT_EQ(BlendTree->Nodes.Num(), 1);
    EXPECT_EQ(BlendTree->Nodes[0]->GetClass(), CAnimGraphNode_FloatConstant::StaticClass());
}

// A whole machine pasted into a blend tree keeps its Entry, which no palette offers but its own canvas accepts.
TEST(NodeGraphText, PastedMachineKeepsItsEntry)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Entry");
    CEdNodeGraph* Tree = NewObject<CAnimationGraphNodeGraph>(Package, "Tree");
    const THashMap<int64, CEdGraphNode*> Created = Tree->ImportNodes(Parse(R"({
        "Type": "LuminaGraphNodes",
        "Nodes": [ { "Class": "CAnimGraphNode_StateMachine", "Id": 1, "Properties": { "MachineName": "Gait" },
            "SubGraph": { "Type": "LuminaGraphNodes", "Nodes": [
                { "Class": "CAnimGraphNode_StateEntry", "Id": 2 },
                { "Class": "CAnimGraphNode_State", "Id": 3, "Properties": { "StateName": "Walk" } } ],
              "Links": [ { "From": 2, "Output": "Entry", "To": 3, "Input": "In" } ] } } ]
    })"), ImVec2(0.0f, 0.0f), EGraphImport::Paste);
    ASSERT_EQ(Created.Num(), 1);
    CEdNodeGraph* Canvas = Created.FindChecked(1)->GetOwnedSubGraph();
    ASSERT_NE(Canvas, nullptr);
    ASSERT_EQ(Canvas->Nodes.Num(), 2);
    CEdGraphNode* Entry = Canvas->Nodes[0].Get();
    EXPECT_EQ(Entry->GetClass(), FindTestClass("CAnimGraphNode_StateEntry"));
    ASSERT_TRUE(Entry->GetOutputPins()[0]->HasConnection());

    // A pasted Entry on its own is refused, since a machine has exactly one and the canvas makes it.
    CEdNodeGraph* Machine = NewObject<CEdNodeGraph>(FindTestClass("CAnimStateMachineGraph"), Package, "Machine");
    EXPECT_EQ(Machine->ImportNodes(Parse(R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CAnimGraphNode_StateEntry","Id":2}]})"),
        ImVec2(0.0f, 0.0f), EGraphImport::Paste).Num(), 0);
}

// Links go through the graph's schema, so edited text cannot wire a surface into a scalar input.
TEST(NodeGraphText, LinksTheGraphRefusesAreDropped)
{
    LinkTestClasses();

    CPackage* Package = CPackage::CreatePackage("/GraphText/Schema");
    CEdNodeGraph* Probe = NewMaterialGraph(Package, "Probe");
    const THashMap<int64, CEdGraphNode*> Built = Probe->ImportNodes(Parse(
        R"({"Type":"LuminaGraphNodes","Nodes":[{"Class":"CMaterialExpression_Multiplication","Id":1}]})"), ImVec2(0.0f, 0.0f), EGraphImport::Paste);
    ASSERT_EQ(Built.Num(), 1);
    const FString Input = Built.FindChecked(1)->GetInputPins()[0]->GetPinName();

    CEdNodeGraph* Graph = NewMaterialGraph(Package, "Graph");
    const THashMap<int64, CEdGraphNode*> Created = Graph->ImportNodes(Parse((FString(R"({
        "Type": "LuminaGraphNodes",
        "Nodes": [ { "Class": "CMaterialExpression_MakeSurface", "Id": 1 }, { "Class": "CMaterialExpression_Multiplication", "Id": 2 },
                   { "Class": "CMaterialExpression_StaticSwitch", "Id": 3 } ],
        "Links": [ { "From": 1, "Output": "Surface", "To": 2, "Input": ")") + Input + R"(" },
                   { "From": 3, "Output": "", "To": 2, "Input": ")" + Input + R"(" } ]
    })").View()), ImVec2(0.0f, 0.0f), EGraphImport::Paste);
    ASSERT_EQ(Created.Num(), 3);

    CEdNodeGraphPin* Pin = Created.FindChecked(2)->GetInputPins()[0].Get();
    ASSERT_EQ(Pin->GetConnections().Num(), 1);
    EXPECT_EQ(Pin->GetConnection(0)->GetOwningNode(), Created.FindChecked(3));
    EXPECT_FALSE(Created.FindChecked(1)->GetOutputPins()[0]->HasConnection());
}
