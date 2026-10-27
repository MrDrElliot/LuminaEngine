#include "ParticleAttributeModules.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

namespace Lumina
{
    namespace
    {
        FString TargetField(EParticleValueTarget Target)
        {
            switch (Target)
            {
            case EParticleValueTarget::Velocity:      return "P.Velocity";
            case EParticleValueTarget::Color:         return "P.Color";
            case EParticleValueTarget::Alpha:         return "P.Color.a";
            case EParticleValueTarget::Size:          return "P.Size";
            case EParticleValueTarget::Rotation:      return "P.Rotation";
            case EParticleValueTarget::RotationSpeed: return "P.RotationSpeed";
            case EParticleValueTarget::Lifetime:      return "P.Lifetime";
            default:                                  return FString();
            }
        }

        // Normalized position of the particle's speed across [Min, Max].
        FString SpeedFraction(FParticleCompiler& Compiler, const SParticleParam& MinSpeed, const SParticleParam& MaxSpeed)
        {
            const FString Min = Compiler.Param("MinSpeed", MinSpeed);
            const FString Max = Compiler.Param("MaxSpeed", MaxSpeed);
            return "saturate((length(P.Velocity) - " + Min + ") / max(" + Max + " - " + Min + ", 1e-4))";
        }
    }

    void CParticleModule_SetValue::SyncValueWidth()
    {
        EParticleParameterType Type = EParticleParameterType::Float;
        if (Target == EParticleValueTarget::Velocity)
        {
            Type = EParticleParameterType::Vec3;
        }
        else if (Target == EParticleValueTarget::Color)
        {
            Type = EParticleParameterType::Color;
        }
        Value.Type = Type;
        Max.Type = Type;
    }

    void CParticleModule_SetValue::GenerateInto(FParticleCompiler& Compiler, EParticleContext Context, int32 ModuleIndex, const FString& Scale, bool bPerSecond)
    {
        SyncValueWidth();

        FString Field = TargetField(Target);
        if (Target == EParticleValueTarget::Attribute)
        {
            const FString Name(AttributeName.ToString().c_str());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Set Particle Value", "Attribute Name '" + Name + "' is not a valid identifier." });
                return;
            }
            Field = Compiler.Attribute(Name.c_str());
        }

        const bool bSpawn = Context == EParticleContext::Spawn;
        const uint32 Width = ParticleParamComponents(Value.Type);
        const FString TypeName = Width == 1 ? "float" : (Width == 3 ? "float3" : "float4");
        const FString V = LocalVar(ModuleIndex, "value");

        FString Expr = Compiler.Param("Value", Value);
        if (bRandomRange)
        {
            // Spawn draws from the particle's seed, and update code has none, so it hashes the particle's own seed instead.
            const auto Random = [&](uint32 Salt) -> FString
            {
                return bSpawn ? FString("RandUnit(Seed)") : FString(Format("StableRandom(P.Seed, {}u)", (uint32)ModuleIndex * 4u + Salt));
            };
            FString T = Random(0);
            if (Width == 3)
            {
                T = "float3(" + Random(0) + ", " + Random(1) + ", " + Random(2) + ")";
            }
            Expr = "lerp(" + Expr + ", " + Compiler.Param("Max", Max) + ", " + T + ")";
        }

        if (Target == EParticleValueTarget::Rotation || Target == EParticleValueTarget::RotationSpeed)
        {
            Expr = "radians(" + Expr + ")";
        }
        if (Target == EParticleValueTarget::Velocity && bEmitterSpace)
        {
            Expr = "EmitterToWorld(" + Expr + ")";
        }
        if (!Scale.empty())
        {
            Expr = "(" + Expr + ") * " + Scale;
        }

        Compiler.Emit(Context, "{");
        Compiler.Emit(Context, "\tconst " + TypeName + " " + V + " = " + Expr + ";");

        switch (Operation)
        {
        case EParticleValueOperation::Set:
            Compiler.Emit(Context, "\t" + Field + " = " + V + ";");
            break;
        case EParticleValueOperation::Add:
            Compiler.Emit(Context, "\t" + Field + " += " + V + (bPerSecond ? " * DeltaTime;" : ";"));
            break;
        case EParticleValueOperation::Multiply:
            Compiler.Emit(Context, "\t" + Field + " *= " + (bPerSecond ? "pow(max(" + V + ", 0.0), DeltaTime);" : V + ";"));
            break;
        }
        Compiler.Emit(Context, "}");
    }

    void CParticleModule_SetValueSpawn::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Spawn, ModuleIndex, FString(), false);
    }

    CParticleModule_SetValueUpdate::CParticleModule_SetValueUpdate()
    {
        Curve.Curve.AddKey(0.0f, 1.0f);
        Curve.Curve.AddKey(1.0f, 0.0f);
    }

    void CParticleModule_SetValueUpdate::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Scale = bScaleByCurve ? "SampleCurveLUT(" + Compiler.ParamCurve("Curve", Curve) + ", LifeRatio)" : FString();
        GenerateInto(Compiler, EParticleContext::Update, ModuleIndex, Scale, bPerSecond);
    }

    void CParticleModule_KillInVolume::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Local = LocalVar(ModuleIndex, "local");
        const FString Inside = LocalVar(ModuleIndex, "inside");
        Compiler.EmitUpdate("const float3 " + Local + " = WorldToEmitter(P.Position - SimParams().EmitterPosition.xyz) / EmitterScale() - "
            + Compiler.Param("Center", Center) + ";");

        switch (Shape)
        {
        case EParticleKillShape::Sphere:
            Compiler.EmitUpdate("const bool " + Inside + " = length(" + Local + ") <= " + Compiler.Param("Radius", Radius) + ";");
            break;
        case EParticleKillShape::Box:
            Compiler.EmitUpdate("const bool " + Inside + " = all(abs(" + Local + ") <= " + Compiler.Param("HalfExtent", HalfExtent) + ");");
            break;
        case EParticleKillShape::Plane:
            Compiler.EmitUpdate("const bool " + Inside + " = " + Local + ".y <= 0.0;");
            break;
        }

        Compiler.EmitUpdate(FString("if (") + (bKillInside ? "" : "!") + Inside + ") { P.Age = P.Lifetime; }");
    }

    void CParticleModule_KillBySpeed::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Speed = LocalVar(ModuleIndex, "speed");
        const FString Min = Compiler.Param("MinSpeed", MinSpeed);
        const FString Max = Compiler.Param("MaxSpeed", MaxSpeed);
        Compiler.EmitUpdate("const float " + Speed + " = length(P.Velocity);");
        Compiler.EmitUpdate("if (P.Age >= " + Compiler.Param("MinAge", MinAge) + " && ((" + Min + " > 0.0 && " + Speed + " < " + Min
            + ") || (" + Max + " > 0.0 && " + Speed + " > " + Max + "))) { P.Age = P.Lifetime; }");
    }

    CParticleModule_ColorBySpeed::CParticleModule_ColorBySpeed()
    {
        Gradient.AddKey(0.0f, FVector4(0.6f, 0.1f, 0.02f, 1.0f));
        Gradient.AddKey(1.0f, FVector4(1.0f, 0.9f, 0.6f, 1.0f));
    }

    void CParticleModule_ColorBySpeed::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Ramp = "SampleGradientLUT(" + Compiler.ParamGradient("Gradient", Gradient) + ", " + SpeedFraction(Compiler, MinSpeed, MaxSpeed) + ")";
        switch (Blend)
        {
        case EParticleSpeedBlend::Replace:
            Compiler.EmitUpdate("P.Color = " + Ramp + ";");
            break;
        case EParticleSpeedBlend::MultiplyCurrent:
            Compiler.EmitUpdate("P.Color *= " + Ramp + ";");
            break;
        case EParticleSpeedBlend::ScaleSpawnValue:
        {
            // The same names Color Over Life keeps the spawn color under, so the two share one copy.
            const FString R = Compiler.Attribute("SpawnColorR", "1.0");
            const FString G = Compiler.Attribute("SpawnColorG", "1.0");
            const FString B = Compiler.Attribute("SpawnColorB", "1.0");
            const FString A = Compiler.Attribute("SpawnColorA", "1.0");
            Compiler.EmitSpawn(R + " = P.Color.r;");
            Compiler.EmitSpawn(G + " = P.Color.g;");
            Compiler.EmitSpawn(B + " = P.Color.b;");
            Compiler.EmitSpawn(A + " = P.Color.a;");
            Compiler.EmitUpdate("P.Color = " + Ramp + " * float4(" + R + ", " + G + ", " + B + ", " + A + ");");
            break;
        }
        }
    }

    CParticleModule_SizeBySpeed::CParticleModule_SizeBySpeed()
    {
        Curve.Curve.AddKey(0.0f, 0.25f);
        Curve.Curve.AddKey(1.0f, 1.0f);
    }

    void CParticleModule_SizeBySpeed::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Factor = "SampleCurveLUT(" + Compiler.ParamCurve("Curve", Curve) + ", " + SpeedFraction(Compiler, MinSpeed, MaxSpeed) + ")";
        switch (Blend)
        {
        case EParticleSpeedBlend::Replace:
            Compiler.EmitUpdate("P.Size = " + Factor + ";");
            break;
        case EParticleSpeedBlend::MultiplyCurrent:
            Compiler.EmitUpdate("P.Size *= " + Factor + ";");
            break;
        case EParticleSpeedBlend::ScaleSpawnValue:
        {
            // The same name Size Over Life keeps the spawn size under.
            const FString BaseSize = Compiler.Attribute("BaseSize", "0.1");
            Compiler.EmitSpawn(BaseSize + " = P.Size;");
            Compiler.EmitUpdate("P.Size = " + BaseSize + " * " + Factor + ";");
            break;
        }
        }
    }

    void CParticleModule_SpawnFromArray::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        const FString Array = Compiler.ArrayInput(Positions);
        const FString Pick  = LocalVar(ModuleIndex, "pick");
        const FString Point = LocalVar(ModuleIndex, "point");

        Compiler.EmitSpawn("if (ArrayCount(" + Array + ") > 0u)");
        Compiler.EmitSpawn("{");
        Compiler.EmitSpawn("\tconst uint " + Pick + " = " + (bRandomPick ? "min((uint)(RandUnit(Seed) * ArrayCount(" + Array + ")), ArrayCount(" + Array + ") - 1u)"
                                                                       : "Index % ArrayCount(" + Array + ")") + ";");
        Compiler.EmitSpawn("\tconst float4 " + Point + " = ArrayAt(" + Array + ", " + Pick + ");");
        Compiler.EmitSpawn(FString("\tP.Position = ") + (bEmitterSpace ? "SpawnOrigin + EmitterToWorld(" + Point + ".xyz) * EmitterScale();" : Point + ".xyz;"));
        if (bSizeFromW)
        {
            Compiler.EmitSpawn("\tP.Size = " + Point + ".w;");
        }
        if (!Colors.IsNone())
        {
            Compiler.EmitSpawn("\tP.Color = ArrayAt(" + Compiler.ArrayInput(Colors) + ", " + Pick + ");");
        }
        Compiler.EmitSpawn("}");
    }
}
