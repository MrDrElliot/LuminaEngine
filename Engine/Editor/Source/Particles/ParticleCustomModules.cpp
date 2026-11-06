#include "ParticleCustomModules.h"
#include "Containers/String.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"


namespace Lumina
{
    static const char* SlangTypeFor(EParticleParameterType Type)
    {
        switch (Type)
        {
        case EParticleParameterType::Vec2:  return "float2";
        case EParticleParameterType::Vec3:  return "float3";
        case EParticleParameterType::Vec4:
        case EParticleParameterType::Color: return "float4";
        default:                            return "float";
        }
    }

    void CParticleModule_CustomCode::GenerateInto(FParticleCompiler& Compiler, EParticleContext Context)
    {
        const char* Directive = "\x23";
        TVector<FString> Macros;

        Compiler.Emit(Context, "{");
        for (const SParticleCustomInput& Input : Inputs)
        {
            const FString Name(*Input.Name.ToString());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Custom Code", "Input '" + Name + "' is not a valid identifier." });
                continue;
            }
            Compiler.Emit(Context, FString("\tconst ") + SlangTypeFor(Input.Value.Type) + " " + Name + " = " + Compiler.Param(*Name, Input.Value) + ";");
        }

        const bool bOnCell = Context == EParticleContext::Grid || Context == EParticleContext::Target;
        if (bOnCell && !Attributes.IsEmpty())
        {
            Compiler.AddError({ "Custom Code", "Grid and render target code runs on cells, which have no particle attributes." });
        }

        for (const FName& Attribute : Attributes)
        {
            const FString Name(*Attribute.ToString());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Custom Code", "Attribute '" + Name + "' is not a valid identifier." });
                continue;
            }
            Compiler.Emit(Context, FString(Directive) + "define " + Name + " " + Compiler.Attribute(*Name));
            Macros.Add(Name);
        }

        for (const FName& Array : Arrays)
        {
            Compiler.ArrayInput(Array);
        }
        if (bUsesNeighbors)
        {
            Compiler.UseNeighborGrid();
        }

        for (const FName& Channel : GridChannels)
        {
            const FString Name(*Channel.ToString());
            const FString Slot = Compiler.GridChannel(Channel);
            if (!IsIdentifier(Name))
            {
                continue;
            }
            Compiler.Emit(Context, FString(Directive) + "define " + Name + (bOnCell ? " Cell[" + Slot + "]" : " SampleGrid(" + Slot + ", P.Position)"));
            Macros.Add(Name);
        }

        for (const SParticleCustomCurve& Input : Curves)
        {
            const FString Name(*Input.Name.ToString());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Custom Code", "Curve '" + Name + "' is not a valid identifier." });
                continue;
            }
            Compiler.Emit(Context, FString(Directive) + "define " + Name + "(T) SampleCurveLUT(" + Compiler.ParamCurve(*Name, Input.Curve) + ", (T))");
            Macros.Add(Name);
        }

        for (const SParticleCustomGradient& Input : Gradients)
        {
            const FString Name(*Input.Name.ToString());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Custom Code", "Gradient '" + Name + "' is not a valid identifier." });
                continue;
            }
            Compiler.Emit(Context, FString(Directive) + "define " + Name + "(T) SampleGradientLUT(" + Compiler.ParamGradient(*Name, Input.Gradient) + ", (T))");
            Macros.Add(Name);
        }

        int32 Start = 0;
        while (Start <= Code.Len())
        {
            const int32 End = Code.Find('\n', Start);
            FString Line = Code.Mid(Start, (End == Constants::kIndexNone ? Code.Len() : End) - Start);
            if (!Line.IsEmpty() && Line.Last() == '\r')
            {
                Line.LeftChopInline(1);
            }
            Compiler.Emit(Context, "\t" + Line);
            if (End == Constants::kIndexNone)
            {
                break;
            }
            Start = End + 1;
        }

        for (const FString& Macro : Macros)
        {
            Compiler.Emit(Context, FString(Directive) + "undef " + Macro);
        }
        Compiler.Emit(Context, "}");
    }

    void CParticleModule_CustomSpawn::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Spawn);
    }

    void CParticleModule_CustomUpdate::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Update);
    }

    void CParticleModule_CustomGrid::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Grid);
    }

    void CParticleModule_CustomTarget::Generate(FParticleCompiler& Compiler, int32 ModuleIndex)
    {
        GenerateInto(Compiler, EParticleContext::Target);
    }
}
