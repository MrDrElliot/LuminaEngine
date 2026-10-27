#include "ParticleGridModules.h"
#include "Containers/String.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

namespace Lumina
{
    static void EmitGrid(FParticleCompiler& Compiler, const FString& Line)
    {
        Compiler.Emit(EParticleContext::Grid, Line);
    }

    static void EmitTarget(FParticleCompiler& Compiler, const FString& Line)
    {
        Compiler.Emit(EParticleContext::Target, Line);
    }

    static FString Cell(const FString& Channel)
    {
        return "Cell[" + Channel + "]";
    }

    static FString At(const char* Offset, const FString& Channel)
    {
        return FString("GridAt(Coord + int2(") + Offset + "), " + Channel + ")";
    }

    // Applies Value to Lvalue the way Operation says, where Mask fades the whole effect in.
    static FString ApplyOperation(EParticleGridOperation Operation, const FString& Lvalue, const FString& Value, const FString& Mask)
    {
        switch (Operation)
        {
        case EParticleGridOperation::Set:      return Lvalue + " = lerp(" + Lvalue + ", " + Value + ", " + Mask + ");";
        case EParticleGridOperation::Multiply: return Lvalue + " *= lerp(1.0, " + Value + ", " + Mask + ");";
        case EParticleGridOperation::Max:      return Lvalue + " = max(" + Lvalue + ", " + Value + " * " + Mask + ");";
        default:                               return Lvalue + " += " + Value + " * " + Mask + ";";
        }
    }

    void CParticleModule_GridBrush::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Slot = Compiler.GridChannel(Channel);
        const FString Mask    = LocalVar(ModuleIndex, "mask");
        const FString Amount  = LocalVar(ModuleIndex, "amount");

        switch (Shape)
        {
        case EParticleGridBrushShape::Circle:
            EmitGrid(Compiler, "const float " + Mask + " = GridBrushMask(distance(UV, " + Compiler.Param("Center", Center) + "), "
                + Compiler.Param("Radius", Radius) + ", " + Compiler.Param("Hardness", Hardness) + ");");
            break;
        case EParticleGridBrushShape::Box:
        {
            const FString Offset = LocalVar(ModuleIndex, "offset");
            EmitGrid(Compiler, "const float2 " + Offset + " = abs(UV - " + Compiler.Param("Center", Center) + ");");
            EmitGrid(Compiler, "const float " + Mask + " = GridBrushMask(max(" + Offset + ".x, " + Offset + ".y), "
                + Compiler.Param("Radius", Radius) + ", " + Compiler.Param("Hardness", Hardness) + ");");
            break;
        }
        case EParticleGridBrushShape::Border:
            EmitGrid(Compiler, "const float " + Mask + " = (any(Coord == int2(0, 0)) || any(Coord == int2(GridSize()) - 1)) ? 1.0 : 0.0;");
            break;
        default:
            EmitGrid(Compiler, "const float " + Mask + " = 1.0;");
            break;
        }

        const bool bRate = Operation == EParticleGridOperation::Add && bPerSecond;
        EmitGrid(Compiler, "const float " + Amount + " = " + Compiler.Param("Value", Value) + (bRate ? " * DeltaTime;" : ";"));
        EmitGrid(Compiler, ApplyOperation(Operation, Cell(Slot), Amount, Mask));
    }

    void CParticleModule_GridDiffuse::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Slot = Compiler.GridChannel(Channel);
        const FString Average = LocalVar(ModuleIndex, "average");
        EmitGrid(Compiler, "const float " + Average + " = 0.25 * (" + At("1, 0", Slot) + " + " + At("-1, 0", Slot) + " + "
            + At("0, 1", Slot) + " + " + At("0, -1", Slot) + ");");
        EmitGrid(Compiler, Cell(Slot) + " = lerp(" + Cell(Slot) + ", " + Average + ", saturate(" + Compiler.Param("Rate", Rate) + " * DeltaTime));");
    }

    void CParticleModule_GridDecay::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Slot = Compiler.GridChannel(Channel);
        EmitGrid(Compiler, Cell(Slot) + " = lerp(" + Compiler.Param("Target", Target) + ", " + Cell(Slot) + ", exp(-"
            + Compiler.Param("Rate", Rate) + " * DeltaTime));");
    }

    CParticleModule_GridAdvect::CParticleModule_GridAdvect()
    {
        Channels.push_back(FName("Density"));
    }

    void CParticleModule_GridAdvect::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Velocity = LocalVar(ModuleIndex, "velocity");
        const FString From     = LocalVar(ModuleIndex, "from");

        EmitGrid(Compiler, "float2 " + Velocity + " = " + Compiler.Param("Wind", Wind) + ";");
        if (!VelocityX.IsNone() && !VelocityY.IsNone())
        {
            EmitGrid(Compiler, Velocity + " += float2(GridAt(Coord, " + Compiler.GridChannel(VelocityX) + "), GridAt(Coord, " + Compiler.GridChannel(VelocityY) + "));");
        }

        // Semi-Lagrangian, each cell pulls its value from where the flow would have brought it, which stays stable at any speed.
        EmitGrid(Compiler, "const float2 " + From + " = UV - " + Velocity + " * DeltaTime / GridWorldSize();");
        for (const FName& Name : Channels)
        {
            // What earlier modules in this stage added to the cell stays in it rather than being replaced by the moved value.
            const FString Channel = Compiler.GridChannel(Name);
            EmitGrid(Compiler, Cell(Channel) + " += SampleGridUV(" + Channel + ", " + From + ") - GridAt(Coord, " + Channel + ");");
        }
    }

    void CParticleModule_FluidDivergence::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString X    = Compiler.GridChannel(VelocityX);
        const FString Y    = Compiler.GridChannel(VelocityY);
        const FString Size = LocalVar(ModuleIndex, "cell");
        EmitGrid(Compiler, "const float2 " + Size + " = GridCellSize();");
        EmitGrid(Compiler, Cell(Compiler.GridChannel(Divergence)) + " = 0.5 * ((" + At("1, 0", X) + " - " + At("-1, 0", X) + ") / " + Size + ".x + ("
            + At("0, 1", Y) + " - " + At("0, -1", Y) + ") / " + Size + ".y);");
    }

    void CParticleModule_FluidPressure::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString P  = Compiler.GridChannel(Pressure);
        const FString D  = Compiler.GridChannel(Divergence);
        const FString H2 = LocalVar(ModuleIndex, "h2");
        EmitGrid(Compiler, "const float2 " + H2 + " = GridCellSize() * GridCellSize();");
        EmitGrid(Compiler, Cell(P) + " = ((" + At("1, 0", P) + " + " + At("-1, 0", P) + ") * " + H2 + ".y + (" + At("0, 1", P) + " + " + At("0, -1", P) + ") * " + H2 + ".x - "
            + "GridAt(Coord, " + D + ") * " + H2 + ".x * " + H2 + ".y) / (2.0 * (" + H2 + ".x + " + H2 + ".y));");
    }

    void CParticleModule_FluidProject::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString P    = Compiler.GridChannel(Pressure);
        const FString Size = LocalVar(ModuleIndex, "cell");
        EmitGrid(Compiler, "const float2 " + Size + " = GridCellSize();");
        EmitGrid(Compiler, Cell(Compiler.GridChannel(VelocityX)) + " -= (" + At("1, 0", P) + " - " + At("-1, 0", P) + ") / (2.0 * " + Size + ".x);");
        EmitGrid(Compiler, Cell(Compiler.GridChannel(VelocityY)) + " -= (" + At("0, 1", P) + " - " + At("0, -1", P) + ") / (2.0 * " + Size + ".y);");
    }

    void CParticleModule_GridWave::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString H       = Compiler.GridChannel(Height);
        const FString V       = Compiler.GridChannel(HeightVelocity);
        const FString Courant = LocalVar(ModuleIndex, "courant");

        // Past about 0.7 cells a step the explicit solve blows up, so faster waves are held there.
        EmitGrid(Compiler, "const float " + Courant + " = min(" + Compiler.Param("Speed", Speed) + " * DeltaTime, 0.7);");
        EmitGrid(Compiler, Cell(V) + " = (" + Cell(V) + " + GridLaplacian(Coord, " + H + ") * " + Courant + " * " + Courant + " / max(DeltaTime, 1e-5)) * exp(-"
            + Compiler.Param("Damping", Damping) + " * DeltaTime);");
        EmitGrid(Compiler, Cell(H) + " += " + Cell(V) + " * DeltaTime;");
    }

    void CParticleModule_ReactionDiffusion::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString A     = Compiler.GridChannel(ChemicalA);
        const FString B     = Compiler.GridChannel(ChemicalB);
        const FString React = LocalVar(ModuleIndex, "react");
        const FString Step  = LocalVar(ModuleIndex, "step");
        const FString FeedRate = LocalVar(ModuleIndex, "feed");

        EmitGrid(Compiler, "const float " + React + " = " + Cell(A) + " * " + Cell(B) + " * " + Cell(B) + ";");
        EmitGrid(Compiler, "const float " + Step + " = " + Compiler.Param("Speed", Speed) + ";");
        EmitGrid(Compiler, "const float " + FeedRate + " = " + Compiler.Param("Feed", Feed) + ";");
        EmitGrid(Compiler, "const float2 " + LocalVar(ModuleIndex, "next") + " = float2(");
        EmitGrid(Compiler, "\t" + Cell(A) + " + (" + Compiler.Param("DiffusionA", DiffusionA) + " * GridLaplacian9(Coord, " + A + ") - " + React + " + " + FeedRate + " * (1.0 - " + Cell(A) + ")) * " + Step + ",");
        EmitGrid(Compiler, "\t" + Cell(B) + " + (" + Compiler.Param("DiffusionB", DiffusionB) + " * GridLaplacian9(Coord, " + B + ") + " + React + " - (" + Compiler.Param("Kill", Kill) + " + " + FeedRate + ") * " + Cell(B) + ") * " + Step + ");");
        EmitGrid(Compiler, Cell(A) + " = saturate(" + LocalVar(ModuleIndex, "next") + ".x);");
        EmitGrid(Compiler, Cell(B) + " = saturate(" + LocalVar(ModuleIndex, "next") + ".y);");
    }

    void CParticleModule_WriteToGrid::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Deposit = LocalVar(ModuleIndex, "amount");
        Compiler.EmitUpdate("const float " + Deposit + " = " + Compiler.Param("Amount", Amount) + (bPerSecond ? " * DeltaTime;" : ";"));

        if (Mode == EParticleGridWriteMode::Velocity)
        {
            const FString Across = LocalVar(ModuleIndex, "across");
            Compiler.EmitUpdate("const float2 " + Across + " = WorldToGridDir(P.Velocity) * " + Deposit + ";");
            Compiler.EmitUpdate("DepositGrid(" + Compiler.GridChannel(VelocityX) + ", P.Position, " + Across + ".x);");
            Compiler.EmitUpdate("DepositGrid(" + Compiler.GridChannel(VelocityY) + ", P.Position, " + Across + ".y);");
            return;
        }

        Compiler.EmitUpdate("DepositGrid(" + Compiler.GridChannel(Channel) + ", P.Position, " + Deposit + ");");
    }

    void CParticleModule_GridForce::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Pull = Compiler.Param("Strength", Strength);
        if (Mode == EParticleGridForceMode::VelocityField)
        {
            const FString Field = LocalVar(ModuleIndex, "field");
            Compiler.EmitUpdate("if (IsInsideGrid(P.Position))");
            Compiler.EmitUpdate("{");
            Compiler.EmitUpdate("\tconst float3 " + Field + " = GridToWorldDir(float2(SampleGrid(" + Compiler.GridChannel(VelocityX) + ", P.Position), SampleGrid("
                + Compiler.GridChannel(VelocityY) + ", P.Position)));");
            Compiler.EmitUpdate("\tP.Velocity += (" + Field + " - GridToWorldDir(WorldToGridDir(P.Velocity))) * saturate(" + Pull + " * DeltaTime);");
            Compiler.EmitUpdate("}");
            return;
        }

        Compiler.EmitUpdate("if (IsInsideGrid(P.Position)) { P.Velocity += GridGradient(" + Compiler.GridChannel(Channel) + ", P.Position) * " + Pull + " * DeltaTime; }");
    }

    void CParticleModule_SampleGrid::GenerateInto(FParticleCompiler& Compiler, EParticleContext Context, int32 ModuleIndex)
    {
        const FString Sample = LocalVar(ModuleIndex, "sample");
        Compiler.Emit(Context, "const float " + Sample + " = SampleGrid(" + Compiler.GridChannel(Channel) + ", P.Position) * "
            + Compiler.Param("Scale", Scale) + " + " + Compiler.Param("Bias", Bias) + ";");

        FString Lvalue;
        switch (Target)
        {
        case EParticleGridSampleTarget::Size:       Lvalue = "P.Size"; break;
        case EParticleGridSampleTarget::Brightness: Lvalue = "P.Color.rgb"; break;
        case EParticleGridSampleTarget::Attribute:
        {
            const FString Name(AttributeName.ToString().c_str());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Sample Grid", "Attribute '" + Name + "' is not a valid identifier." });
                return;
            }
            Lvalue = Compiler.Attribute(Name.c_str());
            break;
        }
        default:                                    Lvalue = "P.Color.a"; break;
        }

        Compiler.Emit(Context, ApplyOperation(Operation, Lvalue, Sample, "1.0"));
    }

    void CParticleModule_SampleGridSpawn::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Spawn, ModuleIndex);
    }

    void CParticleModule_SampleGridUpdate::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Update, ModuleIndex);
    }

    CParticleModule_TargetGradient::CParticleModule_TargetGradient()
    {
        Gradient.AddKey(0.0f, FVector4(0.0f, 0.0f, 0.0f, 1.0f));
        Gradient.AddKey(1.0f, FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    void CParticleModule_TargetGradient::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Low = LocalVar(ModuleIndex, "low");
        EmitTarget(Compiler, "const float " + Low + " = " + Compiler.Param("Min", Min) + ";");
        EmitTarget(Compiler, "Color = SampleGradientLUT(" + Compiler.ParamGradient("Gradient", Gradient) + ", saturate((" + Cell(Compiler.GridChannel(Channel)) + " - " + Low
            + ") / max(" + Compiler.Param("Max", Max) + " - " + Low + ", 1e-5)));");
    }

    void CParticleModule_TargetChannels::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FName Sources[] = { Red, Green, Blue, Alpha };
        const char* Components[] = { "r", "g", "b", "a" };
        for (int32 Component = 0; Component < 4; ++Component)
        {
            if (!Sources[Component].IsNone())
            {
                EmitTarget(Compiler, FString("Color.") + Components[Component] + " = " + Cell(Compiler.GridChannel(Sources[Component])) + ";");
            }
        }
        EmitTarget(Compiler, "Color = Color * " + Compiler.Param("Scale", Scale) + " + " + Compiler.Param("Bias", Bias) + ";");
    }

    void CParticleModule_TargetNormal::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString H     = Compiler.GridChannel(Height);
        const FString Texel = LocalVar(ModuleIndex, "texel");
        const FString Slope = LocalVar(ModuleIndex, "slope");
        EmitTarget(Compiler, "const float2 " + Texel + " = 1.0 / float2(GridSize());");
        EmitTarget(Compiler, "const float2 " + Slope + " = float2(SampleGridUV(" + H + ", UV + float2(" + Texel + ".x, 0.0)) - SampleGridUV(" + H + ", UV - float2(" + Texel + ".x, 0.0)), "
            + "SampleGridUV(" + H + ", UV + float2(0.0, " + Texel + ".y)) - SampleGridUV(" + H + ", UV - float2(0.0, " + Texel + ".y))) * " + Compiler.Param("Strength", Strength) + ";");
        EmitTarget(Compiler, "Color = float4(normalize(float3(-" + Slope + ", 1.0)) * 0.5 + 0.5, 1.0);");
    }
}
