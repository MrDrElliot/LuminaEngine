#include "MCPMaterialTools.h"

#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Agent/AgentToolSchema.h"
#include "Asset/AssetOps.h"
#include "MCPGraphTools.h"
#include "MCPServer.h"
#include "MCPTextMatch.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Material/MaterialOps.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"
#include "Paths/Paths.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Material/MaterialFunctionGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"

namespace Lumina::MCP
{
    namespace MCPMaterialToolsInternal
    {
        struct FMaterialTarget
        {
            CObject*            Asset    = nullptr;
            CMaterial*          Material = nullptr;
            CMaterialNodeGraph* Graph    = nullptr;
        };

        bool ResolveMaterial(const FString& Guid, FMaterialTarget& Out, FString& OutError)
        {
            if (!ResolveEditableAsset(FStringView(Guid), Out.Asset, OutError))
            {
                return false;
            }

            Out.Material = Cast<CMaterial>(Out.Asset);
            CMaterialFunction* Function = Cast<CMaterialFunction>(Out.Asset);
            if (Out.Material == nullptr && Function == nullptr)
            {
                OutError = Lumina::Format("'{}' is a {}, not a material or material function.", Guid, Out.Asset->GetClass()->GetName());
                return false;
            }

            Out.Graph = Out.Material != nullptr
                ? MaterialOps::FindOrCreateGraph(Out.Material)
                : MaterialOps::FindOrCreateFunctionGraph(Function);
            if (Out.Graph == nullptr)
            {
                OutError = "That material has no graph and one could not be created.";
                return false;
            }

            return true;
        }

        void RegisterMaterialListNodeTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListMaterialNodeTypesParams, SListMaterialNodeTypesResult>(
                Owner, "material.list_node_types",
                "List every node type that can be placed in a material graph.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListMaterialNodeTypesParams& In, SListMaterialNodeTypesResult& Out)
                {
                    for (CClass* Class : MaterialOps::GetPlaceableNodeTypes())
                    {
                        CEdGraphNode* CDO = Class->GetDefaultObject<CEdGraphNode>();
                        if (CDO == nullptr)
                        {
                            continue;
                        }

                        SMaterialNodeTypeInfo Info;
                        Info.Name        = FString(*Class->GetName().ToString());
                        Info.DisplayName = FString(CDO->GetNodeDisplayName());
                        Info.Category    = FString(*CDO->GetNodeCategory());
                        Info.Description = FString(CDO->GetNodeTooltip());

                        if (!ContainsText(FStringView(Info.Name), In.Contains)
                            && !ContainsText(FStringView(Info.Category), In.Contains))
                        {
                            continue;
                        }

                        if (In.bIncludeSchema)
                        {
                            const Agent::FSchemaResult Schema = Agent::GenerateSchema(Class);
                            if (Schema.IsValid())
                            {
                                Info.ParameterSchema = Schema.Schema.ToString();
                            }
                        }

                        Out.Types.Add(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} material node type(s).", Out.Types.Num()));
                });
        }

        void RegisterMaterialDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMaterialRefParams, SDescribeMaterialResult>(
                Owner, "material.describe",
                "Report a material's graph, meaning every node, its pins and what they connect to.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SMaterialRefParams& In, SDescribeMaterialResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name = FString(*Target.Asset->GetName().ToString());
                    Out.bReadyForRender = Target.Material != nullptr && Target.Material->IsReadyForRender();

                    for (const TStrongObjectPtr<CEdGraphNode>& Node : Target.Graph->Nodes)
                    {
                        if (!Node.IsValid())
                        {
                            continue;
                        }

                        SGraphNodeInfo Info;
                        Info.Id          = Node->GetNodeID();
                        Info.Type        = FString(*Node->GetClass()->GetName().ToString());
                        Info.DisplayName = FString(Node->GetNodeDisplayName());
                        Info.X           = Node->GetNodeX();
                        Info.Y           = Node->GetNodeY();

                        CollectPins(Node.Get(), Info.Pins);

                        FJsonValue Values;
                        if (Agent::WriteStruct(Node->GetClass(), Node.Get(), Values).IsValid())
                        {
                            Info.Values = Values.ToString();
                        }

                        Out.Nodes.Add(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' has {} node(s).",
                        Out.Name, Out.Nodes.Num()));
                });
        }

        void RegisterMaterialCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateMaterialParams, SCreateMaterialResult>(
                Owner, "material.create",
                "Create a material asset with an empty graph, ready for nodes.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateMaterialParams& In, SCreateMaterialResult& Out)
                {
                    if (In.Name.IsEmpty())
                    {
                        return Agent::FToolResult::Error("A material needs a name.");
                    }

                    if (!AssetOps::IsAssetLocation(FStringView(In.Folder)))
                    {
                        return Agent::FToolResult::Error(
                            "Materials belong under /Game/Content, since nothing scans for assets elsewhere.");
                    }

                    if (!VFS::IsDirectory(In.Folder))
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "{} is not a folder. Use assets.create_folder first.", In.Folder));
                    }

                    FFixedString Path = Paths::Combine(FStringView(In.Folder), FStringView(In.Name));
                    CPackage::AddPackageExt(Path);

                    if (VFS::Exists(Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} already exists.", Path));
                    }

                    CMaterial* Material = CFactory::CreateNewOf<CMaterial>(FStringView(*Path, Path.Len()));
                    if (Material == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not create a material at {}.", Path));
                    }

                    CMaterialNodeGraph* Graph = MaterialOps::FindOrCreateGraph(Material);
                    if (Graph == nullptr)
                    {
                        return Agent::FToolResult::Error("The material was created without a graph.");
                    }

                    if (!CPackage::SavePackage(Material->GetPackage(), Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetCreated(Material);

                    Out.Path = FString(*Path);
                    Out.Guid = FString(*Material->GetGUID().ToString());

                    if (CEdGraphNode* Output = MaterialOps::FindOutputNode(Graph))
                    {
                        Out.OutputNode = Output->GetNodeID();
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        Agent::FToolResult AddMaterialNode(const SMaterialNodeParams& In, const FMaterialTarget& Target, SMaterialNodeResult& Out)
        {
            CClass* NodeClass = MaterialOps::ResolveNodeType(FStringView(In.NodeType));
            if (NodeClass == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "No material node type is named '{}'. Use material.list_node_types.", In.NodeType));
            }

            CEdGraphNode* Node = NodeGraphOps::AddNode(Target.Graph, NodeClass, In.X, In.Y);
            if (Node == nullptr)
            {
                return Agent::FToolResult::Error("The graph refused to create that node.");
            }

            Out.Id = Node->GetNodeID();
            CollectPins(Node, Out.Pins);

            MarkAssetDirty(Target.Asset);

            return Agent::FToolResult::Ok(Lumina::Format("Added {} as node {}.", In.NodeType, Out.Id));
        }

        void RegisterMaterialNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMaterialNodeParams, SMaterialNodeResult>(
                Owner, "material.node",
                "Add or remove a material graph node. Add returns the node's id and pins; X grows right and Y down, "
                "so place a node left of the node it feeds. Remove clears its links. Undoable while the graph is open.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMaterialNodeParams& In, SMaterialNodeResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Action == EMaterialNodeAction::Add)
                    {
                        return AddMaterialNode(In, Target, Out);
                    }

                    if (!RemoveGraphNode(Target.Graph, In.Node, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Removed node {}.", In.Node));
                });
        }

        void RegisterMaterialLink(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMaterialLinkParams>(
                Owner, "material.link",
                "Connect wires FromNode.FromPin (an output) into ToNode.ToPin (an input). "
                "Disconnect clears every link on Node.Pin.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMaterialLinkParams& In)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Action == EMaterialLinkAction::Connect)
                    {
                        if (!ConnectGraphPins(Target.Graph, In.FromNode, In.FromPin, In.ToNode, In.ToPin, Error))
                        {
                            return Agent::FToolResult::Error(Error);
                        }

                        MarkAssetDirty(Target.Asset);

                        return Agent::FToolResult::Ok(Lumina::Format("Connected {}.{} to {}.{}.",
                            In.FromNode, In.FromPin, In.ToNode, In.ToPin));
                    }

                    if (!DisconnectGraphPin(Target.Graph, In.Node, In.Pin, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Disconnected {}.{}.", In.Node, In.Pin));
                });
        }

        void RegisterMaterialSetNodeProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetMaterialNodePropertyParams, SMaterialPropertyResult>(
                Owner, "material.set_node_property",
                "Set one field on one node, such as a constant's value or a parameter name.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetMaterialNodePropertyParams& In, SMaterialPropertyResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = NodeGraphOps::FindNode(Target.Graph, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is in this graph.", In.Node));
                    }

                    Agent::FResolvedProperty Property;
                    if (!Agent::ResolvePropertyPath(Node->GetClass(), Node, FStringView(In.Path), Property, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FJsonValue Value;
                    if (!FJsonValue::Parse(In.Value, Value))
                    {
                        return Agent::FToolResult::Error(
                            "Value is not JSON. Strings need quotes, so a quoted name rather than a bare word.");
                    }

                    if (const Agent::FMarshalResult Check =
                            Agent::ValidatePropertyValue(Value, Property.Property, FStringView(In.Path));
                        !Check.IsValid())
                    {
                        return Agent::FToolResult::Error(Check.Error);
                    }

                    FJsonValue Before;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, Before);
                    Out.Previous = Before.ToString();

                    const Agent::FMarshalResult Applied =
                        Agent::ReadProperty(Value, Property.Property, Property.ValuePtr, FStringView(In.Path));

                    if (!Applied.IsValid())
                    {
                        return Agent::FToolResult::Error(Applied.Error);
                    }

                    // The details panel runs this too, and a node whose pins follow its properties rebuilds them here.
                    Node->PostPropertyChange(Property.Property);
                    NodeGraphOps::NotifyNodeValuesChanged(Target.Graph);

                    FJsonValue After;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, After);
                    Out.Current = After.ToString();

                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Node {} {} is now {}.",
                        In.Node, In.Path, Out.Current));
                });
        }

        void RegisterMaterialCompile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMaterialRefParams, SCompileMaterialResult>(
                Owner, "material.compile",
                "Compile a material's graph into its shaders and report what the graph got wrong.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMaterialRefParams& In, SCompileMaterialResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (Target.Material == nullptr)
                    {
                        return Agent::FToolResult::Error("A material function compiles inline; compile a material that calls it.");
                    }

                    const FMaterialGraphCompileResult Result =
                        CompileMaterialGraph(Target.Material, Target.Graph);

                    Out.bSucceeded = Result.bSuccess;

                    for (const EdNodeGraph::FError& Item : Result.Errors)
                    {
                        Out.Errors.Add(Lumina::Format("{} {}", Item.Name, Item.Description));
                    }

                    for (const EdNodeGraph::FError& Item : Result.Warnings)
                    {
                        Out.Warnings.Add(Lumina::Format("{} {}", Item.Name, Item.Description));
                    }

                    MarkAssetDirty(Target.Asset);

                    if (!Result.bSuccess)
                    {
                        // A failed call carries no structured result, so every error has to ride in the text.
                        FString Listed;
                        for (const FString& Line : Out.Errors)
                        {
                            Listed += "\n- " + Line;
                        }
                        return Agent::FToolResult::Error(Lumina::Format("Compile failed with {} error(s).{}",
                            Out.Errors.Num(), Listed));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Compiled with {} warning(s).",
                        Out.Warnings.Num()));
                });
        }
    }

    using namespace MCPMaterialToolsInternal;

    void RegisterMaterialTools(FStringView Owner)
    {
        RegisterMaterialListNodeTypes(Owner);
        RegisterMaterialDescribe(Owner);
        RegisterMaterialCreate(Owner);
        RegisterMaterialNode(Owner);
        RegisterMaterialLink(Owner);
        RegisterMaterialSetNodeProperty(Owner);
        RegisterMaterialCompile(Owner);
    }
}
