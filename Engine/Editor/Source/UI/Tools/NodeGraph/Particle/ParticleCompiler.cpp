#include "ParticleCompiler.h"
#include "ParticleGraphNode.h"
#include "Containers/Algorithm.h"
#include "Particles/ParticleModule.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Formula.h"
#include "Core/Object/Cast.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    static constexpr const char* SpawnToken         = "$PARTICLE_SPAWN_FUNC";
    static constexpr const char* UpdateToken        = "$PARTICLE_UPDATE_FUNC";
    static constexpr const char* AttrCountToken     = "$PARTICLE_ATTR_COUNT";
    static constexpr const char* AttrDefaultsToken  = "$PARTICLE_ATTR_DEFAULTS";
    static constexpr const char* GridChannelsToken  = "$PARTICLE_GRID_CHANNELS";
    static constexpr const char* GridConstantsToken = "$PARTICLE_GRID_CONSTANTS";
    static constexpr const char* GridStagesToken    = "$PARTICLE_GRID_STAGE_FUNCS";
    static constexpr const char* GridCasesToken     = "$PARTICLE_GRID_STAGE_CASES";
    static constexpr const char* GridTargetToken    = "$PARTICLE_GRID_OUTPUT_FUNC";
    static constexpr const char* ArrayConstantsToken = "$PARTICLE_ARRAY_CONSTANTS";

    // An lvalue, and in bounds because the buffer holds PARTICLE_ATTR_FLOATS per slot of MaxParticles.
    static FString AttributeSlotExpr(int32 Slot)
    {
        return FString("PAttr().At(Index * PARTICLE_ATTR_FLOATS + ") + Format("{}", Slot).c_str() + "u)[0]";
    }

    // A single pass, so inserted text is not rescanned.
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
        return ExpandTemplate(Move(Source));
    }

    FString FParticleCompiler::ExpandTemplate(FString Source) const
    {
        // Attribute defaults splice ahead of the spawn stack, so a read-only module still sees a value.
        FString AttrDefaults;
        for (int32 i = 0; i < (int32)Attributes.size(); ++i)
        {
            AttrDefaults += "\t" + AttributeSlotExpr(i) + " = " + Attributes[i].DefaultExpr + ";\n";
        }

        FString GridConstants;
        for (int32 Channel = 0; Channel < (int32)GridChannels.size(); ++Channel)
        {
            GridConstants += Format("static const uint GRID_{} = {}u;\n", GridChannels[Channel], Channel).c_str();
        }

        FString ArrayConstants;
        for (int32 Array = 0; Array < (int32)Arrays.size(); ++Array)
        {
            ArrayConstants += Format("static const uint ARRAY_{} = {}u;\n", Arrays[Array], Array).c_str();
        }

        FString GridStages;
        FString GridCases;
        for (int32 Stage = 0; Stage < (int32)GridStageChunks.size(); ++Stage)
        {
            GridStages += Format("void GridStage{}(inout float Cell[PARTICLE_GRID_CHANNELS], int2 Coord, float2 UV, float3 WorldPos, float DeltaTime, float TotalTime, uint Iteration)\n{{\n", Stage).c_str();
            GridStages += GridStageChunks[Stage] + "}\n\n";
            GridCases  += Format("    case {}u: GridStage{}(Cell, Coord, UV, WorldPos, DeltaTime, TotalTime, Iteration); break;\n", Stage, Stage).c_str();
        }

        ReplaceAll(Source, AttrCountToken,     Format("{}", GetAttributeFloatCount()).c_str());
        ReplaceAll(Source, AttrDefaultsToken,  AttrDefaults);
        ReplaceAll(Source, GridChannelsToken,  Format("{}u", Math::Max((uint32)GridChannels.size(), 1u)).c_str());
        ReplaceAll(Source, GridConstantsToken, GridConstants);
        ReplaceAll(Source, ArrayConstantsToken, ArrayConstants);
        ReplaceAll(Source, GridStagesToken,    GridStages);
        ReplaceAll(Source, GridCasesToken,     GridCases);
        ReplaceAll(Source, GridTargetToken,    TargetChunks);
        ReplaceAll(Source, SpawnToken,         SpawnChunks);
        ReplaceAll(Source, UpdateToken,        UpdateChunks);

        return Source;
    }

    void FParticleCompiler::Emit(EParticleContext Context, const FString& Line)
    {
        switch (Context)
        {
        case EParticleContext::Spawn:
            SpawnChunks += "\t" + Line + "\n";
            break;
        case EParticleContext::Update:
            UpdateChunks += "\t" + Line + "\n";
            break;
        case EParticleContext::Grid:
            // Grid modules ahead of any Simulation Stage form a stage of their own.
            if (GridStageChunks.empty())
            {
                BeginGridStage(1);
            }
            GridStageChunks.back() += "\t" + Line + "\n";
            break;
        case EParticleContext::Target:
            TargetChunks += "\t" + Line + "\n";
            break;
        }
    }

    void FParticleCompiler::BeginGridStage(int32 Iterations)
    {
        GridStageChunks.emplace_back();
        GridStageIterations.push_back(Math::Clamp(Iterations, 0, ParticleStageMaxIterations));
    }

    FString FParticleCompiler::ArrayInput(const FName& Name)
    {
        const FString ArrayName(Name.ToString().c_str());
        if (!CParticleModule::IsIdentifier(ArrayName))
        {
            AddError({ "Array", "Array '" + ArrayName + "' is not a valid identifier." });
            return "0u";
        }

        if (!Algo::Contains(Arrays, ArrayName))
        {
            Arrays.push_back(ArrayName);
        }
        return "ARRAY_" + ArrayName;
    }

    FString FParticleCompiler::GridChannel(const FName& Name)
    {
        const FString ChannelName(Name.ToString().c_str());
        if (!CParticleModule::IsIdentifier(ChannelName))
        {
            AddError({ "Grid", "Grid channel '" + ChannelName + "' is not a valid identifier." });
            return "0u";
        }

        if (!Algo::Contains(GridChannels, ChannelName))
        {
            if ((int32)GridChannels.size() >= kParticleGridMaxChannels)
            {
                AddError({ "Grid", Format("Grid channel '{}' is past the limit of {} channels per cell.", ChannelName, kParticleGridMaxChannels).c_str() });
                return "0u";
            }
            GridChannels.push_back(ChannelName);
        }
        return "GRID_" + ChannelName;
    }

    FString FParticleCompiler::AddParamSlot(const char* DebugName, const FVector4& Value, uint32 Components)
    {
        const uint32 Slot = (uint32)ParamValues.size();
        ParamValues.push_back(Value);

        FString Expr = "MP(" + Format("{}", Slot) + ")";
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

        int32 Index = Constants::kIndexNone;
        for (int32 i = 0; i < (int32)Attributes.size(); ++i)
        {
            if (Attributes[i].Name == AttrName)
            {
                Index = i;
                break;
            }
        }

        if (Index == Constants::kIndexNone)
        {
            Index = (int32)Attributes.size();
            FAttributeDecl& Decl = Attributes.emplace_back();
            Decl.Name        = AttrName;
            Decl.DefaultExpr = DefaultExpr;
        }

        // CONTRACT, both graph functions in the template must take the particle index named Index.
        return AttributeSlotExpr(Index);
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
        Hash::HashCombine(Hash, FStringView(SpawnChunks.data(), SpawnChunks.size()));
        Hash::HashCombine(Hash, FStringView(UpdateChunks.data(), UpdateChunks.size()));
        for (const FString& Stage : GridStageChunks)
        {
            Hash::HashCombine(Hash, FStringView(Stage.data(), Stage.size()));
        }
        Hash::HashCombine(Hash, GridStageChunks.size());
        Hash::HashCombine(Hash, FStringView(TargetChunks.data(), TargetChunks.size()));
        for (const FString& Channel : GridChannels)
        {
            Hash::HashCombine(Hash, FStringView(Channel.data(), Channel.size()));
        }
        for (const FString& Array : Arrays)
        {
            Hash::HashCombine(Hash, FStringView(Array.data(), Array.size()));
        }
        Hash::HashCombine(Hash, bNeighborGrid);
        // The attribute block is spliced into the source too, so it keys the artifact cache alongside the stacks.
        for (const FAttributeDecl& Attribute : Attributes)
        {
            Hash::HashCombine(Hash, FStringView(Attribute.Name.data(), Attribute.Name.size()));
            Hash::HashCombine(Hash, FStringView(Attribute.DefaultExpr.data(), Attribute.DefaultExpr.size()));
        }
        return Hash;
    }

    FString FParticleCompiler::Param(const char* DebugName, const SParticleParam& Value)
    {
        // The editor preview simulates with it, and the runtime falls back to it for an unset parameter.
        const int32  Slot = (int32)ParamValues.size();
        const FString Expr = AddParamSlot(DebugName, Value.Constant, ParticleParamComponents(Value.Type));

        if (Value.IsBound())
        {
            SParticleParamBinding& Binding = ParamBindings.emplace_back();
            Binding.ParameterName = Value.ParameterName;
            Binding.SlotIndex     = Slot;
            Binding.Type          = Value.Type;
        }

        return Expr;
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

        // Re-normalizing means a curve keyed over any domain still maps onto LifeRatio.
        float TimeMin = 0.0f, TimeMax = 1.0f;
        Curve.GetTimeRange(TimeMin, TimeMax);
        if (TimeMax - TimeMin < 1e-6f)
        {
            TimeMax = TimeMin + 1.0f;
        }

        const uint32 BaseSlot = (uint32)ParamValues.size();

        // A curve is scalar, so one slot each would waste three quarters of the table.
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
        return Format("{}", BaseSlot).c_str();
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

        // One slot per sample, since a color already fills a float4.
        for (int32 i = 0; i < kParticleLUTSamples; ++i)
        {
            const float Alpha = (float)i / (float)(kParticleLUTSamples - 1);
            ParamValues.push_back(Value.Evaluate(TimeMin + (TimeMax - TimeMin) * Alpha));
        }

        (void)DebugName;
        return Format("{}", BaseSlot).c_str();
    }

    void FParticleCompiler::EnsureEmitted(CParticleGraphNode* Node, EParticleContext Context)
    {
        if (Node == nullptr)
        {
            return;
        }

        // Only spawn and update graphs share a compile, so every other context can share the update set.
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
                    Result.Value = Out->ResolvedVar.empty() ? FString(SourceNode->GetNodeFullName()) : Out->ResolvedVar;
                    Result.Type  = Out->GetPinType();
                    return Result;
                }
            }
        }

        if (Pin != nullptr)
        {
            const FVector4& V = Pin->GetDefault();
            Result.Type = Pin->GetPinType();
            switch (Result.Type)
            {
            case EParticlePinType::Float:  Result.Value = Format("{}", V.x).c_str(); break;
            case EParticlePinType::Float2: Result.Value = Format("float2({}, {})", V.x, V.y).c_str(); break;
            case EParticlePinType::Float3: Result.Value = Format("float3({}, {}, {})", V.x, V.y, V.z).c_str(); break;
            case EParticlePinType::Float4: Result.Value = Format("float4({}, {}, {}, {})", V.x, V.y, V.z, V.w).c_str(); break;
            }
        }

        return Result;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat(CParticleInput* Pin, float Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.empty())
        {
            V.Value = FString(Format("{}", Default));
            V.Type  = EParticlePinType::Float;
        }
        return V;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat3(CParticleInput* Pin, const FVector3& Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.empty())
        {
            V.Value = "float3(" + FString(Format("{}", Default.x)) + ", " + FString(Format("{}", Default.y)) + ", " + FString(Format("{}", Default.z)) + ")";
            V.Type  = EParticlePinType::Float3;
        }
        return V;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat4(CParticleInput* Pin, const FVector4& Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.empty())
        {
            V.Value = "float4(" + FString(Format("{}", Default.x)) + ", " + FString(Format("{}", Default.y)) + ", " + FString(Format("{}", Default.z)) + ", " + FString(Format("{}", Default.w)) + ")";
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

        // A float3 widened to float4 is a color or position, so its fourth component is one rather than zero.
        if (Value.Type == EParticlePinType::Float3 && Target == EParticlePinType::Float4)
        {
            return "float4(" + Value.Value + ", 1.0)";
        }
        return ReadFormulaInput(Value.Value, Width(Value.Type), Width(Target));
    }

    FString FParticleCompiler::TypeName(EParticlePinType Type)
    {
        return FormulaVectorType(Width(Type));
    }

    int32 FParticleCompiler::Width(EParticlePinType Type)
    {
        return (int32)Type + 1;
    }

    EParticlePinType FParticleCompiler::TypeOfWidth(int32 Width)
    {
        return (EParticlePinType)(Math::Clamp(Width, 1, 4) - 1);
    }
}
