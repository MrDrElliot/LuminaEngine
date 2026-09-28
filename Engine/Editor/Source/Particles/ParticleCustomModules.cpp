#include "ParticleCustomModules.h"
#include "Containers/String.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

#include <cctype>

namespace Lumina
{
    // Names become Slang identifiers and macro names, so anything else would inject arbitrary code.
    static bool IsIdentifier(const FString& Name)
    {
        if (Name.empty() || std::isdigit((unsigned char)Name[0]))
        {
            return false;
        }

        for (char Character : Name)
        {
            if (!std::isalnum((unsigned char)Character) && Character != '_')
            {
                return false;
            }
        }
        return true;
    }

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
            const FString Name(Input.Name.ToString().c_str());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Custom Code", "Input '" + Name + "' is not a valid identifier." });
                continue;
            }
            Compiler.Emit(Context, FString("\tconst ") + SlangTypeFor(Input.Value.Type) + " " + Name + " = " + Compiler.Param(Name.c_str(), Input.Value) + ";");
        }

        for (const FName& Attribute : Attributes)
        {
            const FString Name(Attribute.ToString().c_str());
            if (!IsIdentifier(Name))
            {
                Compiler.AddError({ "Custom Code", "Attribute '" + Name + "' is not a valid identifier." });
                continue;
            }
            Compiler.Emit(Context, FString(Directive) + "define " + Name + " " + Compiler.Attribute(Name.c_str()));
            Macros.push_back(Name);
        }

        size_t Start = 0;
        while (Start <= Code.size())
        {
            const size_t End = Code.find('\n', Start);
            FString Line = Code.substr(Start, End == FString::npos ? FString::npos : End - Start);
            if (!Line.empty() && Line.back() == '\r')
            {
                Line.pop_back();
            }
            Compiler.Emit(Context, "\t" + Line);
            if (End == FString::npos)
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
}
