#include "MCPMaterialTools.h"

#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Agent/AgentToolSchema.h"
#include "Asset/AssetOps.h"
#include "MCPTextMatch.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Material/MaterialOps.h"
#include "Paths/Paths.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"

namespace Lumina::MCP
{
    namespace
    {
        struct FMaterialTarget
        {
            CMaterial*          Material = nullptr;
            CMaterialNodeGraph* Graph    = nullptr;
        };

        // Read-only tools are fine against an open editor; only a write would desync its view.
        enum class EMaterialAccess : uint8
        {
            Read,
            Write,
        };

        bool ResolveMaterial(const FString& Guid, EMaterialAccess Access, FMaterialTarget& Out, FString& OutError)
        {
            const TOptional<FGuid> Parsed = FGuid::TryParse(FStringView(Guid));
            if (!Parsed.IsSet())
            {
                OutError = "That is not a GUID. Use assets.search to find a material.";
                return false;
            }

            CObject* Loaded = StaticLoadObject(*Parsed);
            if (Loaded == nullptr)
            {
                OutError = "That GUID names no asset that could be loaded.";
                return false;
            }

            Out.Material = Cast<CMaterial>(Loaded);
            if (Out.Material == nullptr)
            {
                OutError = Lumina::Format("'{}' is a {}, not a material.",
                    Loaded->GetName(), Loaded->GetClass()->GetName());
                return false;
            }

            if (Access == EMaterialAccess::Write)
            {
                const FString OpenIn = MaterialOps::FindOpenEditorName(Out.Material);
                if (!OpenIn.empty())
                {
                    OutError = Lumina::Format(
                        "'{}' is open in {}, which would not see this change. Close it and try again.",
                        Out.Material->GetName(), OpenIn);
                    return false;
                }
            }

            Out.Graph = MaterialOps::FindOrCreateGraph(Out.Material);
            if (Out.Graph == nullptr)
            {
                OutError = "That material has no graph and one could not be created.";
                return false;
            }

            return true;
        }

        void MarkMaterialDirty(const FMaterialTarget& Target)
        {
            if (CPackage* Package = Target.Material->GetPackage())
            {
                Package->MarkDirty();
            }
        }

        void CollectPins(CEdGraphNode* Node, TVector<SMaterialPinInfo>& Out)
        {
            const auto Append = [&](const TVector<TObjectPtr<CEdNodeGraphPin>>& Pins, const char* Direction)
            {
                for (const TObjectPtr<CEdNodeGraphPin>& Pin : Pins)
                {
                    if (!Pin.IsValid())
                    {
                        continue;
                    }

                    SMaterialPinInfo Info;
                    Info.Name      = Pin->GetPinName();
                    Info.Direction = Direction;

                    if (Pin->HasConnection())
                    {
                        CEdNodeGraphPin* Other = Pin->GetConnection(0);
                        if (Other != nullptr && Other->GetOwningNode() != nullptr)
                        {
                            Info.ConnectedNode = Other->GetOwningNode()->GetNodeID();
                            Info.ConnectedPin  = Other->GetPinName();
                        }
                    }

                    Out.push_back(Move(Info));
                }
            };

            Append(Node->GetInputPins(), "Input");
            Append(Node->GetOutputPins(), "Output");
        }

        void RegisterListNodeTypes(FStringView Owner)
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
                        Info.Name        = FString(Class->GetName().ToString().c_str());
                        Info.DisplayName = FString(CDO->GetNodeDisplayName());
                        Info.Category    = FString(CDO->GetNodeCategory().c_str());
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
                                Info.ParameterSchema = FString(Schema.Schema.dump().c_str());
                            }
                        }

                        Out.Types.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} material node type(s).", Out.Types.size()));
                });
        }

        void RegisterDescribeMaterial(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeMaterialParams, SDescribeMaterialResult>(
                Owner, "material.describe",
                "Report a material's graph, meaning every node, its pins and what they connect to.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeMaterialParams& In, SDescribeMaterialResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, EMaterialAccess::Read, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name = FString(Target.Material->GetName().ToString().c_str());

                    for (const TObjectPtr<CEdGraphNode>& Node : Target.Graph->Nodes)
                    {
                        if (!Node.IsValid())
                        {
                            continue;
                        }

                        SMaterialNodeInfo Info;
                        Info.Id          = Node->GetNodeID();
                        Info.Type        = FString(Node->GetClass()->GetName().ToString().c_str());
                        Info.DisplayName = FString(Node->GetNodeDisplayName());

                        CollectPins(Node.Get(), Info.Pins);

                        nlohmann::json Values;
                        if (Agent::WriteStruct(Node->GetClass(), Node.Get(), Values).IsValid())
                        {
                            Info.Values = FString(Values.dump().c_str());
                        }

                        Out.Nodes.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' has {} node(s).",
                        Out.Name, Out.Nodes.size()));
                });
        }

        void RegisterCreateMaterial(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateMaterialParams, SCreateMaterialResult>(
                Owner, "material.create",
                "Create a material asset with an empty graph, ready for nodes.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateMaterialParams& In, SCreateMaterialResult& Out)
                {
                    if (In.Name.empty())
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

                    CMaterial* Material = CFactory::CreateNewOf<CMaterial>(FStringView(Path.c_str(), Path.size()));
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

                    Out.Path = FString(Path.c_str());
                    Out.Guid = FString(Material->GetGUID().ToString().c_str());

                    if (CEdGraphNode* Output = MaterialOps::FindOutputNode(Graph))
                    {
                        Out.OutputNode = Output->GetNodeID();
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterAddNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddMaterialNodeParams, SAddMaterialNodeResult>(
                Owner, "material.add_node",
                "Add a node to a material graph and report the pins it came with.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddMaterialNodeParams& In, SAddMaterialNodeResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, EMaterialAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CClass* NodeClass = MaterialOps::ResolveNodeType(FStringView(In.NodeType));
                    if (NodeClass == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "No material node type is named '{}'. Use material.list_node_types.", In.NodeType));
                    }

                    CEdGraphNode* Node = MaterialOps::AddNode(Target.Graph, NodeClass, In.X, In.Y);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error("The graph refused to create that node.");
                    }

                    Out.Id = Node->GetNodeID();
                    CollectPins(Node, Out.Pins);

                    MarkMaterialDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Added {} as node {}.", In.NodeType, Out.Id));
                });
        }

        void RegisterRemoveNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRemoveMaterialNodeParams, SCompileMaterialResult>(
                Owner, "material.remove_node",
                "Remove a node from a material graph, clearing every link it had.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRemoveMaterialNodeParams& In, SCompileMaterialResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, EMaterialAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = MaterialOps::FindNode(Target.Graph, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is in this graph.", In.Node));
                    }

                    if (!MaterialOps::RemoveNode(Target.Graph, Node, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;
                    MarkMaterialDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Removed node {}.", In.Node));
                });
        }

        void RegisterConnect(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SConnectMaterialParams, SCompileMaterialResult>(
                Owner, "material.connect",
                "Wire one node's output pin into another node's input pin.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SConnectMaterialParams& In, SCompileMaterialResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, EMaterialAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* From = MaterialOps::FindNode(Target.Graph, In.FromNode);
                    CEdGraphNode* To   = MaterialOps::FindNode(Target.Graph, In.ToNode);

                    if (From == nullptr || To == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is in this graph.",
                            From == nullptr ? In.FromNode : In.ToNode));
                    }

                    CEdNodeGraphPin* OutputPin =
                        MaterialOps::FindPin(From, FStringView(In.FromPin), ENodePinDirection::Output);

                    if (OutputPin == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Node {} has no output pin '{}'. It has {}.",
                            In.FromNode, In.FromPin,
                            MaterialOps::DescribePinNames(From, ENodePinDirection::Output)));
                    }

                    CEdNodeGraphPin* InputPin =
                        MaterialOps::FindPin(To, FStringView(In.ToPin), ENodePinDirection::Input);

                    if (InputPin == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Node {} has no input pin '{}'. It has {}.",
                            In.ToNode, In.ToPin,
                            MaterialOps::DescribePinNames(To, ENodePinDirection::Input)));
                    }

                    if (!MaterialOps::ConnectPins(Target.Graph, OutputPin, InputPin, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;
                    MarkMaterialDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Connected {}.{} to {}.{}.",
                        In.FromNode, In.FromPin, In.ToNode, In.ToPin));
                });
        }

        void RegisterDisconnect(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDisconnectMaterialParams, SCompileMaterialResult>(
                Owner, "material.disconnect",
                "Clear every link on one pin.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDisconnectMaterialParams& In, SCompileMaterialResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, EMaterialAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = MaterialOps::FindNode(Target.Graph, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is in this graph.", In.Node));
                    }

                    CEdNodeGraphPin* Pin =
                        MaterialOps::FindPin(Node, FStringView(In.Pin), ENodePinDirection::Input);

                    if (Pin == nullptr)
                    {
                        Pin = MaterialOps::FindPin(Node, FStringView(In.Pin), ENodePinDirection::Output);
                    }

                    if (Pin == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Node {} has no pin '{}'.",
                            In.Node, In.Pin));
                    }

                    if (!MaterialOps::DisconnectPin(Target.Graph, Pin, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;
                    MarkMaterialDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Disconnected {}.{}.", In.Node, In.Pin));
                });
        }

        void RegisterSetNodeProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetMaterialNodePropertyParams, SMaterialPropertyResult>(
                Owner, "material.set_node_property",
                "Set one field on one node, such as a constant's value or a parameter name.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetMaterialNodePropertyParams& In, SMaterialPropertyResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, EMaterialAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = MaterialOps::FindNode(Target.Graph, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is in this graph.", In.Node));
                    }

                    Agent::FResolvedProperty Property;
                    if (!Agent::ResolvePropertyPath(Node->GetClass(), Node, FStringView(In.Path), Property, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const nlohmann::json Value = nlohmann::json::parse(
                        In.Value.c_str(), In.Value.c_str() + In.Value.size(), nullptr, false);

                    if (Value.is_discarded())
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

                    nlohmann::json Before;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, Before);
                    Out.Previous = FString(Before.dump().c_str());

                    const Agent::FMarshalResult Applied =
                        Agent::ReadProperty(Value, Property.Property, Property.ValuePtr, FStringView(In.Path));

                    if (!Applied.IsValid())
                    {
                        return Agent::FToolResult::Error(Applied.Error);
                    }

                    MaterialOps::NotifyNodeValuesChanged(Target.Graph);

                    nlohmann::json After;
                    Agent::WriteProperty(Property.Property, Property.ValuePtr, After);
                    Out.Current = FString(After.dump().c_str());

                    MarkMaterialDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Node {} {} is now {}.",
                        In.Node, In.Path, Out.Current));
                });
        }

        void RegisterCompile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCompileMaterialParams, SCompileMaterialResult>(
                Owner, "material.compile",
                "Compile a material's graph into its shaders and report what the graph got wrong.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCompileMaterialParams& In, SCompileMaterialResult& Out)
                {
                    FMaterialTarget Target;
                    FString Error;
                    if (!ResolveMaterial(In.Material, EMaterialAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const FMaterialGraphCompileResult Result =
                        CompileMaterialGraph(Target.Material, Target.Graph);

                    Out.bSucceeded = Result.bSuccess;

                    for (const EdNodeGraph::FError& Item : Result.Errors)
                    {
                        Out.Errors.push_back(Lumina::Format("{} {}", Item.Name, Item.Description));
                    }

                    for (const EdNodeGraph::FError& Item : Result.Warnings)
                    {
                        Out.Warnings.push_back(Lumina::Format("{} {}", Item.Name, Item.Description));
                    }

                    MarkMaterialDirty(Target);

                    if (!Result.bSuccess)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Compile failed with {} error(s). {}",
                            Out.Errors.size(), Out.Errors.empty() ? FString() : Out.Errors[0]));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("Compiled with {} warning(s).",
                        Out.Warnings.size()));
                });
        }
    }

    void RegisterMaterialTools(FStringView Owner)
    {
        RegisterListNodeTypes(Owner);
        RegisterDescribeMaterial(Owner);
        RegisterCreateMaterial(Owner);
        RegisterAddNode(Owner);
        RegisterRemoveNode(Owner);
        RegisterConnect(Owner);
        RegisterDisconnect(Owner);
        RegisterSetNodeProperty(Owner);
        RegisterCompile(Owner);
    }
}
