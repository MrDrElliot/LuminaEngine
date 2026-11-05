#include "MaterialNode_Function.h"

#include "imgui.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialFunctionGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialInput.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "MaterialNode_StaticSwitch.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    CClass* CMaterialExpression_FunctionInput::GetSupportedGraphClass() const
    {
        return CMaterialFunctionGraph::StaticClass();
    }

    CClass* CMaterialFunctionOutput::GetSupportedGraphClass() const
    {
        return CMaterialFunctionGraph::StaticClass();
    }

    // HLSL literal for a value of the given width, e.g. Float3 -> "float3(x, y, z)".
    static FString VecLiteral(EMaterialValueType Type, const FVector4& V)
    {
        switch (Type)
        {
            case EMaterialValueType::Float:  return Format("{}", V.x);
            case EMaterialValueType::Float2: return "float2(" + Format("{}", V.x) + ", " + Format("{}", V.y) + ")";
            case EMaterialValueType::Float3: return "float3(" + Format("{}", V.x) + ", " + Format("{}", V.y) + ", " + Format("{}", V.z) + ")";
            case EMaterialValueType::Float4: return "float4(" + Format("{}", V.x) + ", " + Format("{}", V.y) + ", " + Format("{}", V.z) + ", " + Format("{}", V.w) + ")";
            // A bindless index has no meaningful default, and slot 0 is an arbitrary texture rather than null.
            case EMaterialValueType::TextureHandle: return "0u";
            default:                         return "0.0";
        }
    }

    // Shown on the node body; the details panel is where InputType and OutputType are actually changed.
    static const char* SignatureTypeName(EMaterialValueType Type)
    {
        switch (Type)
        {
            case EMaterialValueType::Float2:        return "float2";
            case EMaterialValueType::Float3:        return "float3";
            case EMaterialValueType::Float4:        return "float4";
            case EMaterialValueType::TextureHandle: return "texture handle";
            default:                                return "float";
        }
    }

    void CMaterialExpression_FunctionInput::BuildNode()
    {
        Super::BuildNode();
        if (Output)
        {
            const EMaterialInputType T = ToMaterialInputType(InputType);
            Output->SetInputType(T);
            Output->SetComponentMask(FullMaskForType(T));
        }
    }

    void CMaterialExpression_FunctionInput::DrawNodeTitleBar()
    {
        if (Output)
        {
            const EMaterialInputType T = ToMaterialInputType(InputType);
            Output->SetInputType(T);
            Output->SetComponentMask(FullMaskForType(T));
        }
        Super::DrawNodeTitleBar();
    }

    void CMaterialExpression_FunctionInput::DrawNodeBody()
    {
        ImGui::Text("%s", *InputName);
        ImGui::TextDisabled("%s", SignatureTypeName(InputType));
    }

    void CMaterialExpression_FunctionInput::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // A call node binds ResolvedVar before inlining, so reaching here means a preview compile.
        const EMaterialInputType T = ToMaterialInputType(InputType);
        if (Output)
        {
            Output->ResolvedVar.Reset();
            Output->SetInputType(T);
            Output->SetComponentMask(FullMaskForType(T));
        }

        Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(T) + " " + GetNodeFullName() + " = " + VecLiteral(InputType, DefaultValue) + ";\n");
    }

    void CMaterialFunctionOutput::BuildNode()
    {
        Input = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Value", ENodePinDirection::Input));
        Input->SetPinName("Value");
        Input->SetComponentMask(FullMaskForType(ToMaterialInputType(OutputType)));
    }

    void CMaterialFunctionOutput::DrawNodeBody()
    {
        ImGui::Text("%s", *OutputName);
        ImGui::TextDisabled("%s", SignatureTypeName(OutputType));
    }

    void CMaterialFunctionOutput::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // Emitting the connected value surfaces upstream type errors, and an unconnected output is fine.
        if (Input)
        {
            Input->SetComponentMask(FullMaskForType(ToMaterialInputType(OutputType)));
        }

        if (!Input || !Input->HasConnection())
        {
            return;
        }

        FMaterialCompiler::FInputValue Val = Compiler.GetTypedInputValue(Input, VecLiteral(OutputType, FVector4(0.0f)));
        Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(Val.Type) + " " + GetNodeFullName() + " = " + Val.Value + GetSwizzleForMask(Val.Mask) + ";\n");
    }

    void CMaterialExpression_MaterialFunctionCall::BuildNode()
    {
        RebuildPins();
    }

    bool CMaterialExpression_MaterialFunctionCall::NeedsPinRebuild() const
    {
        const CMaterialFunction* Fn = Function.Get();
        if (!bPinsBuilt || Fn != CachedFunction)
        {
            return true;
        }

        const int32 InputCount = Fn != nullptr ? Fn->GetInputs().Num() : 0;
        const int32 OutputCount = Fn != nullptr ? Fn->GetOutputs().Num() : 0;
        if (FunctionInputPins.Num() != InputCount || FunctionOutputPins.Num() != OutputCount ||
            CachedInputTypes.Num() != InputCount || CachedOutputTypes.Num() != OutputCount)
        {
            return true;
        }

        for (int32 Index = 0; Index < InputCount; ++Index)
        {
            const SMaterialFunctionInput& Input = Fn->GetInputs()[Index];
            const CMaterialInput* Pin = FunctionInputPins[Index];
            if (Pin == nullptr || Pin->GetPinName() != *Input.Name || CachedInputTypes[Index] != Input.Type)
            {
                return true;
            }
        }

        for (int32 Index = 0; Index < OutputCount; ++Index)
        {
            const SMaterialFunctionOutput& Output = Fn->GetOutputs()[Index];
            const CMaterialOutput* Pin = FunctionOutputPins[Index];
            if (Pin == nullptr || Pin->GetPinName() != *Output.Name || CachedOutputTypes[Index] != Output.Type)
            {
                return true;
            }
        }

        return false;
    }

    void CMaterialExpression_MaterialFunctionCall::RebuildPins()
    {
        // Snapshotted by direction and pin name, so a rebuild keeps wires whose pin still exists.
        struct FConnSnapshot { FString Name; bool bInput; TVector<CEdNodeGraphPin*> Remotes; };
        TVector<FConnSnapshot> Snapshots;

        auto SnapshotPins = [&](const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins, bool bInput)
        {
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
            {
                if (!Pin.IsValid() || !Pin->HasConnection())
                {
                    continue;
                }
                FConnSnapshot Snap;
                Snap.Name    = Pin->GetPinName();
                Snap.bInput  = bInput;
                Snap.Remotes = Pin->GetConnections();
                Snapshots.Add(Move(Snap));
            }
        };
        SnapshotPins(GetInputPins(), true);
        SnapshotPins(GetOutputPins(), false);

        // Sever and drop all current pins.
        auto ClearDirection = [&](ENodePinDirection Direction)
        {
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : NodePins[(uint32)Direction])
            {
                TVector<CEdNodeGraphPin*> Remotes = Pin->GetConnections();
                for (CEdNodeGraphPin* Remote : Remotes)
                {
                    Remote->RemoveConnection(Pin.Get());
                }
                Pin->ClearConnections();
            }
            NodePins[(uint32)Direction].Reset();
        };
        ClearDirection(ENodePinDirection::Input);
        ClearDirection(ENodePinDirection::Output);

        FunctionInputPins.Reset();
        FunctionOutputPins.Reset();
        CachedInputTypes.Reset();
        CachedOutputTypes.Reset();

        if (CMaterialFunction* Fn = Function.Get())
        {
            for (const SMaterialFunctionInput& In : Fn->GetInputs())
            {
                const EMaterialInputType T = ToMaterialInputType(In.Type);
                CMaterialInput* Pin = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), *In.Name, ENodePinDirection::Input));
                Pin->SetPinName(*In.Name);
                Pin->SetInputType(T);
                Pin->SetComponentMask(FullMaskForType(T));
                FunctionInputPins.Add(Pin);
                CachedInputTypes.Add(In.Type);
            }

            for (const SMaterialFunctionOutput& Out : Fn->GetOutputs())
            {
                const EMaterialInputType T = ToMaterialInputType(Out.Type);
                CMaterialOutput* Pin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), *Out.Name, ENodePinDirection::Output));
                Pin->SetPinName(*Out.Name);
                Pin->SetInputType(T);
                Pin->SetComponentMask(FullMaskForType(T));
                Pin->SetShouldDrawEditor(false);
                FunctionOutputPins.Add(Pin);
                CachedOutputTypes.Add(Out.Type);
            }
        }

        // Reconnect by matching pin name.
        for (const FConnSnapshot& Snap : Snapshots)
        {
            const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins = Snap.bInput ? GetInputPins() : GetOutputPins();
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
            {
                if (Pin->GetPinName() != Snap.Name)
                {
                    continue;
                }
                for (CEdNodeGraphPin* Remote : Snap.Remotes)
                {
                    Pin->AddConnection(Remote);
                    Remote->AddConnection(Pin.Get());
                }
                break;
            }
        }

        CachedFunction = Function.Get();
        bPinsBuilt = true;

        if (CEdNodeGraph* Graph = GetOwningGraph())
        {
            Graph->ValidateGraph();
        }
    }

    void CMaterialExpression_MaterialFunctionCall::DrawNodeTitleBar()
    {
        // Runs before this node's pins are drawn, so newly built pins appear immediately.
        if (NeedsPinRebuild())
        {
            RebuildPins();
            if (CEdNodeGraph* Graph = GetOwningGraph())
            {
                if (CPackage* Package = Graph->GetPackage())
                {
                    Package->MarkDirty();
                }
            }
        }
        Super::DrawNodeTitleBar();
    }

    void CMaterialExpression_MaterialFunctionCall::DrawNodeBody()
    {
        const char* Label = Function.IsValid() ? *Function->GetName() : "(no function)";
        ImGui::Text("%s", Label);
    }

    void CMaterialExpression_MaterialFunctionCall::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        auto EmitError = [&](const FString& Name, const FString& Description)
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = Name;
            NodeError.Description  = Description;
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        };

        // On any failure, give each output pin a zero local so the host shader still compiles.
        auto EmitNeutralOutputs = [&]()
        {
            const FString Prefix = Compiler.GetCurrentInlinePrefix() + "MF" + Format("{}", GetNodeID()) + "_";
            for (int32 j = 0; j < FunctionOutputPins.Num(); ++j)
            {
                CMaterialOutput* OutPin = FunctionOutputPins[j];
                const EMaterialInputType T = OutPin->InputType;
                const FString Var = Prefix + "bad_" + Format("{}", j);
                Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(T) + " " + Var + " = " + ZeroLiteral(T) + ";\n");
                OutPin->ResolvedVar = Var;
            }
        };

        CMaterialFunction* Fn = Function.Get();
        if (Fn == nullptr)
        {
            EmitError("Material Function", "No material function is assigned to this node.");
            EmitNeutralOutputs();
            return;
        }

        if (!Compiler.BeginInlineFunction(Fn))
        {
            EmitError("Recursive Function", FString("Material function '") + *Fn->GetName() + "' calls itself (directly or indirectly).");
            EmitNeutralOutputs();
            return;
        }

        CMaterialNodeGraph* FnGraph = Cast<CMaterialNodeGraph>(Fn->GetPackage()->LoadObjectByName(FName(GMaterialFunctionGraphObjectName)));
        if (FnGraph == nullptr)
        {
            EmitError("Material Function", FString("Material function '") + *Fn->GetName() + "' has no graph to inline.");
            EmitNeutralOutputs();
            Compiler.EndInlineFunction(Fn);
            return;
        }

        // Per-call variable prefix, composed with any enclosing call's prefix so nesting never collides.
        const FString CallPrefix = Compiler.GetCurrentInlinePrefix() + "MF" + Format("{}", GetNodeID()) + "_";
        Compiler.PushInlinePrefix(CallPrefix);
        const int32 FirstBodyError   = Compiler.GetErrors().Num();
        const int32 FirstBodyWarning = Compiler.GetWarnings().Num();

        // Gather the function's I/O nodes.
        TVector<CMaterialExpression_FunctionInput*> InputNodes;
        TVector<CMaterialFunctionOutput*>           OutputNodes;
        for (const TStrongObjectPtr<CEdGraphNode>& N : FnGraph->Nodes)
        {
            if (!N.IsValid())
            {
                continue;
            }
            if (CMaterialExpression_FunctionInput* In = Cast<CMaterialExpression_FunctionInput>(N.Get()))
            {
                InputNodes.Add(In);
            }
            else if (CMaterialFunctionOutput* Out = Cast<CMaterialFunctionOutput>(N.Get()))
            {
                OutputNodes.Add(Out);
            }
        }

        Compiler.AddRaw(FString("\t// ---- inline material function: ") + *Fn->GetName() + " ----\n");

        // ResolvedVars are cleared again at the end.
        TVector<CMaterialOutput*> BoundInputOutputs;
        for (int32 i = 0; i < FunctionInputPins.Num() && i < Fn->GetInputs().Num(); ++i)
        {
            const SMaterialFunctionInput& Decl = Fn->GetInputs()[i];
            CMaterialInput* ArgPin = FunctionInputPins[i];

            FMaterialCompiler::FInputValue Arg = Compiler.GetTypedInputValue(ArgPin, VecLiteral(Decl.Type, Decl.DefaultValue));
            const FString ArgVar = CallPrefix + "in_" + Format("{}", i);
            Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(Arg.Type) + " " + ArgVar + " = " + Arg.Value + GetSwizzleForMask(Arg.Mask) + ";\n");

            for (CMaterialExpression_FunctionInput* InNode : InputNodes)
            {
                if (InNode->InputName == Decl.Name && InNode->Output != nullptr)
                {
                    InNode->Output->ResolvedVar = ArgVar;
                    InNode->Output->SetInputType(Arg.Type);
                    InNode->Output->SetComponentMask(FullMaskForType(Arg.Type));
                    BoundInputOutputs.Add(InNode->Output);
                    break;
                }
            }
        }

        // Signature drift gets its own default, so downstream references resolve to a declared variable.
        for (CMaterialExpression_FunctionInput* InNode : InputNodes)
        {
            if (InNode->Output == nullptr || !InNode->Output->ResolvedVar.IsEmpty())
            {
                continue;
            }
            const EMaterialInputType T = ToMaterialInputType(InNode->InputType);
            const FString Var = CallPrefix + "unbound_" + *InNode->InputName;
            Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(T) + " " + Var + " = " + VecLiteral(InNode->InputType, InNode->DefaultValue) + ";\n");
            InNode->Output->ResolvedVar = Var;
            InNode->Output->SetInputType(T);
            InNode->Output->SetComponentMask(FullMaskForType(T));
            BoundInputOutputs.Add(InNode->Output);
        }

        // A function body's switches answer to the calling material's permutation, before the sort below.
        for (const auto& BodyNodeRef : FnGraph->Nodes)
        {
            CEdGraphNode* BodyNode = BodyNodeRef.Get();
            if (CMaterialExpression_StaticSwitch* Switch = Cast<CMaterialExpression_StaticSwitch>(BodyNode))
            {
                const FName Param = Switch->bDynamic ? Switch->ParameterName : NAME_None;
                Switch->SetResolvedValue(Compiler.ResolveStaticSwitch(Param, Switch->bDefaultValue, Switch));
            }
        }

        // Topologically order the body and emit interior nodes with prefixed variable names.
        TVector<CEdGraphNode*> SortedNodes;
        CEdGraphNode* CyclicNode = GraphAlgorithms::TopologicalSortFromRoots(FnGraph->Nodes, SortedNodes, [](CEdGraphNode* N)
        {
            return N->IsA<CMaterialFunctionOutput>();
        });

        if (CyclicNode != nullptr)
        {
            EmitError("Material Function", FString("Material function '") + *Fn->GetName() + "' contains a cycle.");
            for (CMaterialOutput* Bound : BoundInputOutputs)
            {
                Bound->ResolvedVar.Reset();
            }
            Compiler.PopInlinePrefix();
            Compiler.EndInlineFunction(Fn);
            EmitNeutralOutputs();
            return;
        }

        struct FSavedName { CEdGraphNode* Node; FString Name; };
        TVector<FSavedName> SavedNames;
        for (CEdGraphNode* Node : SortedNodes)
        {
            if (Node->IsRerouteNode() || Node->IsA<CMaterialExpression_FunctionInput>() || Node->IsA<CMaterialFunctionOutput>())
            {
                continue;
            }
            SavedNames.Emplace(Node, Node->GetNodeFullName());
            Node->SetNodeFullName(CallPrefix + Node->GetNodeFullName());
            static_cast<CMaterialGraphNode*>(Node)->GenerateDefinition(Compiler);
        }

        // Expose each declared output as a typed local that this call node's output pin reads from.
        for (int32 j = 0; j < FunctionOutputPins.Num() && j < Fn->GetOutputs().Num(); ++j)
        {
            const SMaterialFunctionOutput& Decl = Fn->GetOutputs()[j];
            CMaterialOutput* OutPin = FunctionOutputPins[j];
            const FString OutVar = CallPrefix + "out_" + Format("{}", j);
            const EMaterialInputType DeclType = ToMaterialInputType(Decl.Type);

            CMaterialFunctionOutput* MatchNode = nullptr;
            for (CMaterialFunctionOutput* OutNode : OutputNodes)
            {
                if (OutNode->OutputName == Decl.Name)
                {
                    MatchNode = OutNode;
                    break;
                }
            }

            if (MatchNode != nullptr && MatchNode->Input != nullptr && MatchNode->Input->HasConnection())
            {
                FMaterialCompiler::FInputValue Val = Compiler.GetTypedInputValue(MatchNode->Input, VecLiteral(Decl.Type, FVector4(0.0f)));
                Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(Val.Type) + " " + OutVar + " = " + Val.Value + GetSwizzleForMask(Val.Mask) + ";\n");
                OutPin->SetInputType(Val.Type);
                OutPin->SetComponentMask(FullMaskForType(Val.Type));
            }
            else
            {
                Compiler.AddRaw(FMaterialCompiler::GetHLSLTypeName(DeclType) + " " + OutVar + " = " + ZeroLiteral(DeclType) + ";\n");
                OutPin->SetInputType(DeclType);
                OutPin->SetComponentMask(FullMaskForType(DeclType));
            }
            OutPin->ResolvedVar = OutVar;
        }

        // Restore everything mutated on the shared function graph.
        for (FSavedName& Saved : SavedNames)
        {
            Saved.Node->SetNodeFullName(Saved.Name);
        }
        for (CMaterialOutput* Bound : BoundInputOutputs)
        {
            Bound->ResolvedVar.Reset();
        }

        Compiler.RetargetDiagnosticsToCallNode(FirstBodyError, FirstBodyWarning, this, FString(*Fn->GetName()));
        Compiler.PopInlinePrefix();
        Compiler.EndInlineFunction(Fn);
    }
}
