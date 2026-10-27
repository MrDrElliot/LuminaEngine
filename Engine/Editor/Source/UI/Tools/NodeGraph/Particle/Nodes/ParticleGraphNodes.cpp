#include "ParticleGraphNodes.h"

#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectIteration.h"
#include "Particles/ParticleModule.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Formula.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

namespace Lumina
{
    static bool IsOnCell(EParticleContext Context)
    {
        return Context == EParticleContext::Grid || Context == EParticleContext::Target;
    }

    static const char* ContextName(EParticleContext Context)
    {
        switch (Context)
        {
        case EParticleContext::Spawn:  return "a spawn module";
        case EParticleContext::Update: return "an update module";
        case EParticleContext::Grid:   return "a grid stage";
        default:                       return "a render target module";
        }
    }

    // Reports a node placed in a graph module whose stage cannot run it.
    static bool Require(FParticleCompiler& Compiler, CEdGraphNode* Node, bool bAllowed)
    {
        if (!bAllowed)
        {
            Compiler.AddError({ FString(Node->GetNodeDisplayName()), FString(Node->GetNodeDisplayName()) + " cannot run in " + ContextName(Compiler.GetContext()) + ".", Node });
        }
        return bAllowed;
    }

    // Declares each output as a local of the node, bound so downstream nodes read it by name.
    static void BindOutputs(FParticleCompiler& Compiler, CEdGraphNode* Node, std::initializer_list<const char*> Exprs)
    {
        const auto& Outputs = Node->GetOutputPins();
        size_t Index = 0;
        for (const char* Expr : Exprs)
        {
            CParticleOutput* Output = Index < Outputs.size() ? Cast<CParticleOutput>(Outputs[Index].Get()) : nullptr;
            if (Output != nullptr)
            {
                Output->ResolvedVar = FString(Node->GetNodeFullName()) + "_" + Format("{}", Index).c_str();
                Compiler.EmitCurrent("const " + FParticleCompiler::TypeName(Output->GetPinType()) + " " + Output->ResolvedVar + " = " + Expr + ";");
            }
            ++Index;
        }
    }

    static FString ChannelOf(FParticleCompiler& Compiler, const FName& Channel)
    {
        return Compiler.GridChannel(Channel);
    }

    void CParticleNode_Particle::BuildNode()
    {
        AddOutput("Position", EParticlePinType::Float3);
        AddOutput("Velocity", EParticlePinType::Float3);
        AddOutput("Color", EParticlePinType::Float4);
        AddOutput("Size", EParticlePinType::Float);
        AddOutput("Age", EParticlePinType::Float);
        AddOutput("Lifetime", EParticlePinType::Float);
        AddOutput("Life Ratio", EParticlePinType::Float);
        AddOutput("Rotation", EParticlePinType::Float);
        AddOutput("Seed", EParticlePinType::Float);
    }

    void CParticleNode_Particle::GenerateDefinition(FParticleCompiler& Compiler)
    {
        if (Require(Compiler, this, !IsOnCell(Compiler.GetContext())))
        {
            BindOutputs(Compiler, this, { "P.Position", "P.Velocity", "P.Color", "P.Size", "P.Age", "P.Lifetime",
                                          "saturate(P.Age / max(P.Lifetime, 1e-4))", "P.Rotation", "P.Seed" });
        }
    }

    void CParticleNode_Emitter::BuildNode()
    {
        AddOutput("Position", EParticlePinType::Float3);
        AddOutput("Right", EParticlePinType::Float3);
        AddOutput("Up", EParticlePinType::Float3);
        AddOutput("Forward", EParticlePinType::Float3);
        AddOutput("Scale", EParticlePinType::Float);
        AddOutput("Delta Time", EParticlePinType::Float);
        AddOutput("Time", EParticlePinType::Float);
        AddOutput("System Age", EParticlePinType::Float);
    }

    void CParticleNode_Emitter::GenerateDefinition(FParticleCompiler& Compiler)
    {
        BindOutputs(Compiler, this, { "SimParams().EmitterPosition.xyz", "SimParams().EmitterRight.xyz", "SimParams().EmitterUp.xyz",
                                      "SimParams().EmitterForward.xyz", "EmitterScale()", "SimParams().Timing.x", "SimParams().Timing.y",
                                      "SimParams().Timing.z" });
    }

    void CParticleNode_Parameter::BuildNode()
    {
        AddOutput("Value", FParticleCompiler::TypeOfWidth((int32)ParticleParamComponents(Value.Type)));
    }

    void CParticleNode_Parameter::GenerateDefinition(FParticleCompiler& Compiler)
    {
        // The details panel can change the type after the pins were built, so the output follows it here.
        CParticleOutput* Output = Cast<CParticleOutput>(GetOutputPins()[0].Get());
        Output->SetPinType(FParticleCompiler::TypeOfWidth((int32)ParticleParamComponents(Value.Type)));
        const FString Read = Compiler.Param("Value", Value);
        BindOutputs(Compiler, this, { Read.c_str() });
    }

    void CParticleNode_Random::BuildNode()
    {
        AddInput("Min", EParticlePinType::Float, FVector4(0.0f));
        AddInput("Max", EParticlePinType::Float, FVector4(1.0f));
        AddOutput("Result", EParticlePinType::Float);
    }

    void CParticleNode_Random::GenerateDefinition(FParticleCompiler& Compiler)
    {
        const FString Min  = FParticleCompiler::Coerce(Compiler.GetInputValue(Cast<CParticleInput>(GetInputPins()[0].Get())), EParticlePinType::Float);
        const FString Max  = FParticleCompiler::Coerce(Compiler.GetInputValue(Cast<CParticleInput>(GetInputPins()[1].Get())), EParticlePinType::Float);
        const uint32  Salt = (uint32)GetNodeID() * 2654435761u;

        FString Unit;
        switch (Compiler.GetContext())
        {
        case EParticleContext::Spawn:  Unit = "RandUnit(Seed)"; break;
        case EParticleContext::Update: Unit = Format("StableRandom(P.Seed, {}u)", Salt).c_str(); break;
        case EParticleContext::Grid:   Unit = Format("(float)PCGHash((uint)Coord.x * 73856093u ^ (uint)Coord.y * 19349663u ^ {}u) / 4294967295.0", Salt).c_str(); break;
        default:                       Unit = Format("(float)PCGHash(asuint(UV.x) ^ PCGHash(asuint(UV.y) ^ {}u)) / 4294967295.0", Salt).c_str(); break;
        }
        const FString Expr = "lerp(" + Min + ", " + Max + ", " + Unit + ")";
        BindOutputs(Compiler, this, { Expr.c_str() });
    }

    void CParticleNode_GetAttribute::BuildNode()
    {
        AddOutput("Value", EParticlePinType::Float);
    }

    void CParticleNode_GetAttribute::GenerateDefinition(FParticleCompiler& Compiler)
    {
        const FString Name(AttributeName.ToString().c_str());
        if (!Require(Compiler, this, !IsOnCell(Compiler.GetContext())))
        {
            return;
        }
        if (!CParticleModule::IsIdentifier(Name))
        {
            Compiler.AddError({ "Get Attribute", "Attribute '" + Name + "' is not a valid identifier.", this });
            return;
        }
        const FString Read = Compiler.Attribute(Name.c_str());
        BindOutputs(Compiler, this, { Read.c_str() });
    }

    void CParticleNode_ArrayElement::BuildNode()
    {
        AddInput("Index", EParticlePinType::Float);
        AddOutput("Value", EParticlePinType::Float4);
        AddOutput("Count", EParticlePinType::Float);
    }

    void CParticleNode_ArrayElement::GenerateDefinition(FParticleCompiler& Compiler)
    {
        const FString Array = Compiler.ArrayInput(ArrayName);
        const FString Index = FParticleCompiler::Coerce(Compiler.GetInputValue(Cast<CParticleInput>(GetInputPins()[0].Get())), EParticlePinType::Float);
        const FString Value = "ArrayAt(" + Array + ", (uint)max(" + Index + ", 0.0))";
        const FString Count = "(float)ArrayCount(" + Array + ")";
        BindOutputs(Compiler, this, { Value.c_str(), Count.c_str() });
    }

    void CParticleNode_SampleGrid::BuildNode()
    {
        AddInput("Position", EParticlePinType::Float3);
        AddOutput("Value", EParticlePinType::Float);
        AddOutput("Gradient", EParticlePinType::Float3);
    }

    void CParticleNode_SampleGrid::GenerateDefinition(FParticleCompiler& Compiler)
    {
        CParticleInput* PositionPin = Cast<CParticleInput>(GetInputPins()[0].Get());
        const FString Position = PositionPin->HasConnection()
            ? FParticleCompiler::Coerce(Compiler.GetInputValue(PositionPin), EParticlePinType::Float3)
            : FString(IsOnCell(Compiler.GetContext()) ? "WorldPos" : "P.Position");
        const FString Channel  = ChannelOf(Compiler, this->Channel);
        const FString Value    = "SampleGrid(" + Channel + ", " + Position + ")";
        const FString Gradient = "GridGradient(" + Channel + ", " + Position + ")";
        BindOutputs(Compiler, this, { Value.c_str(), Gradient.c_str() });
    }

    void CParticleNode_Cell::BuildNode()
    {
        AddOutput("Value", EParticlePinType::Float);
        AddOutput("Neighbor Average", EParticlePinType::Float);
        AddOutput("Laplacian", EParticlePinType::Float);
    }

    void CParticleNode_Cell::GenerateDefinition(FParticleCompiler& Compiler)
    {
        if (!Require(Compiler, this, IsOnCell(Compiler.GetContext())))
        {
            return;
        }

        // A render target texel has no neighbors of its own, so the stencil outputs fall back to the value there.
        const FString Channel  = ChannelOf(Compiler, this->Channel);
        const FString Value    = "Cell[" + Channel + "]";
        const bool    bStage   = Compiler.GetContext() == EParticleContext::Grid;
        const FString Average  = bStage ? "0.25 * (GridLaplacian(Coord, " + Channel + ") + 4.0 * GridAt(Coord, " + Channel + "))" : Value;
        const FString Laplace  = bStage ? "GridLaplacian(Coord, " + Channel + ")" : FString("0.0");
        BindOutputs(Compiler, this, { Value.c_str(), Average.c_str(), Laplace.c_str() });
    }

    void CParticleNode_GridNeighbor::BuildNode()
    {
        AddOutput("Value", EParticlePinType::Float);
    }

    void CParticleNode_GridNeighbor::GenerateDefinition(FParticleCompiler& Compiler)
    {
        if (Require(Compiler, this, Compiler.GetContext() == EParticleContext::Grid))
        {
            const FString Read = Format("GridAt(Coord + int2({}, {}), ", OffsetX, OffsetY).c_str() + ChannelOf(Compiler, Channel) + ")";
            BindOutputs(Compiler, this, { Read.c_str() });
        }
    }

    void CParticleNode_CellInfo::BuildNode()
    {
        AddOutput("UV", EParticlePinType::Float2);
        AddOutput("World Position", EParticlePinType::Float3);
        AddOutput("Coord", EParticlePinType::Float2);
        AddOutput("Iteration", EParticlePinType::Float);
    }

    void CParticleNode_CellInfo::GenerateDefinition(FParticleCompiler& Compiler)
    {
        if (!Require(Compiler, this, IsOnCell(Compiler.GetContext())))
        {
            return;
        }
        const bool bStage = Compiler.GetContext() == EParticleContext::Grid;
        BindOutputs(Compiler, this, { "UV", "WorldPos", bStage ? "float2(Coord)" : "floor(UV * float2(GridSize()))", bStage ? "(float)Iteration" : "0.0" });
    }

    // Calls Visit with every material formula class and its formula.
    template<typename TVisit>
    static void ForEachFormulaClass(TVisit&& Visit)
    {
        CClass* Base = CMaterialExpression_Formula::StaticClass();
        TVector<CClass*> Classes;
        ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object != nullptr && Object->IsA<CClass>() && Object != Base && static_cast<CClass*>(Object)->IsChildOf(Base))
            {
                Classes.push_back(static_cast<CClass*>(Object));
            }
        });

        // Resolving a default object can create objects, so it waits until the walk is over.
        for (CClass* Class : Classes)
        {
            Visit(Class, Class->GetDefaultObject<CMaterialExpression_Formula>()->GetFormula());
        }
    }

    static const FMaterialFormula* FindFormula(const FName& ClassName)
    {
        const FMaterialFormula* Found = nullptr;
        if (!ClassName.IsNone())
        {
            ForEachFormulaClass([&](CClass* Class, const FMaterialFormula& Formula)
            {
                Found = Class->GetName() == ClassName ? &Formula : Found;
            });
        }
        return Found;
    }

    const FMaterialFormula* CParticleNode_Formula::GetFormula() const
    {
        if (CachedFormula == nullptr)
        {
            CachedFormula = FindFormula(FormulaClass);
        }
        return CachedFormula;
    }

    // Vertex and pixel inputs a material formula can read in its code, none of which a simulation has.
    static constexpr FStringView MaterialStageSymbols[] = { "WorldTangent", "WorldNormal", "WorldPosition", "UV0", "VertexColor" };

    static bool UsesMaterialStageData(const FMaterialFormula& Formula)
    {
        return Algo::AnyOf(MaterialStageSymbols, [&](FStringView Symbol) { return Formula.Code.find(Symbol) != FStringView::npos; });
    }

    // A material's unwired default in its particle equivalent, or empty where a particle has none.
    static FString ParticleFallback(FStringView Fallback, int32 FallbackWidth, EParticleContext Context, int32& OutWidth)
    {
        const bool bOnCell = IsOnCell(Context);
        if (Fallback.starts_with("float"))
        {
            OutWidth = Math::Max(FallbackWidth, 1);
            return FString(Fallback);
        }
        if (Fallback == "WorldNormal.xyz")
        {
            // The grid lies across the emitter's right and forward axes, so its surface faces the emitter's up.
            OutWidth = 3;
            return "SimParams().EmitterUp.xyz";
        }
        if (Fallback == "UV0" && bOnCell)
        {
            OutWidth = 2;
            return "UV";
        }
        if (Fallback == "WorldPosition")
        {
            OutWidth = 3;
            return bOnCell ? "WorldPos" : "P.Position";
        }
        if (Fallback == "GetTime()")
        {
            OutWidth = 1;
            return "SimParams().Timing.y";
        }
        return {};
    }

    FFixedString CParticleNode_Formula::GetNodeCategory() const
    {
        const FMaterialFormula* Formula = GetFormula();
        return Formula != nullptr ? FFixedString(Formula->Category.data(), Formula->Category.size()) : FFixedString("Math");
    }

    FStringView CParticleNode_Formula::GetNodeDisplayName() const
    {
        const FMaterialFormula* Formula = GetFormula();
        return Formula != nullptr ? Formula->Name : FStringView("Formula");
    }

    FStringView CParticleNode_Formula::GetNodeTooltip() const
    {
        const FMaterialFormula* Formula = GetFormula();
        return Formula != nullptr ? Formula->Tooltip : FStringView("One of the material graph's formulas.");
    }

    void CParticleNode_Formula::GetVariants(TVector<FGraphNodeVariant>& OutVariants) const
    {
        ForEachFormulaClass([&](CClass* Class, const FMaterialFormula& Formula)
        {
            // Screen-space derivatives exist only in pixel shaders, and the simulation is compute.
            if (Formula.Limit == EMaterialFormulaLimit::PixelOnly || UsesMaterialStageData(Formula))
            {
                return;
            }

            FGraphNodeVariant& Variant = OutVariants.emplace_back();
            Variant.Name        = Class->GetName();
            Variant.DisplayName = FString(Formula.Name);
            Variant.Category    = FString(Formula.Category);
            Variant.Tooltip     = FString(Formula.Tooltip);
        });
    }

    void CParticleNode_Formula::BuildNode()
    {
        const FMaterialFormula* Formula = GetFormula();
        if (Formula == nullptr)
        {
            AddOutput("Result", EParticlePinType::Float);
            return;
        }

        for (const FMaterialFormulaPin& Pin : Formula->Inputs)
        {
            AddInput(Pin.Name, Pin.Width == 0 ? EParticlePinType::Float : FParticleCompiler::TypeOfWidth(Pin.Width), FVector4(Pin.Default));
        }
        if (Formula->Outputs.empty())
        {
            AddOutput("Result", FParticleCompiler::TypeOfWidth(Formula->Width));
        }
        for (const FMaterialFormulaPin& Pin : Formula->Outputs)
        {
            AddOutput(Pin.Name, FParticleCompiler::TypeOfWidth(Pin.Width));
        }
    }

    void CParticleNode_Formula::GenerateDefinition(FParticleCompiler& Compiler)
    {
        const FMaterialFormula* Formula = GetFormula();
        if (Formula == nullptr)
        {
            Compiler.AddError({ "Formula", "The formula this node was made from no longer exists.", this });
            return;
        }
        if (Formula->Limit == EMaterialFormulaLimit::PixelOnly || UsesMaterialStageData(*Formula))
        {
            Compiler.AddError({ FString(Formula->Name), FString(Formula->Name) + " reads surface data that only a material has.", this });
            return;
        }

        const auto& Inputs = GetInputPins();
        TVector<TPair<FString, int32>> Reads;
        int32 SharedWidth = 1;
        for (size_t Index = 0; Index < Formula->Inputs.size() && Index < Inputs.size(); ++Index)
        {
            const FMaterialFormulaPin& Spec = Formula->Inputs[Index];
            CParticleInput* Pin = Cast<CParticleInput>(Inputs[Index].Get());

            FString Expr;
            int32   Width = 1;
            if (!Pin->HasConnection() && Spec.Fallback != nullptr)
            {
                Expr = ParticleFallback(Spec.Fallback, Spec.Width, Compiler.GetContext(), Width);
                if (Expr.empty())
                {
                    Compiler.AddError({ FString(Formula->Name), FString("Wire ") + Spec.Name + ", which has no default outside a material.", this });
                    Expr = "0.0";
                    Width = 1;
                }
            }
            else
            {
                const FParticleInputValue Value = Compiler.GetInputValue(Pin);
                Expr  = Value.Value;
                Width = FParticleCompiler::Width(Value.Type);
            }

            if (Spec.Width == 0)
            {
                SharedWidth = Math::Max(SharedWidth, Width);
            }
            Reads.emplace_back(Expr, Width);
        }

        const FString Prefix = FString(GetNodeFullName());
        Compiler.EmitCurrent(ExpandFormula(*Formula, Prefix, TSpan<const TPair<FString, int32>>(Reads.data(), Reads.size()), SharedWidth));

        const auto& Outputs = GetOutputPins();
        auto Bind = [&](size_t Index, const FString& Var, int32 Width)
        {
            if (CParticleOutput* Output = Index < Outputs.size() ? Cast<CParticleOutput>(Outputs[Index].Get()) : nullptr)
            {
                Output->ResolvedVar = Var;
                Output->SetPinType(FParticleCompiler::TypeOfWidth(Width == 0 ? SharedWidth : Width));
            }
        };
        if (Formula->Outputs.empty())
        {
            Bind(0, Prefix, Formula->Width);
        }
        for (size_t Index = 0; Index < Formula->Outputs.size(); ++Index)
        {
            Bind(Index, Prefix + "_" + Formula->Outputs[Index].Name, Formula->Outputs[Index].Width);
        }
    }

    FString CParticleSinkNode::InputVar(int32 Pin)
    {
        return FString(GetNodeFullName()) + "_in" + Format("{}", Pin).c_str();
    }

    bool CParticleSinkNode::IsWired(int32 Pin) const
    {
        return Pin < (int32)GetInputPins().size() && GetInputPins()[Pin]->HasConnection();
    }

    void CParticleSinkNode::GenerateDefinition(FParticleCompiler& Compiler)
    {
        const auto& Inputs = GetInputPins();
        for (int32 Index = 0; Index < (int32)Inputs.size(); ++Index)
        {
            CParticleInput* Pin = Cast<CParticleInput>(Inputs[Index].Get());
            const FString Read = FParticleCompiler::Coerce(Compiler.GetInputValue(Pin), Pin->GetPinType());
            Compiler.EmitCurrent("const " + FParticleCompiler::TypeName(Pin->GetPinType()) + " " + InputVar(Index) + " = " + Read + ";");
        }
    }

    void CParticleSinkNode::EmitWrites(FParticleCompiler& Compiler)
    {
        Write(Compiler);
    }

    void CParticleNode_SetParticle::BuildNode()
    {
        AddInput("Position", EParticlePinType::Float3);
        AddInput("Velocity", EParticlePinType::Float3);
        AddInput("Color", EParticlePinType::Float4, FVector4(1.0f));
        AddInput("Size", EParticlePinType::Float, FVector4(0.1f));
        AddInput("Rotation", EParticlePinType::Float);
        AddInput("Lifetime", EParticlePinType::Float, FVector4(1.0f));
    }

    void CParticleNode_SetParticle::Write(FParticleCompiler& Compiler)
    {
        if (!Require(Compiler, this, !IsOnCell(Compiler.GetContext())))
        {
            return;
        }

        constexpr const char* Fields[] = { "P.Position", "P.Velocity", "P.Color", "P.Size", "P.Rotation", "P.Lifetime" };
        for (int32 Index = 0; Index < (int32)std::size(Fields); ++Index)
        {
            if (IsWired(Index))
            {
                Compiler.EmitCurrent(FString(Fields[Index]) + " = " + InputVar(Index) + ";");
            }
        }
    }

    void CParticleNode_SetAttribute::BuildNode()
    {
        AddInput("Value", EParticlePinType::Float);
    }

    void CParticleNode_SetAttribute::Write(FParticleCompiler& Compiler)
    {
        const FString Name(AttributeName.ToString().c_str());
        if (!Require(Compiler, this, !IsOnCell(Compiler.GetContext())))
        {
            return;
        }
        if (!CParticleModule::IsIdentifier(Name))
        {
            Compiler.AddError({ "Set Attribute", "Attribute '" + Name + "' is not a valid identifier.", this });
            return;
        }
        Compiler.EmitCurrent(Compiler.Attribute(Name.c_str()) + " = " + InputVar(0) + ";");
    }

    void CParticleNode_WriteToGrid::BuildNode()
    {
        AddInput("Value", EParticlePinType::Float, FVector4(1.0f));
        AddInput("Position", EParticlePinType::Float3);
    }

    void CParticleNode_WriteToGrid::Write(FParticleCompiler& Compiler)
    {
        if (Require(Compiler, this, !IsOnCell(Compiler.GetContext())))
        {
            Compiler.EmitCurrent("DepositGrid(" + ChannelOf(Compiler, Channel) + ", " + (IsWired(1) ? InputVar(1) : FString("P.Position")) + ", " + InputVar(0) + ");");
        }
    }

    void CParticleNode_SetCell::BuildNode()
    {
        AddInput("Value", EParticlePinType::Float);
    }

    void CParticleNode_SetCell::Write(FParticleCompiler& Compiler)
    {
        if (Require(Compiler, this, Compiler.GetContext() == EParticleContext::Grid))
        {
            Compiler.EmitCurrent("Cell[" + ChannelOf(Compiler, Channel) + "] = " + InputVar(0) + ";");
        }
    }

    void CParticleNode_SetTargetColor::BuildNode()
    {
        AddInput("Color", EParticlePinType::Float4, FVector4(1.0f));
    }

    void CParticleNode_SetTargetColor::Write(FParticleCompiler& Compiler)
    {
        if (Require(Compiler, this, Compiler.GetContext() == EParticleContext::Target))
        {
            Compiler.EmitCurrent("Color = " + InputVar(0) + ";");
        }
    }
}
