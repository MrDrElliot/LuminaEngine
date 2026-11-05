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
        return FString("PAttr().At(Index * PARTICLE_ATTR_FLOATS + ") + *Format("{}", Slot) + "u)[0]";
    }

    // A single pass, so inserted text is not rescanned.
    static void ReplaceAll(FString& Source, const char* Token, const FString& Replacement)
    {
        const size_t TokenLen = strlen(Token);
        int32 Pos = Source.Find(Token);
        while (Pos != Constants::kIndexNone)
        {
            Source.RemoveAt(Pos, IntCastChecked<int32>(TokenLen)).InsertAt(Pos, Replacement);
            Pos = Source.Find(Token, Pos + Replacement.Len());
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
        for (int32 i = 0; i < Attributes.Num(); ++i)
        {
            AttrDefaults += "\t" + AttributeSlotExpr(i) + " = " + Attributes[i].DefaultExpr + ";\n";
        }

        FString GridConstants;
        for (int32 Channel = 0; Channel < GridChannels.Num(); ++Channel)
        {
            GridConstants += *Format("static const uint GRID_{} = {}u;\n", GridChannels[Channel], Channel);
        }

        FString ArrayConstants;
        for (int32 Array = 0; Array < Arrays.Num(); ++Array)
        {
            ArrayConstants += *Format("static const uint ARRAY_{} = {}u;\n", Arrays[Array], Array);
        }

        FString GridStages;
        FString GridCases;
        for (int32 Stage = 0; Stage < GridStageChunks.Num(); ++Stage)
        {
            GridStages += *Format("void GridStage{}(inout float Cell[PARTICLE_GRID_CHANNELS], int2 Coord, float2 UV, float3 WorldPos, float DeltaTime, float TotalTime, uint Iteration)\n{{\n", Stage);
            GridStages += GridStageChunks[Stage] + "}\n\n";
            GridCases  += *Format("    case {}u: GridStage{}(Cell, Coord, UV, WorldPos, DeltaTime, TotalTime, Iteration); break;\n", Stage, Stage);
        }

        ReplaceAll(Source, AttrCountToken,     *Format("{}", GetAttributeFloatCount()));
        ReplaceAll(Source, AttrDefaultsToken,  AttrDefaults);
        ReplaceAll(Source, GridChannelsToken,  *Format("{}u", Math::Max((uint32)GridChannels.Num(), 1u)));
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
            if (GridStageChunks.IsEmpty())
            {
                BeginGridStage(1);
            }
            GridStageChunks.Last() += "\t" + Line + "\n";
            break;
        case EParticleContext::Target:
            TargetChunks += "\t" + Line + "\n";
            break;
        }
    }

    void FParticleCompiler::BeginGridStage(int32 Iterations)
    {
        GridStageChunks.Emplace();
        GridStageIterations.Add(Math::Clamp(Iterations, 0, ParticleStageMaxIterations));
    }

    FString FParticleCompiler::ArrayInput(const FName& Name)
    {
        const FString ArrayName(*Name.ToString());
        if (!CParticleModule::IsIdentifier(ArrayName))
        {
            AddError({ "Array", "Array '" + ArrayName + "' is not a valid identifier." });
            return "0u";
        }

        if (!Algo::Contains(Arrays, ArrayName))
        {
            Arrays.Add(ArrayName);
        }
        return "ARRAY_" + ArrayName;
    }

    FString FParticleCompiler::GridChannel(const FName& Name)
    {
        const FString ChannelName(*Name.ToString());
        if (!CParticleModule::IsIdentifier(ChannelName))
        {
            AddError({ "Grid", "Grid channel '" + ChannelName + "' is not a valid identifier." });
            return "0u";
        }

        if (!Algo::Contains(GridChannels, ChannelName))
        {
            if (GridChannels.Num() >= kParticleGridMaxChannels)
            {
                AddError({ "Grid", *Format("Grid channel '{}' is past the limit of {} channels per cell.", ChannelName, kParticleGridMaxChannels) });
                return "0u";
            }
            GridChannels.Add(ChannelName);
        }
        return "GRID_" + ChannelName;
    }

    FString FParticleCompiler::AddParamSlot(const char* DebugName, const FVector4& Value, uint32 Components)
    {
        const uint32 Slot = (uint32)ParamValues.Num();
        ParamValues.Add(Value);

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
        for (int32 i = 0; i < Attributes.Num(); ++i)
        {
            if (Attributes[i].Name == AttrName)
            {
                Index = i;
                break;
            }
        }

        if (Index == Constants::kIndexNone)
        {
            Index = Attributes.Num();
            FAttributeDecl& Decl = Attributes.EmplaceGetRef();
            Decl.Name        = AttrName;
            Decl.DefaultExpr = DefaultExpr;
        }

        // CONTRACT, both graph functions in the template must take the particle index named Index.
        return AttributeSlotExpr(Index);
    }

    int32 FParticleCompiler::FindAttributeSlot(const char* Name) const
    {
        const FString Wanted(Name);
        for (int32 i = 0; i < Attributes.Num(); ++i)
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
        return Math::Max((uint32)Attributes.Num(), 1u);
    }

    uint64 FParticleCompiler::GetGeneratedCodeHash() const
    {
        uint64 Hash = 0;
        Hash::HashCombine(Hash, FStringView(SpawnChunks.GetData(), SpawnChunks.Len()));
        Hash::HashCombine(Hash, FStringView(UpdateChunks.GetData(), UpdateChunks.Len()));
        for (const FString& Stage : GridStageChunks)
        {
            Hash::HashCombine(Hash, FStringView(Stage.GetData(), Stage.Len()));
        }
        Hash::HashCombine(Hash, GridStageChunks.Num());
        Hash::HashCombine(Hash, FStringView(TargetChunks.GetData(), TargetChunks.Len()));
        for (const FString& Channel : GridChannels)
        {
            Hash::HashCombine(Hash, FStringView(Channel.GetData(), Channel.Len()));
        }
        for (const FString& Array : Arrays)
        {
            Hash::HashCombine(Hash, FStringView(Array.GetData(), Array.Len()));
        }
        Hash::HashCombine(Hash, bNeighborGrid);
        // The attribute block is spliced into the source too, so it keys the artifact cache alongside the stacks.
        for (const FAttributeDecl& Attribute : Attributes)
        {
            Hash::HashCombine(Hash, FStringView(Attribute.Name.GetData(), Attribute.Name.Len()));
            Hash::HashCombine(Hash, FStringView(Attribute.DefaultExpr.GetData(), Attribute.DefaultExpr.Len()));
        }
        return Hash;
    }

    FString FParticleCompiler::Param(const char* DebugName, const SParticleParam& Value)
    {
        // The editor preview simulates with it, and the runtime falls back to it for an unset parameter.
        const int32  Slot = ParamValues.Num();
        const FString Expr = AddParamSlot(DebugName, Value.Constant, ParticleParamComponents(Value.Type));

        if (Value.IsBound())
        {
            SParticleParamBinding& Binding = ParamBindings.EmplaceGetRef();
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

        const uint32 BaseSlot = (uint32)ParamValues.Num();

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
                ParamValues.Add(Packed);
                Packed = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
                break;
            }
        }
        // Flush a partial trailing group so the table is always a whole number of slots.
        if ((kParticleLUTSamples & 3) != 0)
        {
            ParamValues.Add(Packed);
        }

        (void)DebugName;
        return *Format("{}", BaseSlot);
    }

    FString FParticleCompiler::ParamGradient(const char* DebugName, const SGradient& Value)
    {
        float TimeMin = 0.0f, TimeMax = 1.0f;
        Value.GetTimeRange(TimeMin, TimeMax);
        if (TimeMax - TimeMin < 1e-6f)
        {
            TimeMax = TimeMin + 1.0f;
        }

        const uint32 BaseSlot = (uint32)ParamValues.Num();

        // One slot per sample, since a color already fills a float4.
        for (int32 i = 0; i < kParticleLUTSamples; ++i)
        {
            const float Alpha = (float)i / (float)(kParticleLUTSamples - 1);
            ParamValues.Add(Value.Evaluate(TimeMin + (TimeMax - TimeMin) * Alpha));
        }

        (void)DebugName;
        return *Format("{}", BaseSlot);
    }

    void FParticleCompiler::EnsureEmitted(CParticleGraphNode* Node, EParticleContext Context)
    {
        if (Node == nullptr)
        {
            return;
        }

        // Only spawn and update graphs share a compile, so every other context can share the update set.
        THashSet<CParticleGraphNode*>& EmittedSet = (Context == EParticleContext::Spawn) ? EmittedSpawn : EmittedUpdate;
        if (EmittedSet.Contains(Node))
        {
            return;
        }
        EmittedSet.Add(Node);

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
                    Result.Value = Out->ResolvedVar.IsEmpty() ? SourceNode->GetNodeFullName() : Out->ResolvedVar;
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
            case EParticlePinType::Float:  Result.Value = *Format("{}", V.x); break;
            case EParticlePinType::Float2: Result.Value = *Format("float2({}, {})", V.x, V.y); break;
            case EParticlePinType::Float3: Result.Value = *Format("float3({}, {}, {})", V.x, V.y, V.z); break;
            case EParticlePinType::Float4: Result.Value = *Format("float4({}, {}, {}, {})", V.x, V.y, V.z, V.w); break;
            }
        }

        return Result;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat(CParticleInput* Pin, float Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.IsEmpty())
        {
            V.Value = Format("{}", Default);
            V.Type  = EParticlePinType::Float;
        }
        return V;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat3(CParticleInput* Pin, const FVector3& Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.IsEmpty())
        {
            V.Value = "float3(" + Format("{}", Default.x) + ", " + Format("{}", Default.y) + ", " + Format("{}", Default.z) + ")";
            V.Type  = EParticlePinType::Float3;
        }
        return V;
    }

    FParticleInputValue FParticleCompiler::GetInputFloat4(CParticleInput* Pin, const FVector4& Default)
    {
        FParticleInputValue V = GetInputValue(Pin);
        if (V.Value.IsEmpty())
        {
            V.Value = "float4(" + Format("{}", Default.x) + ", " + Format("{}", Default.y) + ", " + Format("{}", Default.z) + ", " + Format("{}", Default.w) + ")";
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
