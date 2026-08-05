#include "ParticleCompiler.h"
#include "ParticleGraphNode.h"
#include "Core/Object/Cast.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"

namespace Lumina
{
    static constexpr const char* SpawnToken         = "$PARTICLE_SPAWN_FUNC";
    static constexpr const char* UpdateToken        = "$PARTICLE_UPDATE_FUNC";
    static constexpr const char* AttrCountToken     = "$PARTICLE_ATTR_COUNT";
    static constexpr const char* AttrDefaultsToken  = "$PARTICLE_ATTR_DEFAULTS";

    // Replaces every Token in Source (single pass, inserted text not rescanned); splices the
    // module-stack chunks into the function-body tokens in ParticleSimulateTemplate.slang.
    static void ReplaceAll(FString& Source, const char* Token, const FString& Replacement)
    {
        const size_t TokenLen = strlen(Token);
        size_t Pos = Source.find(Token);
        while (Pos != FString::npos)
        {
            Source.replace(Pos, TokenLen, Replacement);
            Pos = Source.find(Token, Pos + Replacement.length());
        }
    }

    FString FParticleCompiler::BuildShader() const
    {
        FString Path = Paths::GetEngineResourceDirectory() + "/Shaders/Particles/ParticleSimulateTemplate.slang";
        FString Source;
        if (!FileHelper::LoadFileIntoString(Source, Path))
        {
            return Source;
        }

        // The tokens sit inside SpawnParticleGraph / UpdateParticleGraph in the template, so the
        // chunks are spliced in as raw statement bodies. Empty stacks splice to nothing.
        // Attribute defaults are spliced ahead of the spawn stack so a module that only READS an attribute
        // still sees a sane value when no module wrote it.
        FString AttrDefaults;
        for (int32 i = 0; i < (int32)Attributes.size(); ++i)
        {
            AttrDefaults += FString("\tPAttr()[Index * PARTICLE_ATTR_FLOATS + ") + eastl::to_string(i).c_str()
                          + "u] = " + Attributes[i].DefaultExpr + ";\n";
        }

        ReplaceAll(Source, AttrCountToken,    eastl::to_string(GetAttributeFloatCount()).c_str());
        ReplaceAll(Source, AttrDefaultsToken, AttrDefaults);
        ReplaceAll(Source, SpawnToken,        SpawnChunks);
        ReplaceAll(Source, UpdateToken,       UpdateChunks);

        return Source;
    }

    FString FParticleCompiler::AddParamSlot(const char* DebugName, const FVector4& Value, uint32 Components)
    {
        const uint32 Slot = (uint32)ParamValues.size();
        ParamValues.push_back(Value);

        FString Expr = "MP(" + eastl::to_string(Slot) + ")";
        switch (Components)
        {
        case 1:  Expr += ".x";    break;
        case 2:  Expr += ".xy";   break;
        case 3:  Expr += ".xyz";  break;
        default: break;           // float4 reads the slot whole
        }

        (void)DebugName;
        return Expr;
    }

    FString FParticleCompiler::Attribute(const char* Name, const char* DefaultExpr)
    {
        const FString AttrName(Name);

        int32 Index = INDEX_NONE;
        for (int32 i = 0; i < (int32)Attributes.size(); ++i)
        {
            if (Attributes[i].Name == AttrName)
            {
                Index = i;
                break;
            }
        }

        if (Index == INDEX_NONE)
        {
            Index = (int32)Attributes.size();
            FAttributeDecl& Decl = Attributes.emplace_back();
            Decl.Name        = AttrName;
            Decl.DefaultExpr = DefaultExpr;
        }

        // Addressed rather than named so no struct declaration is needed: FParticleSimArgs is defined in
        // the shared include, which is parsed before any generated code, so it cannot reference a
        // generated type. A plain float* sidesteps that ordering entirely.
        //
        // CONTRACT: this expression names `Index`, so BOTH SpawnParticleGraph and UpdateParticleGraph in
        // ParticleSimulateTemplate.slang must take the particle index under exactly that name. Update
        // originally took no index at all and Spawn called it SpawnIndex, which compiled fine until the
        // first module actually declared an attribute.
        return FString("PAttr()[Index * PARTICLE_ATTR_FLOATS + ") + eastl::to_string(Index).c_str() + "u]";
    }

    int32 FParticleCompiler::FindAttributeSlot(const char* Name) const
    {
        const FString Wanted(Name);
        for (int32 i = 0; i < (int32)Attributes.size(); ++i)
        {
            if (Attributes[i].Name == Wanted)
            {
                return i;
            }
        }
        return -1;
    }

    uint32 FParticleCompiler::GetAttributeFloatCount() const
    {
        return Math::Max((uint32)Attributes.size(), 1u);
    }

    uint64 FParticleCompiler::GetGeneratedCodeHash() const
    {
        uint64 Hash = 0;
        Hash::HashCombine(Hash, eastl::string_view(SpawnChunks.data(), SpawnChunks.size()));
        Hash::HashCombine(Hash, eastl::string_view(UpdateChunks.data(), UpdateChunks.size()));
        return Hash;
    }

    FString FParticleCompiler::Param(const char* DebugName, float Value)
    {
        return AddParamSlot(DebugName, FVector4(Value, 0.0f, 0.0f, 0.0f), 1);
    }

    FString FParticleCompiler::Param(const char* DebugName, const FVector2& Value)
    {
        return AddParamSlot(DebugName, FVector4(Value.x, Value.y, 0.0f, 0.0f), 2);
    }

    FString FParticleCompiler::Param(const char* DebugName, const FVector3& Value)
    {
        return AddParamSlot(DebugName, FVector4(Value.x, Value.y, Value.z, 0.0f), 3);
    }

    FString FParticleCompiler::Param(const char* DebugName, const FVector4& Value)
    {
        return AddParamSlot(DebugName, Value, 4);
    }

    FString FParticleCompiler::ParamCurve(const char* DebugName, const SCurve& Value)
    {
        const SKeyedCurve& Curve = Value.Resolve();

        // Sampled across the curve's OWN time range, then addressed by normalized position in the shader.
        // An over-life curve is authored 0..1, but nothing forces that, and re-normalizing here means a
        // curve keyed over any domain still maps onto LifeRatio without the module having to know.
        float TimeMin = 0.0f, TimeMax = 1.0f;
        Curve.GetTimeRange(TimeMin, TimeMax);
        if (TimeMax - TimeMin < 1e-6f)
        {
            TimeMax = TimeMin + 1.0f;
        }

        const uint32 BaseSlot = (uint32)ParamValues.size();

        // Packed 4 samples per float4 -- a curve is scalar, so one slot each would waste three quarters of
        // the table.
        FVector4 Packed(0.0f, 0.0f, 0.0f, 0.0f);
        for (int32 i = 0; i < kParticleLUTSamples; ++i)
        {
            const float Alpha = (float)i / (float)(kParticleLUTSamples - 1);
            const float Sample = Curve.Evaluate(TimeMin + (TimeMax - TimeMin) * Alpha);

            switch (i & 3)
            {
            case 0: Packed.x = Sample; break;
            case 1: Packed.y = Sample; break;
            case 2: Packed.z = Sample; break;
            default:
                Packed.w = Sample;
                ParamValues.push_back(Packed);
                Packed = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
                break;
            }
        }
        // Flush a partial trailing group so the table is always a whole number of slots.
        if ((kParticleLUTSamples & 3) != 0)
        {
            ParamValues.push_back(Packed);
        }

        (void)DebugName;
        return eastl::to_string(BaseSlot).c_str();
    }

    FString FParticleCompiler::ParamGradient(const char* DebugName, const SGradient& Value)
    {
        float TimeMin = 0.0f, TimeMax = 1.0f;
        Value.GetTimeRange(TimeMin, TimeMax);
        if (TimeMax - TimeMin < 1e-6f)
        {
            TimeMax = TimeMin + 1.0f;
        }

        const uint32 BaseSlot = (uint32)ParamValues.size();

        // One slot per sample: a colour already fills a float4.
        for (int32 i = 0; i < kParticleLUTSamples; ++i)
        {
            const float Alpha = (float)i / (float)(kParticleLUTSamples - 1);
            ParamValues.push_back(Value.Evaluate(TimeMin + (TimeMax - TimeMin) * Alpha));
        }

        (void)DebugName;
        return eastl::to_string(BaseSlot).c_str();
    }

    void FParticleCompiler::EnsureEmitted(CParticleGraphNode* Node, EParticleContext Context)
    {
        if (Node == nullptr)
        {
            return;
        }

        THashSet<CParticleGraphNode*>& EmittedSet = (Context == EParticleContext::Spawn) ? EmittedSpawn : EmittedUpdate;
        if (EmittedSet.find(Node) != EmittedSet.end())
        {
            return;
        }
        EmittedSet.insert(Node);

        const EParticleContext Saved = CurrentContext;
        CurrentContext = Context;
        Node->GenerateDefinition(*this);
        CurrentContext = Saved;
    }

    FParticleInputValue FParticleCompiler::GetInputValue(CParticleInput* Pin)
    {
        FParticleInputValue Result;

        if (Pin && Pin->HasConnection())
        {
            CParticleOutput* Out = Pin->GetConnection<CParticleOutput>(0);
            if (Out)
            {
                CParticleGraphNode* SourceNode = Cast<CParticleGraphNode>(Out->GetOwningNode());
                if (SourceNode)
                {
                    EnsureEmitted(SourceNode, CurrentContext);
                    Result.Value = SourceNode->GetNodeFullName();
                    Result.Type  = Out->GetPinType();
                    return Result;
                }
            }
        }

        if (Pin != nullptr)
        {
            switch (Pin->GetPinType())
            {
            case EParticlePinType::Float:
            {
                const float V = Pin->GetDefaultFloat();
                Result.Value = FString(eastl::to_string(V));
                Result.Type  = EParticlePinType::Float;
                break;
            }
            case EParticlePinType::Float3:
            {
                const FVector3& V = Pin->GetDefaultFloat3();
                Result.Value = "float3(" + FString(eastl::to_string(V.x)) + ", " + FString(eastl::to_string(V.y)) + ", " + FString(eastl::to_string(V.z)) + ")";
                Result.Type  = EParticlePinType::Float3;
                break;
            }
            case EParticlePinType::Float4:
            {
                const FVector4& V = Pin->GetDefaultFloat4();
                Result.Value = "float4(" + FString(eastl::to_string(V.x)) + ", " + FString(eastl::to_string(V.y)) + ", " + FString(eastl::to_string(V.z)) + ", " + FString(eastl::to_string(V.w)) + ")";
                Result.Type  = EParticlePinType::Float4;
                break;
            }
            }
        }

        return Result;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat(CParticleInput* Pin, float Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.empty())
        {
            V.Value = FString(eastl::to_string(Default));
            V.Type  = EParticlePinType::Float;
        }
        return V;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat3(CParticleInput* Pin, const FVector3& Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.empty())
        {
            V.Value = "float3(" + FString(eastl::to_string(Default.x)) + ", " + FString(eastl::to_string(Default.y)) + ", " + FString(eastl::to_string(Default.z)) + ")";
            V.Type  = EParticlePinType::Float3;
        }
        return V;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat4(CParticleInput* Pin, const FVector4& Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.empty())
        {
            V.Value = "float4(" + FString(eastl::to_string(Default.x)) + ", " + FString(eastl::to_string(Default.y)) + ", " + FString(eastl::to_string(Default.z)) + ", " + FString(eastl::to_string(Default.w)) + ")";
            V.Type  = EParticlePinType::Float4;
        }
        return V;
    }

    FString FParticleCompiler::Coerce(const FParticleInputValue& Value, EParticlePinType Target)
    {
        if (Value.Type == Target)
        {
            return Value.Value;
        }

        switch (Target)
        {
        case EParticlePinType::Float:
            return "(" + Value.Value + ").x";

        case EParticlePinType::Float3:
            if (Value.Type == EParticlePinType::Float)
            {
                return "float3(" + Value.Value + ", " + Value.Value + ", " + Value.Value + ")";
            }
            if (Value.Type == EParticlePinType::Float4)
            {
                return "(" + Value.Value + ").xyz";
            }
            break;

        case EParticlePinType::Float4:
            if (Value.Type == EParticlePinType::Float)
            {
                return "float4(" + Value.Value + ", " + Value.Value + ", " + Value.Value + ", " + Value.Value + ")";
            }
            if (Value.Type == EParticlePinType::Float3)
            {
                return "float4(" + Value.Value + ", 1.0)";
            }
            break;
        }
        return Value.Value;
    }

    FString FParticleCompiler::TypeName(EParticlePinType Type)
    {
        switch (Type)
        {
        case EParticlePinType::Float:  return "float";
        case EParticlePinType::Float3: return "float3";
        case EParticlePinType::Float4: return "float4";
        }
        return "float";
    }
}
