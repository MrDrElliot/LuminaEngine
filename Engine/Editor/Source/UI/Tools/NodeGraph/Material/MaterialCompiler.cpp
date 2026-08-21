#include <iterator>
#include "MaterialCompiler.h"

#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Nodes/MaterialNodes.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNode_Reroute.h"
#include "Log/Log.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
	// The one translation unit seeing both definitions, so a reorder gets caught here.
	static_assert((uint8)EMaterialValueType::Float         == MaterialValueOrdinal::Float);
	static_assert((uint8)EMaterialValueType::Float2        == MaterialValueOrdinal::Float2);
	static_assert((uint8)EMaterialValueType::Float3        == MaterialValueOrdinal::Float3);
	static_assert((uint8)EMaterialValueType::Float4        == MaterialValueOrdinal::Float4);
	static_assert((uint8)EMaterialValueType::TextureHandle == MaterialValueOrdinal::TextureHandle);

	FMaterialCompiler::FMaterialCompiler()
	{
		PixelChunks.reserve(2000);
		VertexChunks.reserve(512);
		PixelOutputChunks.reserve(512);
		VertexOutputChunks.reserve(128);
	}

	// Pixel-only references stub to neutral values, which WPO-sourced nodes never reach.
	static const char* GVertexStageAliasPreamble =
		"\t// Material graph variable aliases (vertex stage).\n"
		"\tfloat3 WorldPosition = WorldPos.xyz;\n"
		"\tfloat3 WorldNormal   = NormalWS;\n"
		// The same name and float4 shape the pixel templates declare, so a node emits identical code.
		"\tfloat4 WorldTangent  = float4(TangentWS, TangentSignWS);\n"
		"\tfloat2 UV0           = VertexData.UV;\n"
		"\tfloat4 VertexColor   = VertexData.Color;\n"
		"\tuint   MaterialIndex = Inst.MaterialIndex;\n"
		"\tuint   EntityID      = Inst.EntityID;\n"
		"\tfloat3 ViewPosition  = float3(0.0);\n";

	// The terrain vertex pass lacks the shared structs, and matching alias names keep node code reusable.
	static const char* GVertexStageAliasPreambleTerrain =
		"\t// Material graph variable aliases (vertex stage, terrain).\n"
		"\tfloat3 WorldPosition = WorldPos;\n"
		"\tfloat3 WorldNormal   = NormalWS;\n"
		// Terrain UV is the world XZ plane, so world +X is the U direction on flat ground.
		"\tfloat4 WorldTangent  = float4(1.0, 0.0, 0.0, 1.0);\n"
		"\tfloat2 UV0           = HeightUV;\n"
		"\tfloat4 VertexColor   = float4(1.0, 1.0, 1.0, 1.0);\n"
		"\tuint   MaterialIndex = TerrainParams.MaterialIndex;\n"
		"\tuint   EntityID      = TerrainParams.EntityID;\n"
		"\tfloat3 ViewPosition  = float3(0.0);\n";

	// Substitute a single token; logs and returns false if the token is missing.
	static bool SubstituteToken(FString& Source, const char* Token, const FString& Replacement)
	{
		size_t Pos = Source.find(Token);
		if (Pos == FString::npos)
		{
			LOG_ERROR("Missing [{}] in base shader!", Token);
			return false;
		}
		Source.replace(Pos, strlen(Token), Replacement);
		return true;
	}

	FString FMaterialCompiler::BuildTree(size_t& StartReplacement, size_t& EndReplacement, EMaterialType MaterialType) const
	{
		const FString BasePath = Paths::GetEngineResourceDirectory() + "/Shaders/MaterialShader/";
		const FString FragmentPath = (MaterialType == EMaterialType::Terrain)
			? BasePath + "TerrainBasePixelPass.slang"
			: BasePath + "BasePixelPass.slang";

		FString LoadedString;
		if (!FileHelper::LoadFileIntoString(LoadedString, FragmentPath))
		{
			LOG_ERROR("Failed to find {}!", FragmentPath);
			return LoadedString;
		}

		const char* Token = "$MATERIAL_INPUTS";
		size_t Pos = LoadedString.find(Token);

		FString Combined = PixelChunks + PixelOutputChunks;

		if (Pos != FString::npos)
		{
			StartReplacement = Pos;
			LoadedString.replace(Pos, strlen(Token), Combined);
			EndReplacement = Pos + Combined.length();
		}
		else
		{
			LOG_ERROR("Missing [$MATERIAL_INPUTS] in base shader!");
			return LoadedString;
		}

		return LoadedString;
	}

	void FMaterialCompiler::BuildShaders(FString& OutPixelShader, FString& OutVertexShader, EMaterialType MaterialType) const
	{
		const FString BasePath = Paths::GetEngineResourceDirectory() + "/Shaders/MaterialShader/";
		const bool bIsTerrain     = (MaterialType == EMaterialType::Terrain);
		const bool bIsPostProcess = (MaterialType == EMaterialType::PostProcess);
		const bool bIsUI          = (MaterialType == EMaterialType::UI);
		const bool bIsDecal       = (MaterialType == EMaterialType::Decal);
		const FString PixelPath  = bIsPostProcess ? (BasePath + "PostProcessPixelPass.slang")
		                                          : (bIsUI ? BasePath + "UIPixelPass.slang"
		                                                   : (bIsDecal ? BasePath + "DecalPixelPass.slang"
		                                                   : (bIsTerrain ? BasePath + "TerrainBasePixelPass.slang"
		                                                                 : BasePath + "BasePixelPass.slang")));

		// The output node declares the pixel input struct, so only the body and assignments are appended.
		OutPixelShader.clear();
		if (FileHelper::LoadFileIntoString(OutPixelShader, PixelPath))
		{
			SubstituteToken(OutPixelShader, "$MATERIAL_INPUTS", PixelChunks + PixelOutputChunks);
		}
		else
		{
			LOG_ERROR("Failed to find {}!", PixelPath);
		}

		// PostProcess/UI use a fullscreen quad vertex stage with no $MATERIAL_VERTEX_INPUTS substitution; WPO is meaningless here.
		if (bIsPostProcess || bIsUI)
		{
			OutVertexShader.clear();
			const FString FullscreenQuadPath = Paths::GetEngineResourceDirectory() + "/Shaders/FullscreenQuad.slang";
			if (!FileHelper::LoadFileIntoString(OutVertexShader, FullscreenQuadPath))
			{
				LOG_ERROR("Failed to find {}!", FullscreenQuadPath);
			}
			return;
		}

		// A decal uses a fixed unit-cube vertex stage and projects onto scene depth, so it has no WPO.
		if (bIsDecal)
		{
			OutVertexShader.clear();
			const FString DecalVertexPath = BasePath + "DecalVertexPass.slang";
			if (!FileHelper::LoadFileIntoString(OutVertexShader, DecalVertexPath))
			{
				LOG_ERROR("Failed to find {}!", DecalVertexPath);
			}
			return;
		}

		// Terrain is the only domain still rastering meshlet geometry through a vertex stage.
		if (bIsTerrain)
		{
			OutVertexShader = BuildVertexShaderFromTemplate(BasePath + "TerrainBaseVertexPass.slang", MaterialType);
		}
	}

	// The stub goes first, so an unconnected WPO pin never reaches an unwritten struct.
	FString FMaterialCompiler::BuildVertexStageBody(EMaterialType MaterialType) const
	{
		static const char* kWPOStub = "\tMaterial.WorldPositionOffset = float3(0.0, 0.0, 0.0);\n";

		if (!UsesVertexStage())
		{
			return FString(kWPOStub);
		}

		const char* Preamble = (MaterialType == EMaterialType::Terrain)
			? GVertexStageAliasPreambleTerrain
			: GVertexStageAliasPreamble;
		return FString(kWPOStub) + Preamble + VertexChunks + VertexOutputChunks;
	}

	FString FMaterialCompiler::BuildVertexShaderFromTemplate(const FString& TemplateAbsolutePath, EMaterialType MaterialType) const
	{
		FString Loaded;
		if (!FileHelper::LoadFileIntoString(Loaded, TemplateAbsolutePath))
		{
			LOG_ERROR("Failed to find {}!", TemplateAbsolutePath);
			return Loaded;
		}

		SubstituteToken(Loaded, "$MATERIAL_VERTEX_INPUTS", BuildVertexStageBody(MaterialType));
		return Loaded;
	}

	FString FMaterialCompiler::BuildDeferredShaderFromTemplate(const FString& TemplateAbsolutePath, EMaterialType MaterialType) const
	{
		FString Loaded;
		if (!FileHelper::LoadFileIntoString(Loaded, TemplateAbsolutePath))
		{
			LOG_ERROR("Failed to find {}!", TemplateAbsolutePath);
			return Loaded;
		}

		// Vertex graph (WPO) for the geometry reconstruction loop.
		SubstituteToken(Loaded, "$MATERIAL_VERTEX_INPUTS", BuildVertexStageBody(MaterialType));

		// Pixel graph (shading). The output node declares FMaterialPixelInputs Material; we append body + assignments.
		SubstituteToken(Loaded, "$MATERIAL_INPUTS", PixelChunks + PixelOutputChunks);
		return Loaded;
	}

	FString FMaterialCompiler::BuildPixelShaderFromTemplate(const FString& TemplateAbsolutePath) const
	{
		FString Loaded;
		if (!FileHelper::LoadFileIntoString(Loaded, TemplateAbsolutePath))
		{
			LOG_ERROR("Failed to find {}!", TemplateAbsolutePath);
			return Loaded;
		}

		// Pixel template declares FMaterialPixelInputs Material above the token; append body + assignments only.
		SubstituteToken(Loaded, "$MATERIAL_INPUTS", PixelChunks + PixelOutputChunks);
		return Loaded;
	}

	static FString GetVectorType(EMaterialInputType Type)
	{
		switch (Type)
		{
			case EMaterialInputType::Float:		return "float";
			case EMaterialInputType::Float2:	return "float2";
			case EMaterialInputType::Float3:	return "float3";
			case EMaterialInputType::Float4:	return "float4";
			case EMaterialInputType::Texture: return "float4";
			// A bindless index rather than a sampled color, the one non-float type the graph can carry.
			case EMaterialInputType::TextureHandle: return "uint";
			default: return "float";
		}
	}

	static FString GetVectorType(int32 ComponentCount)
	{
		switch (ComponentCount)
		{
			case 1: return "float";
			case 2: return "float2";
			case 3: return "float3";
			case 4: return "float4";
			default: return "float";
		}
	}

	static EComponentMask GetMaskFromComponentCount(int32 Count)
	{
		switch (Count)
		{
			case 1: return EComponentMask::R;
			case 2: return EComponentMask::RG;
			case 3: return EComponentMask::RGB;
			case 4: return EComponentMask::RGBA;
			default: return EComponentMask::None;
		}
	}

	int32 FMaterialCompiler::GetComponentCount(EComponentMask Mask)
	{
		switch (Mask)
		{
			case EComponentMask::None: return 0;
			case EComponentMask::RGBA: return 4;
			case EComponentMask::R: return 1;
			case EComponentMask::G: return 1;
			case EComponentMask::B: return 1;
			case EComponentMask::A: return 1;
			case EComponentMask::RG: return 2;
			case EComponentMask::GB: return 2;
			case EComponentMask::RGB: return 3;
		}

		return 0;
	}

	int32 FMaterialCompiler::GetComponentCount(EMaterialInputType Type)
	{
		switch (Type)
		{
			case EMaterialInputType::Float:		return 1;
			case EMaterialInputType::Float2:	return 2;
			case EMaterialInputType::Float3:	return 3;
			case EMaterialInputType::Float4:	return 4;
			case EMaterialInputType::Texture:	return 4;
			case EMaterialInputType::TextureHandle:	return 1;
			default: return 1;
		}
	}

	FString FMaterialCompiler::GetHLSLTypeName(EMaterialInputType Type)
	{
		return GetVectorType(Type);
	}

	const FString& FMaterialCompiler::GetCurrentInlinePrefix() const
	{
		static const FString Empty;
		return InlinePrefixStack.empty() ? Empty : InlinePrefixStack.back();
	}

	bool FMaterialCompiler::BeginInlineFunction(CMaterialFunction* Function)
	{
		for (CMaterialFunction* Active : InlineFunctionStack)
		{
			if (Active == Function)
			{
				return false;
			}
		}
		InlineFunctionStack.push_back(Function);
		return true;
	}

	void FMaterialCompiler::EndInlineFunction(CMaterialFunction* Function)
	{
		if (!InlineFunctionStack.empty())
		{
			InlineFunctionStack.pop_back();
		}
	}

	static EMaterialInputType GetTypeFromComponentCount(int32 Count)
	{
		switch (Count)
		{
		case 1: return EMaterialInputType::Float;
		case 2: return EMaterialInputType::Float2;
		case 3: return EMaterialInputType::Float3;
		case 4: return EMaterialInputType::Float4;
		default: return EMaterialInputType::Float;
		}
	}

	FMaterialCompiler::FInputValue FMaterialCompiler::GetTypedInputValue(CMaterialInput* Input, float DefaultValue)
	{
		return GetTypedInputValue(Input, Format("{}", DefaultValue));
	}

	CMaterialOutput* FMaterialCompiler::ResolveThroughReroutes(CMaterialOutput* OutputPin)
	{
		// Cap the walk so a malformed/cyclic graph can't hang the compiler.
		constexpr int MaxHops = 64;
		int Hops = 0;
		while (OutputPin != nullptr && Hops++ < MaxHops)
		{
			CEdGraphNode* Owner = OutputPin->GetOwningNode();
			if (Owner == nullptr || !Owner->IsRerouteNode())
			{
				return OutputPin;
			}

			// A plain reroute chases its own input pin, while a named one chases its declaration's input.
			CEdNodeGraphPin* RerouteInput = Owner->GetRerouteSourcePin();
			if (RerouteInput == nullptr || !RerouteInput->HasConnection())
			{
				return nullptr;
			}

			OutputPin = static_cast<CMaterialOutput*>(RerouteInput->GetConnection(0));
		}
		return nullptr;
	}

	FMaterialCompiler::FInputValue FMaterialCompiler::GetTypedInputValue(CMaterialInput* Input, const FString& DefaultValueStr)
	{
		FInputValue Result;

		if (Input->HasConnection())
		{
			CMaterialOutput* Conn	= Input->GetConnection<CMaterialOutput>(0);
			Conn					= ResolveThroughReroutes(Conn);

			if (Conn == nullptr)
			{
				FString NodeName		= Input->GetOwningNode()->GetNodeFullName();
				Result.Type				= GetTypeFromComponentCount(GetComponentCount(Input->GetComponentMask()));
				Result.ComponentCount	= GetComponentCount(Input->GetComponentMask());
				Result.Value 			= DefaultValueStr;
				Result.Mask  			= Input->GetComponentMask();
				return Result;
			}

			// A function-call output binds its own emitted local, while everything else uses the node name.
			FString NodeName		= Conn->ResolvedVar.empty() ? Conn->GetOwningNode()->GetNodeFullName() : Conn->ResolvedVar;

			Result.Type				= Conn->InputType;
			Result.ComponentCount	= GetComponentCount(Result.Type);
			Result.Value 			= NodeName;
			Result.Mask  			= Conn->GetComponentMask();

			// Absent means Unknown, or a node with no rule silently samples the wrong mip.
			auto It = DerivByVar.find(Result.Value);
			if (It != DerivByVar.end())
			{
				Result.Deriv = It->second.State;
				Result.DDX   = It->second.DDX;
				Result.DDY   = It->second.DDY;
			}
			else
			{
				Result.Deriv = EDerivState::Unknown;
			}
		}
		else
		{
			FString NodeName		= Input->GetOwningNode()->GetNodeFullName();

			Result.Type				= GetTypeFromComponentCount(GetComponentCount(Input->GetComponentMask()));
			Result.ComponentCount	= GetComponentCount(Input->GetComponentMask());
			Result.Value 			= DefaultValueStr;
			Result.Mask  			= Input->GetComponentMask();
		}

		return Result;
	}

	void FMaterialCompiler::EmitDedupedParamFetch(const FString& TypeStr, const FString& NodeID, const FString& FetchExpr)
	{
		auto It = EmittedParamFetches.find(FetchExpr);
		if (It != EmittedParamFetches.end())
		{
			// Downstream nodes address it by name, so the variable still has to exist and just aliases.
			GetActiveChunk().append(TypeStr + " " + NodeID + " = " + It->second + ";\n");
		}
		else
		{
			GetActiveChunk().append(TypeStr + " " + NodeID + " = " + FetchExpr + ";\n");
			EmittedParamFetches[FetchExpr] = NodeID;
		}

		// Material parameters are uniform, so a parameter-driven tiling value keeps a valid UV gradient.
		RegisterDeriv(NodeID, EDerivState::Zero);
	}

	void FMaterialCompiler::RegisterDeriv(const FString& ID, EDerivState State,
	                                      const FString& DdxExpr, const FString& DdyExpr,
	                                      int32 ComponentCount)
	{
		FDerivInfo Info;
		Info.State = State;

		// A vertex shader has no quad to differentiate against, so record the state and emit nothing.
		if (Info.State == EDerivState::Valid && CurrentStage == EMaterialCompileStage::Vertex)
		{
			Info.State = EDerivState::Unknown;
		}

		if (Info.State == EDerivState::Valid)
		{
			// Named off the value so a consumer finds them from FInputValue alone, and they dead-strip.
			Info.DDX = ID + "_DDX";
			Info.DDY = ID + "_DDY";

			const FString Type = GetVectorType(Math::Clamp(ComponentCount, 1, 4));
			GetActiveChunk().append(Type + " " + Info.DDX + " = " + DdxExpr + ";\n");
			GetActiveChunk().append(Type + " " + Info.DDY + " = " + DdyExpr + ";\n");
		}

		DerivByVar[ID] = Info;
	}

	void FMaterialCompiler::RegisterScaledDeriv(const FString& ID, const FInputValue& Source, const FString& ScaleExpr)
	{
		switch (Source.Deriv)
		{
		case EDerivState::Valid:
		{
			// The companion takes the same mask the value does, or the two describe different channels.
			const FString Mask = GetSwizzleForMask(Source.Mask);
			RegisterDeriv(ID, EDerivState::Valid,
			              Source.DDX + Mask + " * " + ScaleExpr,
			              Source.DDY + Mask + " * " + ScaleExpr,
			              Source.ComponentCount);
			break;
		}

		case EDerivState::Zero:
			// A constant UV scaled by anything is still constant.
			RegisterDeriv(ID, EDerivState::Zero);
			break;

		default:
			RegisterDeriv(ID, EDerivState::Unknown);
			break;
		}
	}

	void FMaterialCompiler::GetUVGradients(const FInputValue& UV, FString& OutDdx, FString& OutDdy) const
	{
		if (UV.Deriv == EDerivState::Valid)
		{
			// Shaped exactly like the UV expression TextureSample builds from the same FInputValue.
			if (UV.ComponentCount >= 2)
			{
				OutDdx = UV.DDX + ".xy";
				OutDdy = UV.DDY + ".xy";
			}
			else
			{
				OutDdx = "float2(" + UV.DDX + ")";
				OutDdy = "float2(" + UV.DDY + ")";
			}
			return;
		}

		// Zero would mean one texel per pixel, so UV0's gradient is the honest approximation.
		++UVGradientFallbackCount;
		OutDdx = "UV0_DDX";
		OutDdy = "UV0_DDY";
	}

	void FMaterialCompiler::RegisterBinaryDeriv(const FString& ID, const FString& Op,
	                                            const FInputValue& A, const FString& AExpr,
	                                            const FInputValue& B, const FString& BExpr,
	                                            EMaterialInputType ResultType)
	{
		// Shapes must broadcast the way HLSL does, so the companions mirror the value term for term.
		const int32 ResultComponents = GetComponentCount(ResultType);
		const bool bShapeOK = ResultComponents >= 1 && ResultComponents <= 4
		                   && (A.ComponentCount == B.ComponentCount
		                    || A.ComponentCount == 1 || B.ComponentCount == 1);

		if (!bShapeOK || A.Deriv == EDerivState::Unknown || B.Deriv == EDerivState::Unknown)
		{
			RegisterDeriv(ID, EDerivState::Unknown);
			return;
		}

		// The companions hold each operand's FULL width, so mask them identically to the value.
		const FString ADdx = A.DDX + GetSwizzleForMask(A.Mask);
		const FString ADdy = A.DDY + GetSwizzleForMask(A.Mask);
		const FString BDdx = B.DDX + GetSwizzleForMask(B.Mask);
		const FString BDdy = B.DDY + GetSwizzleForMask(B.Mask);

		const bool bAZero = A.Deriv == EDerivState::Zero;
		const bool bBZero = B.Deriv == EDerivState::Zero;

		if (bAZero && bBZero)
		{
			RegisterDeriv(ID, EDerivState::Zero);
			return;
		}

		FString Ddx, Ddy;

		if (Op == "+" || Op == "-")
		{
			// d(A +- B) = dA +- dB. A zero side just drops out (negated for the B side of a subtract).
			if (bAZero)      { Ddx = Op + BDdx;                       Ddy = Op + BDdy; }
			else if (bBZero) { Ddx = ADdx;                            Ddy = ADdy; }
			else             { Ddx = "(" + ADdx + " " + Op + " " + BDdx + ")";
			                   Ddy = "(" + ADdy + " " + Op + " " + BDdy + ")"; }
		}
		else if (Op == "*")
		{
			// Product rule. Scalar operands broadcast against the wider companion on their own.
			if (bBZero)      { Ddx = "(" + ADdx + " * " + BExpr + ")";
			                   Ddy = "(" + ADdy + " * " + BExpr + ")"; }
			else if (bAZero) { Ddx = "(" + AExpr + " * " + BDdx + ")";
			                   Ddy = "(" + AExpr + " * " + BDdy + ")"; }
			else             { Ddx = "(" + ADdx + " * " + BExpr + " + " + AExpr + " * " + BDdx + ")";
			                   Ddy = "(" + ADdy + " * " + BExpr + " + " + AExpr + " * " + BDdy + ")"; }
		}
		else if (Op == "/")
		{
			// Quotient rule; a constant divisor collapses it to a scale.
			if (bBZero)      { Ddx = "(" + ADdx + " / " + BExpr + ")";
			                   Ddy = "(" + ADdy + " / " + BExpr + ")"; }
			else             { Ddx = "((" + ADdx + " * " + BExpr + " - " + AExpr + " * " + BDdx + ") / (" + BExpr + " * " + BExpr + "))";
			                   Ddy = "((" + ADdy + " * " + BExpr + " - " + AExpr + " * " + BDdy + ") / (" + BExpr + " * " + BExpr + "))"; }
		}
		else
		{
			RegisterDeriv(ID, EDerivState::Unknown);
			return;
		}

		RegisterDeriv(ID, EDerivState::Valid, Ddx, Ddy, ResultComponents);
	}

	void FMaterialCompiler::SetOwningOutputType(CMaterialInput* AnyInputOnNode, EMaterialInputType Type)
	{
		if (!AnyInputOnNode)
		{
			return;
		}

		CMaterialExpression* Owner = AnyInputOnNode->GetOwningNode<CMaterialExpression>();
		if (Owner && Owner->Output)
		{
			Owner->Output->SetInputType(Type);
			Owner->Output->SetComponentMask(GetMaskFromComponentCount(GetComponentCount(Type)));
		}
	}

	EMaterialInputType FMaterialCompiler::DetermineResultType(EMaterialInputType A, EMaterialInputType B, bool IsComponentWise)
	{
		int32 CountA = GetComponentCount(A);
		int32 CountB = GetComponentCount(B);

		if (IsComponentWise)
		{
			if (CountA == 1)
			{
				return B;
			}
			if (CountB == 1)
			{
				return A;
			}

			return CountA >= CountB ? A : B;
		}
		else
		{
			return EMaterialInputType::Float;
		}
	}

	EMaterialInputType FMaterialCompiler::EmitBinaryOp(const FString& Op, CMaterialInput* A, CMaterialInput* B, float DefaultA, float DefaultB, bool IsComponentWise)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();

		FInputValue AValue = GetTypedInputValue(A, DefaultA);
		FInputValue BValue = GetTypedInputValue(B, DefaultB);

		EMaterialInputType ResultType = DetermineResultType(AValue.Type, BValue.Type, IsComponentWise);
		FString ResultTypeStr = GetVectorType(ResultType);

		if (AValue.ComponentCount > 1 && BValue.ComponentCount > 1 && AValue.ComponentCount != BValue.ComponentCount)
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = "Cannot perform " + Op + " between " + GetVectorType(AValue.Type) + " and " + GetVectorType(BValue.Type);
			AddError(Error);

			GetActiveChunk().append("// ERROR: Type mismatch\n");
		}

		FString RMask = GetSwizzleForMask(AValue.Mask);
		FString GMask = GetSwizzleForMask(BValue.Mask);

		GetActiveChunk().append(ResultTypeStr + " " + OwningNode + " = " + AValue.Value + RMask + " " + Op + " " + BValue.Value + GMask + ";\n");

		RegisterBinaryDeriv(OwningNode, Op, AValue, AValue.Value + RMask, BValue, BValue.Value + GMask, ResultType);

		// Stamp both type and mask; leaving Mask=None causes downstream consumers to see component count 0 and emit a broken float3() wrap for float4 inputs.
		SetOwningOutputType(A, ResultType);
		return ResultType;
	}

	EMaterialInputType FMaterialCompiler::EmitUnaryFunc(const FString& Func, CMaterialInput* A, float DefaultA)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, DefaultA);
		FString TypeStr = GetVectorType(AValue.Type);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + Func + "(" + AValue.Value + GetSwizzleForMask(AValue.Mask) + ");\n");

		// Anything else stays Unknown and falls back rather than claiming a gradient it cannot justify.
		RegisterDeriv(OwningNode, AValue.Deriv == EDerivState::Zero
		                        ? EDerivState::Zero : EDerivState::Unknown);

		SetOwningOutputType(A, AValue.Type);
		return AValue.Type;
	}

	EMaterialInputType FMaterialCompiler::EmitBinaryFunc(const FString& Func, CMaterialInput* A, CMaterialInput* B, float DefaultA, float DefaultB)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, DefaultA);
		FInputValue BValue = GetTypedInputValue(B, DefaultB);

		EMaterialInputType ResultType = DetermineResultType(AValue.Type, BValue.Type, true);
		FString TypeStr = GetVectorType(ResultType);

		if (AValue.ComponentCount > 1 && BValue.ComponentCount > 1 && AValue.ComponentCount != BValue.ComponentCount)
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = "Cannot perform " + Func + " between " + GetVectorType(AValue.Type) + " and " + GetVectorType(BValue.Type);
			AddError(Error);
			GetActiveChunk().append("// ERROR: Type mismatch\n");
		}

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + Func + "(" + AValue.Value + GetSwizzleForMask(AValue.Mask) + ", " + BValue.Value + GetSwizzleForMask(BValue.Mask) + ");\n");

		SetOwningOutputType(A, ResultType);
		return ResultType;
	}

	EMaterialInputType FMaterialCompiler::EmitTernaryFunc(const FString& Func, CMaterialInput* A, CMaterialInput* B, CMaterialInput* C, float DA, float DB, float DC)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, DA);
		FInputValue BValue = GetTypedInputValue(B, DB);
		FInputValue CValue = GetTypedInputValue(C, DC);

		EMaterialInputType ResultType = DetermineResultType(AValue.Type, BValue.Type, true);
		ResultType = DetermineResultType(ResultType, CValue.Type, true);
		FString TypeStr = GetVectorType(ResultType);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + Func + "(" + AValue.Value + ", " + BValue.Value + ", " + CValue.Value + ");\n");

		// Otherwise Unknown until the specific function earns a rule of its own.
		RegisterDeriv(OwningNode, (AValue.Deriv == EDerivState::Zero && BValue.Deriv == EDerivState::Zero
		                        && CValue.Deriv == EDerivState::Zero) ? EDerivState::Zero : EDerivState::Unknown);

		SetOwningOutputType(A, ResultType);
		return ResultType;
	}

	void FMaterialCompiler::DefineFloatParameter(const FString& NodeID, const FName& ParamID, float Value)
	{
		if (ScalarParameters.find(ParamID) == ScalarParameters.end())
		{
			ScalarParameters[ParamID].Index = NumScalarParams++;
			ScalarParameters[ParamID].Value = Value;
		}

		FString IndexString = Format("{}", ScalarParameters[ParamID].Index);
		EmitDedupedParamFetch("float", NodeID, "GetMaterialScalar(MaterialIndex, " + IndexString + ")");
	}

	void FMaterialCompiler::DefineFloat2Parameter(const FString& NodeID, const FName& ParamID, float Value[2])
	{
		if (VectorParameters.find(ParamID) == VectorParameters.end())
		{
			VectorParameters[ParamID].Index = NumVectorParams++;
			VectorParameters[ParamID].Value = FVector4(Value[0], Value[1], 0.0f, 1.0f);
		}

		FString IndexString = Format("{}", VectorParameters[ParamID].Index);
		EmitDedupedParamFetch("float2", NodeID, "GetMaterialVec4(MaterialIndex, " + IndexString + ").xy");
	}

	void FMaterialCompiler::DefineFloat3Parameter(const FString& NodeID, const FName& ParamID, float Value[3])
	{
		if (VectorParameters.find(ParamID) == VectorParameters.end())
		{
			VectorParameters[ParamID].Index = NumVectorParams++;
			VectorParameters[ParamID].Value = FVector4(Value[0], Value[1], Value[2], 1.0f);
		}

		FString IndexString = Format("{}", VectorParameters[ParamID].Index);
		EmitDedupedParamFetch("float3", NodeID, "GetMaterialVec4(MaterialIndex, " + IndexString + ").xyz");
	}

	void FMaterialCompiler::DefineFloat4Parameter(const FString& NodeID, const FName& ParamID, float Value[4])
	{
		if (VectorParameters.find(ParamID) == VectorParameters.end())
		{
			VectorParameters[ParamID].Index = NumVectorParams++;
			VectorParameters[ParamID].Value = FVector4(Value[0], Value[1], Value[2], Value[3]);
		}

		FString IndexString = Format("{}", VectorParameters[ParamID].Index);
		EmitDedupedParamFetch("float4", NodeID, "GetMaterialVec4(MaterialIndex, " + IndexString + ")");
	}

	void FMaterialCompiler::DefineConstantFloat(const FString& ID, float Value)
	{
		FString ValueString = Format("{}", Value);
		GetActiveChunk().append("float " + ID + " = " + ValueString + ";\n");
	}

	void FMaterialCompiler::DefineConstantFloat2(const FString& ID, float Value[2])
	{
		FString ValueStringX = Format("{}", Value[0]);
		FString ValueStringY = Format("{}", Value[1]);
		GetActiveChunk().append("float2 " + ID + " = float2(" + ValueStringX + ", " + ValueStringY + ");\n");
	}

	void FMaterialCompiler::DefineConstantFloat3(const FString& ID, float Value[3])
	{
		FString ValueStringX = Format("{}", Value[0]);
		FString ValueStringY = Format("{}", Value[1]);
		FString ValueStringZ = Format("{}", Value[2]);
		GetActiveChunk().append("float3 " + ID + " = float3(" + ValueStringX + ", " + ValueStringY + ", " + ValueStringZ + ");\n");
	}

	void FMaterialCompiler::DefineConstantFloat4(const FString& ID, float Value[4])
	{
		FString ValueStringX = Format("{}", Value[0]);
		FString ValueStringY = Format("{}", Value[1]);
		FString ValueStringZ = Format("{}", Value[2]);
		FString ValueStringW = Format("{}", Value[3]);
		GetActiveChunk().append("float4 " + ID + " = float4(" + ValueStringX + ", " + ValueStringY + ", " + ValueStringZ + ", " + ValueStringW + ");\n");
	}

	void FMaterialCompiler::BreakFloat2(CMaterialInput* A)
	{
		const FString OwningNode = A->GetOwningNode()->GetNodeFullName();

		FInputValue ValueString = GetTypedInputValue(A, 0.0);

		const FString TypeStr = GetVectorType(EMaterialInputType::Float2);

		if (ValueString.ComponentCount != 2)
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = "BreakFloat2 requires a Float2 input, got " + GetVectorType(ValueString.Type);
			AddError(Error);
			GetActiveChunk().append("// ERROR: Type mismatch in BreakFloat2\n");
		}

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + ValueString.Value + ".xy" + ";\n");
		// BreakFloatN is distinct from ComponentMask and was the last unruled link in the tiling chain.
		RegisterDeriv(OwningNode, ValueString.Deriv == EDerivState::Zero
		                       ? EDerivState::Zero : EDerivState::Unknown);
	}

	void FMaterialCompiler::BreakFloat3(CMaterialInput* A)
	{
		const FString OwningNode = A->GetOwningNode()->GetNodeFullName();

		FInputValue ValueString = GetTypedInputValue(A, 0.0);
		const FString TypeStr = GetVectorType(EMaterialInputType::Float3);

		if (ValueString.ComponentCount != 3)
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = "BreakFloat3 requires a Float3 input, got " + GetVectorType(ValueString.Type);
			AddError(Error);
			GetActiveChunk().append("// ERROR: Type mismatch in BreakFloat3\n");
		}

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + ValueString.Value + ".xyz" + ";\n");
		// BreakFloatN is distinct from ComponentMask and was the last unruled link in the tiling chain.
		RegisterDeriv(OwningNode, ValueString.Deriv == EDerivState::Zero
		                       ? EDerivState::Zero : EDerivState::Unknown);
	}

	void FMaterialCompiler::BreakFloat4(CMaterialInput* A)
	{
		const FString OwningNode = A->GetOwningNode()->GetNodeFullName();

		FInputValue ValueString = GetTypedInputValue(A, 0.0);

		const FString TypeStr = GetVectorType(EMaterialInputType::Float4);

		if (ValueString.ComponentCount != 4)
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = "BreakFloat4 requires a Float4 input, got " + GetVectorType(ValueString.Type);
			AddError(Error);
			GetActiveChunk().append("// ERROR: Type mismatch in BreakFloat4\n");
		}

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + ValueString.Value + ".xyzw" + ";\n");
		// BreakFloatN is distinct from ComponentMask and was the last unruled link in the tiling chain.
		RegisterDeriv(OwningNode, ValueString.Deriv == EDerivState::Zero
		                       ? EDerivState::Zero : EDerivState::Unknown);
	}

	void FMaterialCompiler::MakeFloat2(CMaterialInput* R, CMaterialInput* G)
	{
		const FString OwningNode = R->GetOwningNode()->GetNodeFullName();
		FInputValue ValueR = GetTypedInputValue(R, 0.0f);
		FInputValue ValueG = GetTypedInputValue(G, 0.0f);

		const FString TypeStr = GetVectorType(EMaterialInputType::Float2);

		if (ValueR.ComponentCount != 1 || ValueG.ComponentCount != 1)
		{
			EdNodeGraph::FError Error;
			Error.Node = R->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = Format("MakeFloat2 requires two Float inputs, got {} and {}",
				GetVectorType(ValueR.Type).c_str(),
				GetVectorType(ValueG.Type).c_str());

			AddError(Error);
			GetActiveChunk().append("// ERROR: Type mismatch in MakeFloat2\n");
			return;
		}

		FString RMask = GetSwizzleForMask(ValueR.Mask);
		FString GMask = GetSwizzleForMask(ValueG.Mask);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = float2(" + ValueR.Value + RMask + ", " +  ValueG.Value + GMask + ");\n");
	}

	void FMaterialCompiler::MakeFloat3(CMaterialInput* R, CMaterialInput* G, CMaterialInput* B)
	{
		const FString OwningNode = R->GetOwningNode()->GetNodeFullName();
		FInputValue ValueR = GetTypedInputValue(R, 0.0f);
		FInputValue ValueG = GetTypedInputValue(G, 0.0f);
		FInputValue ValueB = GetTypedInputValue(B, 0.0f);

		const FString TypeStr = GetVectorType(EMaterialInputType::Float3);

		if (ValueR.ComponentCount != 1 || ValueG.ComponentCount != 1 || ValueB.ComponentCount != 1)
		{
			EdNodeGraph::FError Error;
			Error.Node = R->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = Format("MakeFloat3 requires three Float inputs, got {}, {} and {}",
				GetVectorType(ValueR.Type).c_str(),
				GetVectorType(ValueG.Type).c_str(),
				GetVectorType(ValueB.Type).c_str());

			AddError(Error);
			GetActiveChunk().append("// ERROR: Type mismatch in MakeFloat3\n");
			return;
		}

		FString RMask = GetSwizzleForMask(ValueR.Mask);
		FString GMask = GetSwizzleForMask(ValueG.Mask);
		FString BMask = GetSwizzleForMask(ValueB.Mask);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = float3(" + ValueR.Value + RMask + ", " +  ValueG.Value + GMask + ", " + ValueB.Value + BMask + ");\n");
	}

	void FMaterialCompiler::MakeFloat4(CMaterialInput* R, CMaterialInput* G, CMaterialInput* B, CMaterialInput* A)
	{
		const FString OwningNode = R->GetOwningNode()->GetNodeFullName();
		FInputValue ValueR = GetTypedInputValue(R, 0.0f);
		FInputValue ValueG = GetTypedInputValue(G, 0.0f);
		FInputValue ValueB = GetTypedInputValue(B, 0.0f);
		FInputValue ValueA = GetTypedInputValue(A, 0.0f);

		const FString TypeStr = GetVectorType(EMaterialInputType::Float4);

		if (ValueR.ComponentCount != 1 || ValueG.ComponentCount != 1 || ValueB.ComponentCount != 1 || ValueA.ComponentCount != 1)
		{
			EdNodeGraph::FError Error;
			Error.Node = R->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = Format("MakeFloat4 requires four Float inputs, got {}, {}, {} and {}",
				GetVectorType(ValueR.Type).c_str(),
				GetVectorType(ValueG.Type).c_str(),
				GetVectorType(ValueB.Type).c_str(),
				GetVectorType(ValueA.Type).c_str());

			AddError(Error);
			GetActiveChunk().append("// ERROR: Type mismatch in MakeFloat4\n");
		}

		FString RMask = GetSwizzleForMask(ValueR.Mask);
		FString GMask = GetSwizzleForMask(ValueG.Mask);
		FString BMask = GetSwizzleForMask(ValueB.Mask);
		FString AMask = GetSwizzleForMask(ValueA.Mask);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = float4(" + ValueR.Value + RMask + ", "
			+  ValueG.Value + GMask + ", " + ValueB.Value + BMask + ", " + ValueA.Value + AMask + ");\n");
	}

	void FMaterialCompiler::Append(CMaterialInput* A, CMaterialInput* B)
	{
		const FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, 0.0f);
		FInputValue BValue = GetTypedInputValue(B, 0.0f);

		int32 TotalComponents = AValue.ComponentCount + BValue.ComponentCount;
		if (TotalComponents > 4)
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Append Too Wide";
			Error.Description = "Append result would have more than 4 components.";
			AddError(Error);
			TotalComponents = 4;
		}

		EMaterialInputType ResultType = GetTypeFromComponentCount(TotalComponents);
		FString TypeStr = GetVectorType(ResultType);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + TypeStr + "(" + AValue.Value + GetSwizzleForMask(AValue.Mask) + ", " + BValue.Value + GetSwizzleForMask(BValue.Mask) + ");\n");

		// This carries a uniform tiling value through to TexCoords instead of degrading the UV chain.
		RegisterDeriv(OwningNode, (AValue.Deriv == EDerivState::Zero && BValue.Deriv == EDerivState::Zero)
		                        ? EDerivState::Zero : EDerivState::Unknown);

		SetOwningOutputType(A, ResultType);
	}

	void FMaterialCompiler::ComponentMask(CMaterialInput* A)
	{
		CMaterialExpression_ComponentMask* OwningNode = A->GetOwningNode<CMaterialExpression_ComponentMask>();
		const FString ResultName = OwningNode->GetNodeFullName();

		// Falling through used to append a bare masked scalar and surface later as a Slang syntax error.
		auto EmitFallback = [&]
		{
			GetActiveChunk().append("float " + ResultName + " = 0.0;\n");
			SetOwningOutputType(A, EMaterialInputType::Float);
		};

		if (!A->HasConnection())
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Invalid Action";
			Error.Description.append("ComponentMask is required to have an input value");

			AddError(Error);
			EmitFallback();
			return;
		}

		FInputValue Value = GetTypedInputValue(A, "");

		// The node offers every channel whatever it is fed, so selecting past the end is the failure.
		const int32 MaskComponents = GetComponentCount(Value.Mask);
		const int32 Available = MaskComponents > 0 ? MaskComponents : GetComponentCount(Value.Type);

		const bool bRequested[4] = { OwningNode->R, OwningNode->G, OwningNode->B, OwningNode->A };
		constexpr char Channels[4] = { 'r', 'g', 'b', 'a' };

		FString Swizzle;
		int32 ComponentCount = 0;
		FString Missing;

		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (!bRequested[Index])
			{
				continue;
			}

			if (Index >= Available)
			{
				Missing.push_back(Channels[Index]);
				continue;
			}

			Swizzle.push_back(Channels[Index]);
			ComponentCount++;
		}

		if (!Missing.empty())
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Type Mismatch";
			Error.Description = Format("ComponentMask selects channel(s) '{}' that its input does not have; the input is a {}.",
				Missing.c_str(), GetVectorType(Available).c_str());

			AddError(Error);
		}

		if (ComponentCount == 0)
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Invalid Action";
			Error.Description.append("ComponentMask has no channel selected that its input provides");

			AddError(Error);
			EmitFallback();
			return;
		}

		// A channel output pin hands back the whole vector and relies on the consumer to swizzle.
		GetActiveChunk().append(GetVectorType(ComponentCount) + " " + ResultName + " = "
			+ Value.Value + GetSwizzleForMask(Value.Mask) + "." + Swizzle + ";\n");

		// The derivative takes the same swizzle, so a world-space UV can arrive wide and leave narrow.
		if (Value.Deriv == EDerivState::Valid)
		{
			const FString Selector = GetSwizzleForMask(Value.Mask) + "." + Swizzle;
			RegisterDeriv(ResultName, EDerivState::Valid,
			              Value.DDX + Selector, Value.DDY + Selector, ComponentCount);
		}
		else
		{
			RegisterDeriv(ResultName, Value.Deriv == EDerivState::Zero
			                        ? EDerivState::Zero : EDerivState::Unknown);
		}

		SetOwningOutputType(A, GetTypeFromComponentCount(ComponentCount));
	}

	void FMaterialCompiler::DefineTextureSample(const FString& ID)
	{
		return;
	}

	int32 FMaterialCompiler::BindTexture(CTexture* Texture)
	{
		auto It = Algo::Find(BoundImages.begin(), BoundImages.end(), Texture);
		if (It != BoundImages.end())
		{
			return (int32)std::distance(BoundImages.begin(), It);
		}

		const int32 Index = (int32)BoundImages.size();
		BoundImages.push_back(Texture);
		NumTextureParams++;
		return Index;
	}

	int32 FMaterialCompiler::BindTextureParameter(const FName& ParamID, CTexture* Texture)
	{
		auto Existing = TextureParameters.find(ParamID);
		if (Existing != TextureParameters.end())
		{
			return (int32)Existing->second.Index;
		}

		const int32 Index = (int32)BoundImages.size();
		BoundImages.push_back(Texture);
		TextureParameters[ParamID] = FTextureParam{ (uint16)Index, Texture };
		NumTextureParams++;
		return Index;
	}

	// A 32x-tiled UV on UV0's gradient picks a mip five levels too fine, so this warns in the editor.
	void FMaterialCompiler::WarnUVGradientFallback(const FInputValue& UVValue, CEdGraphNode* Node)
	{
		if (UVValue.Deriv == EDerivState::Valid)
		{
			return;
		}

		EdNodeGraph::FError Warning;
		Warning.Node        = Node;
		Warning.Name        = "UV Gradient Fallback";
		Warning.Description = "This sample's UV chain ('" + UVValue.Value + "') has no analytic derivative, so "
		                      "the deferred pass samples it with UV0's gradient instead. Mips will be selected "
		                      "too fine wherever the UV is scaled or distorted relative to UV0 -- a tiled UV is "
		                      "the common case, and the cost is texture bandwidth, not a visible error. Feed the "
		                      "sample from UV0 through nodes that carry derivatives, or accept the cost if the "
		                      "UV is close to UV0 in scale.";
		AddWarning(Warning);
	}

	void FMaterialCompiler::TextureSample(const FString& ID, CTexture* Texture, CMaterialInput* Input, CEdGraphNode* Node, FStringView SamplerName)
	{
		if (Texture == nullptr || Texture->GetResourceID() < 0)
		{
			return;
		}

		FInputValue UVValue = GetTypedInputValue(Input, "float2(UV0)");

		FString UVStr;
		if (UVValue.ComponentCount >= 2)
		{
			UVStr = UVValue.Value + ".xy";
		}
		else
		{
			UVStr = "float2(" + UVValue.Value + ")";
		}

		const int32 Index = BindTexture(Texture);
		const FString SamplerStr(SamplerName.data(), SamplerName.size());

		if (!LaneSamplesWithGradients())
		{
			GetActiveChunk().append("float4 " + ID + " = SampleTexture2DLevel(GetMaterialTexture(MaterialIndex, "
				+ Format("{}", Index) + "), " + SamplerStr + ", " + UVStr + ", 0.0);\n");
			return;
		}

		// The forward templates ignore these, so the chunks dead-strip outside the deferred pass.
		FString Ddx, Ddy;
		GetUVGradients(UVValue, Ddx, Ddy);

		// Names the variable that broke the chain, so a shader dump localizes the unruled node.
		if (UVValue.Deriv != EDerivState::Valid)
		{
			GetActiveChunk().append("// UV-GRADIENT FALLBACK: '" + UVValue.Value
				+ "' has no analytic derivative, sampling with UV0's gradient (mip may be too fine).\n");
		}
		WarnUVGradientFallback(UVValue, Node);

		GetActiveChunk().append("float4 " + ID + " = SampleTexture2DAuto(GetMaterialTexture(MaterialIndex, " + Format("{}", Index) + "), " + SamplerStr + ", " + UVStr + ", " + Ddx + ", " + Ddy + ");\n");
	}

	void FMaterialCompiler::TextureSampleParameter(const FString& ID, const FName& ParamID, CTexture* Texture, CMaterialInput* Input, CEdGraphNode* Node, FStringView SamplerName)
	{
		FInputValue UVValue = GetTypedInputValue(Input, "float2(UV0)");

		FString UVStr;
		if (UVValue.ComponentCount >= 2)
		{
			UVStr = UVValue.Value + ".xy";
		}
		else
		{
			UVStr = "float2(" + UVValue.Value + ")";
		}

		const int32 Index = BindTextureParameter(ParamID, Texture);
		const FString SamplerStr(SamplerName.data(), SamplerName.size());

		if (!LaneSamplesWithGradients())
		{
			GetActiveChunk().append("float4 " + ID + " = SampleTexture2DLevel(GetMaterialTexture(MaterialIndex, "
				+ Format("{}", Index) + "), " + SamplerStr + ", " + UVStr + ", 0.0);\n");
			return;
		}

		// The forward templates ignore these, so the chunks dead-strip outside the deferred pass.
		FString Ddx, Ddy;
		GetUVGradients(UVValue, Ddx, Ddy);

		// Names the variable that broke the chain, so a shader dump localizes the unruled node.
		if (UVValue.Deriv != EDerivState::Valid)
		{
			GetActiveChunk().append("// UV-GRADIENT FALLBACK: '" + UVValue.Value
				+ "' has no analytic derivative, sampling with UV0's gradient (mip may be too fine).\n");
		}
		WarnUVGradientFallback(UVValue, Node);

		GetActiveChunk().append("float4 " + ID + " = SampleTexture2DAuto(GetMaterialTexture(MaterialIndex, " + Format("{}", Index) + "), " + SamplerStr + ", " + UVStr + ", " + Ddx + ", " + Ddy + ");\n");
	}

	void FMaterialCompiler::TextureSampleArray(CMaterialGraphNode* Node, int32 TextureIndex, uint32 NumLayers,
	                                           CMaterialInput* UV, CMaterialInput* Slice)
	{
		const FString OwningNode = GetCurrentInlinePrefix() + Node->GetNodeFullName();

		// Downstream nodes read this by name, so a rejection below still leaves a compilable shader.
		AddRaw("float4 " + OwningNode + " = float4(0.0, 0.0, 0.0, 1.0);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float4);

		if (TextureIndex < 0)
		{
			EdNodeGraph::FError Error;
			Error.Node        = Node;
			Error.Name        = "Texture Sample Array";
			Error.Description = "TextureSampleArray needs a Texture Array asset assigned, and that asset must "
			                    "have at least one built layer.";
			AddError(Error);
			return;
		}

		FInputValue UVValue    = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue SliceValue = GetTypedInputValue(Slice, 0.0f);

		const FString UVStr = UVValue.ComponentCount >= 2
		                    ? UVValue.Value + ".xy"
		                    : "float2(" + UVValue.Value + ")";

		// Rounded, since arithmetic lands on 2.999 as readily as 3.0, and clamped since an OOB index is undefined.
		const FString MaxLayer = Format("{}", NumLayers > 0 ? NumLayers - 1u : 0u);
		AddRaw("uint " + OwningNode + "_Slice = (uint)clamp(round(" + SliceValue.Value + "), 0.0, "
			+ MaxLayer + ".0);\n");

		const FString TexStr = "GetMaterialTexture(MaterialIndex, " + Format("{}", TextureIndex) + ")";

		if (!LaneSamplesWithGradients())
		{
			AddRaw(OwningNode + " = SampleTexture2DArrayLevel(" + TexStr + ", SAMPLER_LINEAR_WRAP, "
				+ UVStr + ", " + OwningNode + "_Slice, 0.0);\n");
			return;
		}

		FString Ddx, Ddy;
		GetUVGradients(UVValue, Ddx, Ddy);

		if (UVValue.Deriv != EDerivState::Valid)
		{
			AddRaw("// UV-GRADIENT FALLBACK: '" + UVValue.Value
				+ "' has no analytic derivative, sampling with UV0's gradient (mip may be too fine).\n");
		}
		WarnUVGradientFallback(UVValue, Node);

		AddRaw(OwningNode + " = SampleTexture2DArrayAuto(" + TexStr + ", SAMPLER_LINEAR_WRAP, " + UVStr + ", "
			+ OwningNode + "_Slice, " + Ddx + ", " + Ddy + ");\n");
	}

	namespace
	{
		// Curve constants are emitted as plain literals; matches the formatting the other emitters use.
		FString CurveFloat(float Value)
		{
			return Format("{}", Value);
		}
	}

	void FMaterialCompiler::CurveSample(const FString& ID, const SKeyedCurve& Curve, CMaterialInput* TimeInput)
	{
		SetOwningOutputType(TimeInput, EMaterialInputType::Float);

		FString& Chunk = GetActiveChunk();

		const int32 NumKeys = Curve.NumKeys();
		if (NumKeys == 0)
		{
			Chunk.append("float " + ID + " = 0.0;\n");
			return;
		}

		if (NumKeys == 1)
		{
			Chunk.append("float " + ID + " = " + CurveFloat(Curve.Keys[0].Value) + ";\n");
			return;
		}

		TVector<FCurveSegment> Segments;
		Curve.BakeSegments(Segments);
		if (Segments.empty())
		{
			Chunk.append("float " + ID + " = " + CurveFloat(Curve.Keys[0].Value) + ";\n");
			return;
		}

		const FInputValue TimeValue = GetTypedInputValue(TimeInput, "0.0");
		const FString TimeStr = TimeValue.ComponentCount >= 2 ? ("(" + TimeValue.Value + ").x") : TimeValue.Value;

		const float FirstTime  = Curve.Keys.front().Time;
		const float LastTime   = Curve.Keys.back().Time;
		const float FirstValue = Curve.Keys.front().Value;
		const float LastValue  = Curve.Keys.back().Value;
		const float Span       = LastTime - FirstTime;

		Chunk.append("float " + ID + "_T = " + TimeStr + ";\n");

		if (Span <= 1e-6f)
		{
			// Every key shares a time, so only the two end values are reachable.
			Chunk.append("float " + ID + " = (" + ID + "_T > " + CurveFloat(LastTime) + ") ? " + CurveFloat(LastValue) + " : " + CurveFloat(FirstValue) + ";\n");
			return;
		}

		float PreSlope = 0.0f;
		float PostSlope = 0.0f;
		Curve.GetExtrapolationSlopes(PreSlope, PostSlope);

		// _L is the sample time remapped into the keyed range, _O an offset for the shifting modes.
		Chunk.append("float " + ID + "_L = clamp(" + ID + "_T, " + CurveFloat(FirstTime) + ", " + CurveFloat(LastTime) + ");\n");
		Chunk.append("float " + ID + "_O = 0.0;\n");

		// The two guards are mutually exclusive, so each side can declare its own locals.
		auto EmitExtrapolation = [&](ECurveExtrapolation Mode, bool bBefore)
		{
			const FString Guard = bBefore
				? (ID + "_T < " + CurveFloat(FirstTime))
				: (ID + "_T > " + CurveFloat(LastTime));

			switch (Mode)
			{
			case ECurveExtrapolation::Linear:
				{
					const float Slope = bBefore ? PreSlope : PostSlope;
					const float Anchor = bBefore ? FirstTime : LastTime;
					Chunk.append("if (" + Guard + ") { " + ID + "_O = " + CurveFloat(Slope) + " * (" + ID + "_T - " + CurveFloat(Anchor) + "); }\n");
				}
				break;

			case ECurveExtrapolation::Cycle:
			case ECurveExtrapolation::CycleWithOffset:
			case ECurveExtrapolation::Oscillate:
				{
					FString Body;
					Body.append("float " + ID + "_D = " + ID + "_T - " + CurveFloat(FirstTime) + "; ");
					Body.append("float " + ID + "_C = floor(" + ID + "_D / " + CurveFloat(Span) + "); ");
					Body.append("float " + ID + "_W = " + ID + "_D - " + ID + "_C * " + CurveFloat(Span) + "; ");

					if (Mode == ECurveExtrapolation::Oscillate)
					{
						Body.append("if (abs(fmod(" + ID + "_C, 2.0)) >= 0.5) { " + ID + "_W = " + CurveFloat(Span) + " - " + ID + "_W; } ");
					}

					Body.append(ID + "_L = " + CurveFloat(FirstTime) + " + " + ID + "_W; ");

					if (Mode == ECurveExtrapolation::CycleWithOffset)
					{
						Body.append(ID + "_O = " + ID + "_C * " + CurveFloat(LastValue - FirstValue) + "; ");
					}

					Chunk.append("if (" + Guard + ") { " + Body + "}\n");
				}
				break;

			case ECurveExtrapolation::Clamp:
			default:
				break;   // the clamp() above already covers it.
			}
		};

		EmitExtrapolation(Curve.PreExtrapolation, true);
		EmitExtrapolation(Curve.PostExtrapolation, false);

		const int32 NumSegments = (int32)Segments.size();
		FString Coefficients = "float4 " + ID + "_K[" + Format("{}", NumSegments) + "] = { ";
		FString Ranges       = "float2 " + ID + "_R[" + Format("{}", NumSegments) + "] = { ";

		for (int32 Index = 0; Index < NumSegments; ++Index)
		{
			const FCurveSegment& Segment = Segments[Index];
			const float InvDuration = Segment.Duration > 0.0f ? (1.0f / Segment.Duration) : 0.0f;

			if (Index > 0)
			{
				Coefficients.append(", ");
				Ranges.append(", ");
			}

			Coefficients.append("float4(" + CurveFloat(Segment.A) + ", " + CurveFloat(Segment.B) + ", " + CurveFloat(Segment.C) + ", " + CurveFloat(Segment.D) + ")");
			Ranges.append("float2(" + CurveFloat(Segment.StartTime) + ", " + CurveFloat(InvDuration) + ")");
		}

		Coefficients.append(" };\n");
		Ranges.append(" };\n");
		Chunk.append(Coefficients);
		Chunk.append(Ranges);

		// _L is clamped into the range, so the last segment whose start is behind it is the right one.
		const FString Loop = ID + "_i";
		Chunk.append("float " + ID + "_V = " + CurveFloat(FirstValue) + ";\n");
		Chunk.append("for (int " + Loop + " = 0; " + Loop + " < " + Format("{}", NumSegments) + "; ++" + Loop + ")\n");
		Chunk.append("{\n");
		Chunk.append("\tfloat " + ID + "_U = saturate((" + ID + "_L - " + ID + "_R[" + Loop + "].x) * " + ID + "_R[" + Loop + "].y);\n");
		Chunk.append("\tfloat " + ID + "_E = " + ID + "_K[" + Loop + "].x + " + ID + "_U * (" + ID + "_K[" + Loop + "].y + " + ID + "_U * (" + ID + "_K[" + Loop + "].z + " + ID + "_U * " + ID + "_K[" + Loop + "].w));\n");
		Chunk.append("\t" + ID + "_V = (" + ID + "_L >= " + ID + "_R[" + Loop + "].x) ? " + ID + "_E : " + ID + "_V;\n");
		Chunk.append("}\n");
		Chunk.append("float " + ID + " = " + ID + "_V + " + ID + "_O;\n");
	}

	namespace
	{
		// Substring-only match; patterns like '.Sample(' and 'sin(' are unambiguous in generated shader code.
		uint32 CountSubstring(const FString& Haystack, const char* Needle)
		{
			const size_t NeedleLen = strlen(Needle);
			if (NeedleLen == 0)
			{
				return 0;
			}

			uint32 Count = 0;
			size_t Pos = 0;
			while ((Pos = Haystack.find(Needle, Pos)) != FString::npos)
			{
				++Count;
				Pos += NeedleLen;
			}
			return Count;
		}

		uint32 CountLines(const FString& Source)
		{
			if (Source.empty())
			{
				return 0;
			}

			uint32 Count = 0;
			for (size_t i = 0; i < Source.size(); ++i)
			{
				if (Source[i] == '\n')
				{
					++Count;
				}
			}
			return Count;
		}

		uint32 CountMathOps(const FString& Source)
		{
			static const char* const Patterns[] = {
				"sin(", "cos(", "tan(", "asin(", "acos(", "atan(", "atan2(",
				"sinh(", "cosh(", "tanh(",
				"sqrt(", "rsqrt(", "pow(", "exp(", "exp2(",
				"log(", "log2(", "log10(",
				"normalize(", "length(", "distance(", "dot(", "cross(",
				"reflect(", "refract(",
				"lerp(", "clamp(", "smoothstep(", "step(", "saturate(",
				"min(", "max(", "abs(", "sign(", "floor(", "ceil(", "round(",
				"trunc(", "frac(", "fmod(",
			};
			uint32 Total = 0;
			for (const char* P : Patterns)
			{
				Total += CountSubstring(Source, P);
			}
			return Total;
		}

		uint32 CountNoiseOps(const FString& Source)
		{
			static const char* const Patterns[] = {
				"ValueNoise(", "GradientNoise(", "PerlinNoise(",
				"VoronoiNoise(", "SimpleNoise(",
				"Hash11(", "Hash21(", "Hash22(", "Hash33(",
				// One ComputeWind is an Octaves-deep sine sum, so it belongs in the noise bucket.
				"ComputeWind(",
			};
			uint32 Total = 0;
			for (const char* P : Patterns)
			{
				Total += CountSubstring(Source, P);
			}
			return Total;
		}
	}

	FMaterialCompiler::FShaderStats FMaterialCompiler::GetStats() const
	{
		FShaderStats Stats;

		const FString PixelAll  = PixelChunks  + PixelOutputChunks;
		const FString VertexAll = VertexChunks + VertexOutputChunks;

		Stats.PixelInstructions   = CountLines(PixelAll);
		Stats.VertexInstructions  = CountLines(VertexAll);
		Stats.PixelCharacters     = static_cast<uint32>(PixelAll.size());
		Stats.VertexCharacters    = static_cast<uint32>(VertexAll.size());

		Stats.TextureSamples      = CountSubstring(PixelAll, ".Sample(") + CountSubstring(VertexAll, ".Sample(");
		Stats.MathOps             = CountMathOps(PixelAll) + CountMathOps(VertexAll);
		Stats.NoiseOps            = CountNoiseOps(PixelAll) + CountNoiseOps(VertexAll);
		Stats.UVGradientFallbacks = UVGradientFallbackCount;

		Stats.ScalarParameters    = NumScalarParams;
		Stats.VectorParameters    = NumVectorParams;
		Stats.TextureParameters   = NumTextureParams;
		Stats.BoundTextures       = static_cast<uint32>(BoundImages.size());
		Stats.bUsesVertexStage    = UsesVertexStage();

		// Vertex-stage work is amortized across vertices, so it counts less than per-pixel work.
		Stats.EstimatedCost =
			Stats.TextureSamples       * 8 +
			Stats.NoiseOps             * 16 +
			Stats.MathOps              * 1 +
			Stats.PixelInstructions    * 1 +
			Stats.VertexInstructions   / 2;

		return Stats;
	}

	void FMaterialCompiler::GetParameters(TVector<FMaterialParameter>& OutParams, FMaterialUniforms& OutUniforms) const
	{
		for (const auto& Pair : ScalarParameters)
		{
			FMaterialParameter Out;
			Out.ParameterName = Pair.first;
			Out.Type = EMaterialParameterType::Scalar;
			Out.Index = Pair.second.Index;
			Out.ScalarDefault = Pair.second.Value;
			OutParams.push_back(Out);

			if (Pair.second.Index < MAX_SCALARS)
			{
				OutUniforms.Scalars[Pair.second.Index] = Pair.second.Value;
			}
		}

		for (const auto& Pair : VectorParameters)
		{
			FMaterialParameter Out;
			Out.ParameterName = Pair.first;
			Out.Type = EMaterialParameterType::Vector;
			Out.Index = Pair.second.Index;
			Out.VectorDefault = Pair.second.Value;
			OutParams.push_back(Out);

			if (Pair.second.Index < MAX_VECTORS)
			{
				OutUniforms.Vectors[Pair.second.Index] = Pair.second.Value;
			}
		}

		for (const auto& Pair : TextureParameters)
		{
			FMaterialParameter Out;
			Out.ParameterName = Pair.first;
			Out.Type = EMaterialParameterType::Texture;
			Out.Index = Pair.second.Index;
			OutParams.push_back(Out);
		}
	}

	bool FMaterialCompiler::RequirePixelStage(CMaterialGraphNode* Node, const FString& NodeKindName)
	{
		if (CurrentStage == EMaterialCompileStage::Pixel)
		{
			return true;
		}

		EdNodeGraph::FError Error;
		Error.Node = Node;
		Error.Name = "Stage Error";
		Error.Description = NodeKindName + " is only available in the pixel stage and cannot feed World Position Offset.";
		AddError(Error);
		return false;
	}

	bool FMaterialCompiler::RejectInUI(CMaterialGraphNode* Node, const char* NodeName)
	{
		if (CurrentMaterialType != EMaterialType::UI)
		{
			return false;
		}

		EdNodeGraph::FError Error;
		Error.Name        = NodeName;
		Error.Description = FString(NodeName) + " is not available in UI materials -- the fullscreen brush pass has no surface geometry, camera, or scene depth. It reads as a neutral default.";
		Error.Node        = Node;
		AddError(Error);
		return true;
	}

	void FMaterialCompiler::NewLine()
	{
		GetActiveChunk().append("\n");
	}

	// Built-in scene inputs

	void FMaterialCompiler::VertexNormal(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Vertex Normal"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 0.0, 1.0);\n");
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = WorldNormal.xyz;\n");
	}

	void FMaterialCompiler::VertexTangent(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Vertex Tangent"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(1.0, 0.0, 0.0);\n");
			return;
		}
		// Input is the pixel stage's interpolant struct, and both stages declare WorldTangent.
		GetActiveChunk().append("float3 " + ID + " = WorldTangent.xyz;\n");
	}

	void FMaterialCompiler::VertexBitangent(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Vertex Bitangent"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 1.0, 0.0);\n");
			return;
		}
		// WorldTangent's w carries the handedness sign the bitangent needs on mirrored UVs.
		GetActiveChunk().append("float3 " + ID + " = cross(WorldNormal.xyz, WorldTangent.xyz) * WorldTangent.w;\n");
	}

	void FMaterialCompiler::VertexColor(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Vertex Color"))
		{
			GetActiveChunk().append("float4 " + ID + " = float4(1.0, 1.0, 1.0, 1.0);\n");
			RegisterDeriv(ID, EDerivState::Zero);
			return;
		}
		GetActiveChunk().append("float4 " + ID + " = VertexColor;\n");
		RegisterDeriv(ID, EDerivState::Valid, "VertexColor_DDX", "VertexColor_DDY", 4);
	}

	void FMaterialCompiler::TexCoords(const FString& ID, uint32 Index, CMaterialInput* Tiling, float UTiling, float VTiling,
	                                  CMaterialInput* Rotation, float RotationDegrees)
	{
		// Connected Tiling pin overrides the inline UTiling/VTiling defaults.
		FInputValue TilingValue = GetTypedInputValue(Tiling, "float2(" + Format("{}", UTiling) + ", " + Format("{}", VTiling) + ")");

		FString Scale = TilingValue.Value;

		// The unconnected default is already a float2 literal; only a connection needs widening.
		if (Tiling != nullptr && Tiling->HasConnection())
		{
			Scale += GetSwizzleForMask(TilingValue.Mask);

			// A mask narrows the connection, so it decides the width where there is one.
			const int32 MaskComponents = GetComponentCount(TilingValue.Mask);
			const int32 Components = MaskComponents > 0 ? MaskComponents : TilingValue.ComponentCount;

			if (Components <= 1)
			{
				// A scalar tiles both axes by the same amount, which is what tiling usually wants.
				Scale = "float2(" + Scale + ")";
			}
			else if (Components > 2)
			{
				Scale = "float2((" + Scale + ").xy)";
			}
		}

		// Every stage declares both sets, so an out-of-range index clamps rather than reading zero.
		const FString SetName = (Index >= 1) ? FString("UV1") : FString("UV0");

		// Rotation applies after tiling about the UV origin, matching KHR_texture_transform order.
		const bool bRotationConnected = (Rotation != nullptr && Rotation->HasConnection());
		FInputValue RotationValue = GetTypedInputValue(Rotation, Format("{}", Math::Radians(RotationDegrees)));
		const bool  bHasRotation  = bRotationConnected || RotationDegrees != 0.0f;

		if (!bHasRotation)
		{
			GetActiveChunk().append("float2 " + ID + " = " + SetName + " * " + Scale + ";\n");
		}
		else
		{
			const FString Scaled = ID + "_Scaled";
			const FString Cos    = ID + "_Cos";
			const FString Sin    = ID + "_Sin";

			GetActiveChunk().append("float2 " + Scaled + " = " + SetName + " * " + Scale + ";\n");
			GetActiveChunk().append("float " + Cos + " = cos(" + RotationValue.Value + ");\n");
			GetActiveChunk().append("float " + Sin + " = sin(" + RotationValue.Value + ");\n");
			GetActiveChunk().append("float2 " + ID + " = float2("
			                      + Scaled + ".x * " + Cos + " - " + Scaled + ".y * " + Sin + ", "
			                      + Scaled + ".x * " + Sin + " + " + Scaled + ".y * " + Cos + ");\n");
		}

		// Without the chain rule the deferred pass sampled tiled materials several mips too fine.
		FInputValue UVSetValue;
		UVSetValue.Value = SetName;
		UVSetValue.Deriv = EDerivState::Valid;
		UVSetValue.DDX   = SetName + "_DDX";
		UVSetValue.DDY   = SetName + "_DDY";

		const bool bScaleIsConstant = (Tiling == nullptr || !Tiling->HasConnection())
		                           || TilingValue.Deriv == EDerivState::Zero;

		// A uniform-driven rotation is still derivative-free, so only a per-pixel one forces a fallback.
		const bool bRotationIsConstant = !bRotationConnected || RotationValue.Deriv == EDerivState::Zero;

		if (!bScaleIsConstant || !bRotationIsConstant)
		{
			RegisterDeriv(ID, EDerivState::Unknown);
			return;
		}

		if (!bHasRotation)
		{
			RegisterScaledDeriv(ID, UVSetValue, Scale);
			return;
		}

		// Rotation is linear, so the same basis change applies to the gradients.
		const FString Cos = ID + "_Cos";
		const FString Sin = ID + "_Sin";

		auto RotateGradient = [&](const FString& Source) -> FString
		{
			const FString Scaled = "(" + Source + " * " + Scale + ")";
			return "float2(" + Scaled + ".x * " + Cos + " - " + Scaled + ".y * " + Sin + ", "
			                 + Scaled + ".x * " + Sin + " + " + Scaled + ".y * " + Cos + ")";
		};

		RegisterDeriv(ID, EDerivState::Valid,
		              RotateGradient(UVSetValue.DDX),
		              RotateGradient(UVSetValue.DDY),
		              2);
	}

	void FMaterialCompiler::Panner(CMaterialInput* UV, CMaterialInput* Time, CMaterialInput* Speed)
	{
		CMaterialExpression_Panner* PannerNode = UV->GetOwningNode<CMaterialExpression_Panner>();

		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue TimeValue = GetTypedInputValue(Time, "GetTime()");
		FInputValue SpeedValue = GetTypedInputValue(Speed, "float2(" + Format("{}", PannerNode->SpeedX) + ", " + Format("{}", PannerNode->SpeedY) + ")");
		const FString OwningNode = UV->GetOwningNode()->GetNodeFullName();

		GetActiveChunk().append("float2 " + OwningNode + " = " + UVValue.Value + " + " + SpeedValue.Value + " * " + TimeValue.Value + ";\n");

		// A pan is a translation, and panning must not change the mip.
		const bool bOffsetIsConstant = SpeedValue.Deriv == EDerivState::Zero
		                            && TimeValue.Deriv  == EDerivState::Zero;
		if (bOffsetIsConstant && UVValue.Deriv == EDerivState::Valid)
		{
			RegisterDeriv(OwningNode, EDerivState::Valid, UVValue.DDX, UVValue.DDY);
		}
		else if (bOffsetIsConstant && UVValue.Deriv == EDerivState::Zero)
		{
			RegisterDeriv(OwningNode, EDerivState::Zero);
		}
		else
		{
			RegisterDeriv(OwningNode, EDerivState::Unknown);
		}

		PannerNode->Output->SetInputType(EMaterialInputType::Float2);
		PannerNode->Output->SetComponentMask(EComponentMask::RG);
	}

	void FMaterialCompiler::RotateUV(CMaterialInput* UV, CMaterialInput* Center, CMaterialInput* Rotation)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue CenterValue = GetTypedInputValue(Center, "float2(0.5, 0.5)");
		FInputValue RotValue = GetTypedInputValue(Rotation, 0.0f);

		GetActiveChunk().append("float2 " + OwningNode + "_C = " + UVValue.Value + " - " + CenterValue.Value + ";\n");
		GetActiveChunk().append("float  " + OwningNode + "_S = sin(" + RotValue.Value + ");\n");
		GetActiveChunk().append("float  " + OwningNode + "_K = cos(" + RotValue.Value + ");\n");
		GetActiveChunk().append("float2 " + OwningNode + " = float2("
			+ OwningNode + "_C.x * " + OwningNode + "_K - " + OwningNode + "_C.y * " + OwningNode + "_S, "
			+ OwningNode + "_C.x * " + OwningNode + "_S + " + OwningNode + "_C.y * " + OwningNode + "_K) + " + CenterValue.Value + ";\n");

		// A rotation is linear provided the angle and center are uniform, reusing the sin and cos locals.
		if (UVValue.Deriv == EDerivState::Valid
		 && RotValue.Deriv == EDerivState::Zero
		 && CenterValue.Deriv == EDerivState::Zero)
		{
			auto Rotate = [&](const FString& D)
			{
				return "float2(" + D + ".x * " + OwningNode + "_K - " + D + ".y * " + OwningNode + "_S, "
				                 + D + ".x * " + OwningNode + "_S + " + D + ".y * " + OwningNode + "_K)";
			};
			RegisterDeriv(OwningNode, EDerivState::Valid, Rotate(UVValue.DDX), Rotate(UVValue.DDY));
		}
		else if (UVValue.Deriv == EDerivState::Zero && RotValue.Deriv == EDerivState::Zero)
		{
			RegisterDeriv(OwningNode, EDerivState::Zero);
		}
		else
		{
			RegisterDeriv(OwningNode, EDerivState::Unknown);
		}

		SetOwningOutputType(UV, EMaterialInputType::Float2);
	}

	void FMaterialCompiler::TilingAndOffset(CMaterialInput* UV, CMaterialInput* Tiling, CMaterialInput* Offset)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue TilingValue = GetTypedInputValue(Tiling, "float2(1.0, 1.0)");
		FInputValue OffsetValue = GetTypedInputValue(Offset, "float2(0.0, 0.0)");

		GetActiveChunk().append("float2 " + OwningNode + " = " + UVValue.Value + " * " + TilingValue.Value + " + " + OffsetValue.Value + ";\n");

		// Affine in UV, provided the tiling and offset are themselves constant.
		if (TilingValue.Deriv == EDerivState::Zero && OffsetValue.Deriv == EDerivState::Zero)
		{
			RegisterScaledDeriv(OwningNode, UVValue, TilingValue.Value);
		}
		else
		{
			RegisterDeriv(OwningNode, EDerivState::Unknown);
		}

		SetOwningOutputType(UV, EMaterialInputType::Float2);
	}

	void FMaterialCompiler::FlipBookUV(CMaterialInput* UV, CMaterialInput* NumCols, CMaterialInput* NumRows, CMaterialInput* Time, CMaterialInput* FPS)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue ColsValue = GetTypedInputValue(NumCols, 1.0f);
		FInputValue RowsValue = GetTypedInputValue(NumRows, 1.0f);
		FInputValue TimeValue = GetTypedInputValue(Time, "GetTime()");
		FInputValue FPSValue = GetTypedInputValue(FPS, 30.0f);

		GetActiveChunk().append("float " + OwningNode + "_FN = floor((" + TimeValue.Value + ") * (" + FPSValue.Value + "));\n");
		GetActiveChunk().append("float " + OwningNode + "_NF = max((" + ColsValue.Value + ") * (" + RowsValue.Value + "), 1.0);\n");
		GetActiveChunk().append("float " + OwningNode + "_FI = fmod(" + OwningNode + "_FN, " + OwningNode + "_NF);\n");
		GetActiveChunk().append("float " + OwningNode + "_CX = fmod(" + OwningNode + "_FI, max((" + ColsValue.Value + "), 1.0));\n");
		GetActiveChunk().append("float " + OwningNode + "_CY = floor(" + OwningNode + "_FI / max((" + ColsValue.Value + "), 1.0));\n");
		GetActiveChunk().append("float2 " + OwningNode + " = float2(((" + UVValue.Value + ").x + " + OwningNode + "_CX) / max((" + ColsValue.Value + "), 1.0), 1.0 - (((" + UVValue.Value + ").y + " + OwningNode + "_CY + 1.0) / max((" + RowsValue.Value + "), 1.0)));\n");
		SetOwningOutputType(UV, EMaterialInputType::Float2);
	}

	void FMaterialCompiler::ParallaxOcclusionMapping(CMaterialGraphNode* Node, int32 HeightTextureIndex, const FParallaxInputs& Inputs,
	                                                 CMaterialOutput* UVOut, CMaterialOutput* ShadowOut, CMaterialOutput* HeightOut)
	{
		const FString Prefix = GetCurrentInlinePrefix() + Node->GetNodeFullName();
		const FString UVVar     = Prefix + "_UV";
		const FString ShadowVar = Prefix + "_Shadow";
		const FString HeightVar = Prefix + "_Height";

		FInputValue UVValue = GetTypedInputValue(Inputs.UV, "float2(UV0)");
		FString     UVStr   = UVValue.ComponentCount >= 2 ? UVValue.Value + ".xy" : "float2(" + UVValue.Value + ")";

		// Every downstream node reads these through ResolvedVar, so a rejection must still compile.
		AddRaw("float2 " + UVVar + " = " + UVStr + ";\n");
		AddRaw("float " + ShadowVar + " = 1.0;\n");
		AddRaw("float " + HeightVar + " = 1.0;\n");

		if (UVOut)     UVOut->ResolvedVar     = UVVar;
		if (ShadowOut) ShadowOut->ResolvedVar = ShadowVar;
		if (HeightOut) HeightOut->ResolvedVar = HeightVar;

		// The vertex graph has no view ray or tangent frame, so the march would reference missing locals.
		if (CurrentStage != EMaterialCompileStage::Pixel)
		{
			EdNodeGraph::FError Error;
			Error.Node        = Node;
			Error.Name        = "Parallax Occlusion Mapping";
			Error.Description = "ParallaxOcclusionMapping is a pixel-stage node; it cannot be reached from World Position Offset.";
			AddError(Error);
			return;
		}

		// UI and PostProcess are fullscreen passes with no tangent frame and no view ray to displace along.
		const EMaterialType Domain = GetMaterialType();
		if (Domain == EMaterialType::UI || Domain == EMaterialType::PostProcess)
		{
			EdNodeGraph::FError Error;
			Error.Node        = Node;
			Error.Name        = "Parallax Occlusion Mapping";
			Error.Description = "ParallaxOcclusionMapping needs a surface to displace; it is not available in UI or PostProcess materials.";
			AddError(Error);
			return;
		}

		if (HeightTextureIndex < 0)
		{
			EdNodeGraph::FError Error;
			Error.Node        = Node;
			Error.Name        = "Parallax Occlusion Mapping";
			Error.Description = "ParallaxOcclusionMapping needs a Height Map texture assigned.";
			AddError(Error);
			return;
		}

		// A masked material also runs in the VisBuffer pre-pass, so the march is paid twice per pixel.
		if (IsMasked())
		{
			LOG_WARN("[Material] ParallaxOcclusionMapping in a MASKED material: the height-field march also runs "
			         "in the VisBuffer masked pre-pass, roughly doubling its cost. Prefer Opaque unless the "
			         "cutout is required.");
		}

		FInputValue ScaleValue     = GetTypedInputValue(Inputs.HeightScale, 0.05f);
		FInputValue MinValue       = GetTypedInputValue(Inputs.MinSamples, 8.0f);
		FInputValue MaxValue       = GetTypedInputValue(Inputs.MaxSamples, 32.0f);
		FInputValue LODValue       = GetTypedInputValue(Inputs.LODThreshold, 6.0f);
		FInputValue ShadowSamples  = GetTypedInputValue(Inputs.ShadowSamples, 0.0f);
		FInputValue ShadowSoftness = GetTypedInputValue(Inputs.ShadowSoftness, 1.0f);

		const FString TexStr = "GetMaterialTexture(MaterialIndex, " + Format("{}", HeightTextureIndex) + ")";

		AddRaw("FParallaxResult " + Prefix + "_R = ParallaxOcclusion(" + TexStr + ", SAMPLER_LINEAR_WRAP, "
			+ UVStr + ", normalize(GetCameraPosition() - WorldPosition), WorldNormal, WorldTangent, "
			+ "(" + ScaleValue.Value + "), (" + MinValue.Value + "), (" + MaxValue.Value + "), (" + LODValue.Value + "), "
			+ "UV0_DDX, UV0_DDY);\n");
		AddRaw(UVVar + " = " + Prefix + "_R.UV;\n");
		AddRaw(HeightVar + " = " + Prefix + "_R.Height;\n");

		// Opt-in, since it is a second march per pixel and an unused Shadow output should cost nothing.
		AddRaw("if ((" + ShadowSamples.Value + ") > 0.0 && LightData().bHasSun != 0u && " + Prefix + "_R.Weight > 0.0)\n");
		AddRaw("{\n");
		AddRaw("\t" + ShadowVar + " = ParallaxSelfShadow(" + TexStr + ", SAMPLER_LINEAR_WRAP, " + UVVar + ", "
			+ Prefix + "_R.Height, GetSunDirection(), WorldNormal, WorldTangent, (" + ScaleValue.Value + "), ("
			+ ShadowSamples.Value + "), (" + ShadowSoftness.Value + "), ParallaxMipLevel(" + TexStr + ", UV0_DDX, UV0_DDY));\n");
		// A surface faded to flat normal mapping must not still cast parallax shadows onto itself.
		AddRaw("\t" + ShadowVar + " = lerp(1.0, " + ShadowVar + ", " + Prefix + "_R.Weight);\n");
		AddRaw("}\n");
	}

	namespace
	{
		// The field is reached through the meshlet header, which only the surface pixel lane can resolve.
		bool RejectDistanceFieldNode(FMaterialCompiler& Compiler, CMaterialGraphNode* Node, const char* NodeName)
		{
			auto Fail = [&](const char* Reason)
			{
				EdNodeGraph::FError Error;
				Error.Node        = Node;
				Error.Name        = NodeName;
				Error.Description = Reason;
				Compiler.AddError(Error);
				return true;
			};

			if (Compiler.GetStage() != EMaterialCompileStage::Pixel)
			{
				return Fail("Mesh distance field nodes are pixel-stage; they cannot be reached from World Position Offset.");
			}

			if (Compiler.GetMaterialType() != EMaterialType::PBR)
			{
				return Fail("Mesh distance field nodes need a mesh instance; they are only available in Surface (PBR) materials.");
			}

			return false;
		}

		// Returns the variable-name prefix the caller appends to.
		FString EmitDistanceFieldPreamble(FMaterialCompiler& Compiler, CMaterialGraphNode* Node)
		{
			const FString Prefix = Compiler.GetCurrentInlinePrefix() + Node->GetNodeFullName();

			// Only the deferred template declares that local, and all three pixel templates share this chunk.
			Compiler.AddRaw("FGPUInstance " + Prefix + "_Inst = GetInstance(Input.InstanceIndex);\n");
			Compiler.AddRaw("float4x4 " + Prefix + "_M = " + Prefix + "_Inst.ModelMatrix;\n");
			Compiler.AddRaw("float4x4 " + Prefix + "_W2L = MakeInstanceWorldToLocal(" + Prefix + "_M);\n");
			Compiler.AddRaw("float3 " + Prefix + "_Scale = GetInstanceScale(" + Prefix + "_M);\n");
			Compiler.AddRaw("FDistanceFieldVolume " + Prefix + "_Vol = GetInstanceDistanceFieldVolume(" + Prefix + "_Inst);\n");

			return Prefix;
		}
	}

	void FMaterialCompiler::MeshDistanceField(CMaterialGraphNode* Node, CMaterialInput* Position,
	                                          CMaterialOutput* DistanceOut, CMaterialOutput* GradientOut,
	                                          CMaterialOutput* ValidOut)
	{
		const FString Prefix       = GetCurrentInlinePrefix() + Node->GetNodeFullName();
		const FString DistanceVar  = Prefix + "_Distance";
		const FString GradientVar  = Prefix + "_Gradient";
		const FString ValidVar     = Prefix + "_Valid";

		// Downstream nodes bind by ResolvedVar, and a large default reads as nothing nearby.
		AddRaw("float " + DistanceVar + " = 1e6;\n");
		AddRaw("float3 " + GradientVar + " = float3(0.0, 0.0, 1.0);\n");
		AddRaw("float " + ValidVar + " = 0.0;\n");

		if (DistanceOut) DistanceOut->ResolvedVar = DistanceVar;
		if (GradientOut) GradientOut->ResolvedVar = GradientVar;
		if (ValidOut)    ValidOut->ResolvedVar    = ValidVar;

		if (RejectDistanceFieldNode(*this, Node, "Mesh Distance Field"))
		{
			return;
		}

		FInputValue PositionValue = GetTypedInputValue(Position, "WorldPosition");
		const FString PositionStr = PositionValue.ComponentCount >= 3
		                          ? PositionValue.Value + ".xyz"
		                          : "float3(" + PositionValue.Value + ")";

		EmitDistanceFieldPreamble(*this, Node);

		AddRaw("FDistanceFieldQuery " + Prefix + "_Q = QueryDistanceField(" + Prefix + "_Vol, "
			+ Prefix + "_W2L, " + Prefix + "_Scale, " + PositionStr + ");\n");
		AddRaw(DistanceVar + " = " + Prefix + "_Q.Distance;\n");
		AddRaw(ValidVar + " = " + Prefix + "_Q.bValid ? 1.0 : 0.0;\n");
		// The gradient is zero on a saturated plateau, so keep the default and avoid a NaN downstream.
		AddRaw("if (any(" + Prefix + "_Q.Gradient != 0.0)) { " + GradientVar + " = " + Prefix + "_Q.Gradient; }\n");
	}

	void FMaterialCompiler::MeshDistanceFieldOcclusion(CMaterialGraphNode* Node, const FDistanceFieldOcclusionInputs& Inputs,
	                                                   int32 StepCount, CMaterialOutput* OcclusionOut)
	{
		const FString Prefix        = GetCurrentInlinePrefix() + Node->GetNodeFullName();
		const FString OcclusionVar  = Prefix + "_Occlusion";

		// 1 = fully unoccluded, which is the neutral value every AO consumer multiplies by.
		AddRaw("float " + OcclusionVar + " = 1.0;\n");
		if (OcclusionOut) OcclusionOut->ResolvedVar = OcclusionVar;

		if (RejectDistanceFieldNode(*this, Node, "Mesh Distance Field Occlusion"))
		{
			return;
		}

		// A masked material runs the whole pixel graph again in the pre-pass, so this is paid twice.
		if (IsMasked())
		{
			LOG_WARN("[Material] MeshDistanceFieldOcclusion in a MASKED material: the cone trace also runs in "
			         "the VisBuffer masked pre-pass, roughly doubling its cost.");
		}

		FInputValue NormalValue    = GetTypedInputValue(Inputs.Normal, "WorldNormal");
		FInputValue RadiusValue    = GetTypedInputValue(Inputs.Radius, 0.25f);
		FInputValue ConeValue      = GetTypedInputValue(Inputs.ConeAngle, 0.5f);
		FInputValue IntensityValue = GetTypedInputValue(Inputs.Intensity, 1.0f);

		const FString NormalStr = NormalValue.ComponentCount >= 3
		                        ? NormalValue.Value + ".xyz"
		                        : "float3(" + NormalValue.Value + ")";

		EmitDistanceFieldPreamble(*this, Node);

		AddRaw("if (" + Prefix + "_Vol.IsValid())\n");
		AddRaw("{\n");

		// A normal transforms by the transpose of object to world, whose rows are the model columns.
		AddRaw("\tfloat3 " + Prefix + "_LP = mul(" + Prefix + "_W2L, float4(WorldPosition, 1.0)).xyz;\n");
		AddRaw("\tfloat3 " + Prefix + "_NW = normalize(" + NormalStr + ");\n");
		AddRaw("\tfloat3 " + Prefix + "_LN = normalize(float3("
			"dot(float3(" + Prefix + "_M[0][0], " + Prefix + "_M[1][0], " + Prefix + "_M[2][0]), " + Prefix + "_NW), "
			"dot(float3(" + Prefix + "_M[0][1], " + Prefix + "_M[1][1], " + Prefix + "_M[2][1]), " + Prefix + "_NW), "
			"dot(float3(" + Prefix + "_M[0][2], " + Prefix + "_M[1][2], " + Prefix + "_M[2][2]), " + Prefix + "_NW)));\n");

		// Authored as a fraction of the volume extent, so one value works on a prop and a building alike.
		AddRaw("\tfloat " + Prefix + "_R = max(" + RadiusValue.Value + ", 0.0) * "
			"max(max(" + Prefix + "_Vol.VolumeSize.x, " + Prefix + "_Vol.VolumeSize.y), " + Prefix + "_Vol.VolumeSize.z);\n");

		// The field is 0 AT the surface, so a cone starting there reads full occlusion and never recovers.
		AddRaw("\tfloat3 " + Prefix + "_Start = " + Prefix + "_LP + " + Prefix + "_LN * (" + Prefix + "_Vol.MaxDistance * 0.05);\n");

		AddRaw("\tfloat " + Prefix + "_Vis = DistanceFieldConeOcclusion(" + Prefix + "_Vol, " + Prefix + "_Start, "
			+ Prefix + "_LN, " + Prefix + "_R, max(" + ConeValue.Value + ", 0.01), " + Format("{}", StepCount) + ");\n");
		AddRaw("\t" + OcclusionVar + " = saturate(lerp(1.0, " + Prefix + "_Vis, saturate(" + IntensityValue.Value + ")));\n");
		AddRaw("}\n");
	}

	void FMaterialCompiler::MeshDistanceFieldThickness(CMaterialGraphNode* Node, CMaterialInput* Normal,
	                                                   CMaterialInput* MaxDistance, int32 StepCount,
	                                                   CMaterialOutput* ThicknessOut, CMaterialOutput* NormalizedOut)
	{
		const FString Prefix         = GetCurrentInlinePrefix() + Node->GetNodeFullName();
		const FString ThicknessVar   = Prefix + "_Thickness";
		const FString NormalizedVar  = Prefix + "_Normalized";

		AddRaw("float " + ThicknessVar + " = 0.0;\n");
		AddRaw("float " + NormalizedVar + " = 0.0;\n");

		if (ThicknessOut)  ThicknessOut->ResolvedVar  = ThicknessVar;
		if (NormalizedOut) NormalizedOut->ResolvedVar = NormalizedVar;

		if (RejectDistanceFieldNode(*this, Node, "Mesh Distance Field Thickness"))
		{
			return;
		}

		FInputValue NormalValue = GetTypedInputValue(Normal, "WorldNormal");
		FInputValue MaxValue    = GetTypedInputValue(MaxDistance, 0.5f);

		const FString NormalStr = NormalValue.ComponentCount >= 3
		                        ? NormalValue.Value + ".xyz"
		                        : "float3(" + NormalValue.Value + ")";

		EmitDistanceFieldPreamble(*this, Node);

		AddRaw("if (" + Prefix + "_Vol.IsValid() && !" + Prefix + "_Vol.bTwoSided)\n");
		AddRaw("{\n");
		AddRaw("\tfloat3 " + Prefix + "_LP = mul(" + Prefix + "_W2L, float4(WorldPosition, 1.0)).xyz;\n");
		AddRaw("\tfloat3 " + Prefix + "_NW = normalize(" + NormalStr + ");\n");
		AddRaw("\tfloat3 " + Prefix + "_LN = normalize(float3("
			"dot(float3(" + Prefix + "_M[0][0], " + Prefix + "_M[1][0], " + Prefix + "_M[2][0]), " + Prefix + "_NW), "
			"dot(float3(" + Prefix + "_M[0][1], " + Prefix + "_M[1][1], " + Prefix + "_M[2][1]), " + Prefix + "_NW), "
			"dot(float3(" + Prefix + "_M[0][2], " + Prefix + "_M[1][2], " + Prefix + "_M[2][2]), " + Prefix + "_NW)));\n");

		// Max March is a fraction of the volume extent, same convention as the occlusion radius.
		AddRaw("\tfloat " + Prefix + "_Max = max(" + MaxValue.Value + ", 0.0) * "
			"max(max(" + Prefix + "_Vol.VolumeSize.x, " + Prefix + "_Vol.VolumeSize.y), " + Prefix + "_Vol.VolumeSize.z);\n");

		AddRaw("\t" + ThicknessVar + " = DistanceFieldThickness(" + Prefix + "_Vol, " + Prefix + "_LP, "
			+ Prefix + "_LN, " + Prefix + "_Max, " + Prefix + "_Scale, " + Format("{}", StepCount) + ");\n");

		// Normalized in WORLD units, so the output is usable without the material knowing the mesh size.
		AddRaw("\t" + NormalizedVar + " = saturate(" + ThicknessVar + " / max(LocalDistanceToWorld("
			+ Prefix + "_Max, " + Prefix + "_Scale), 1e-4));\n");
		AddRaw("}\n");
	}

	void FMaterialCompiler::WindAnimation(CMaterialGraphNode* Node, const FWindInputs& Inputs, int32 Octaves,
	                                      bool bLODGate, CMaterialOutput* OffsetOut, CMaterialOutput* WeightOut,
	                                      CMaterialOutput* NoiseOut)
	{
		const FString Prefix    = GetCurrentInlinePrefix() + Node->GetNodeFullName();
		const FString OffsetVar = Prefix + "_Offset";
		const FString WeightVar = Prefix + "_Weight";
		const FString NoiseVar  = Prefix + "_Noise";

		// Downstream nodes bind by ResolvedVar, and zero offset and weight is exactly no wind.
		AddRaw("float3 " + OffsetVar + " = float3(0.0, 0.0, 0.0);\n");
		AddRaw("float " + WeightVar + " = 0.0;\n");
		AddRaw("float " + NoiseVar + " = 0.0;\n");

		if (OffsetOut) OffsetOut->ResolvedVar = OffsetVar;
		if (WeightOut) WeightOut->ResolvedVar = WeightVar;
		if (NoiseOut)  NoiseOut->ResolvedVar  = NoiseVar;

		// Only World Position Offset consumes this, so a pixel pin would burn the octave sum per pixel.
		if (CurrentStage != EMaterialCompileStage::Vertex)
		{
			EdNodeGraph::FError Error;
			Error.Node        = Node;
			Error.Name        = "Wind Animation";
			Error.Description = "WindAnimation is a vertex-stage node; connect its Offset to World Position Offset. "
			                    "It cannot be reached from the pixel outputs.";
			AddError(Error);
			return;
		}

		FInputValue PositionValue   = GetTypedInputValue(Inputs.Position, "WorldPosition");
		FInputValue DirectionValue  = GetTypedInputValue(Inputs.Direction, "float3(1.0, 0.0, 0.0)");
		FInputValue StrengthValue   = GetTypedInputValue(Inputs.Strength, 0.25f);
		FInputValue SpeedValue      = GetTypedInputValue(Inputs.Speed, 1.0f);
		FInputValue FrequencyValue  = GetTypedInputValue(Inputs.Frequency, 0.15f);
		FInputValue LacunarityValue = GetTypedInputValue(Inputs.Lacunarity, 2.1f);
		FInputValue GainValue       = GetTypedInputValue(Inputs.Gain, 0.5f);
		FInputValue MaskValue       = GetTypedInputValue(Inputs.Mask, 1.0f);
		FInputValue PhaseValue      = GetTypedInputValue(Inputs.Phase, 0.0f);
		FInputValue GustinessValue  = GetTypedInputValue(Inputs.Gustiness, 0.5f);
		FInputValue FadeStartValue  = GetTypedInputValue(Inputs.FadeStart, 150.0f);
		FInputValue FadeEndValue    = GetTypedInputValue(Inputs.FadeEnd, 300.0f);

		const FString PositionStr = PositionValue.ComponentCount >= 3
		                          ? PositionValue.Value + ".xyz"
		                          : "float3(" + PositionValue.Value + ")";
		const FString DirectionStr = DirectionValue.ComponentCount >= 3
		                           ? DirectionValue.Value + ".xyz"
		                           : "float3(" + DirectionValue.Value + ")";

		AddRaw("float3 " + Prefix + "_P = " + PositionStr + ";\n");

		// Off, the weight is a literal, so the distance test folds away instead of leaving a dead compare.
		const FString LODWeightStr = bLODGate
			? "WindLODWeight(" + Prefix + "_P, (" + FadeStartValue.Value + "), (" + FadeEndValue.Value + "))"
			: FString("1.0");

		AddRaw("FWindResult " + Prefix + "_W = ComputeWind(" + Prefix + "_P, " + DirectionStr + ", "
			+ "(" + StrengthValue.Value + "), (" + SpeedValue.Value + "), (" + FrequencyValue.Value + "), "
			+ "(" + LacunarityValue.Value + "), (" + GainValue.Value + "), " + Format("{}", Octaves) + ", "
			+ "(" + MaskValue.Value + "), (" + PhaseValue.Value + "), (" + GustinessValue.Value + "), "
			+ LODWeightStr + ");\n");

		AddRaw(OffsetVar + " = " + Prefix + "_W.Offset;\n");
		AddRaw(WeightVar + " = " + Prefix + "_W.Weight;\n");
		AddRaw(NoiseVar + " = " + Prefix + "_W.Noise;\n");
	}

	void FMaterialCompiler::PolarCoordinates(CMaterialInput* UV, CMaterialInput* Center)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue CenterValue = GetTypedInputValue(Center, "float2(0.5, 0.5)");

		GetActiveChunk().append("float2 " + OwningNode + "_D = " + UVValue.Value + " - " + CenterValue.Value + ";\n");
		GetActiveChunk().append("float2 " + OwningNode + " = float2(length(" + OwningNode + "_D), atan2(" + OwningNode + "_D.y, " + OwningNode + "_D.x) / 6.2831853 + 0.5);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float2);
	}

	void FMaterialCompiler::TwirlUV(CMaterialInput* UV, CMaterialInput* Center, CMaterialInput* Strength)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue CenterValue = GetTypedInputValue(Center, "float2(0.5, 0.5)");
		FInputValue StrengthValue = GetTypedInputValue(Strength, 1.0f);

		GetActiveChunk().append("float2 " + OwningNode + "_O = " + UVValue.Value + " - " + CenterValue.Value + ";\n");
		GetActiveChunk().append("float  " + OwningNode + "_R = length(" + OwningNode + "_O);\n");
		GetActiveChunk().append("float  " + OwningNode + "_A = atan2(" + OwningNode + "_O.y, " + OwningNode + "_O.x) + " + StrengthValue.Value + " * " + OwningNode + "_R;\n");
		GetActiveChunk().append("float2 " + OwningNode + " = " + CenterValue.Value + " + float2(cos(" + OwningNode + "_A), sin(" + OwningNode + "_A)) * " + OwningNode + "_R;\n");
		SetOwningOutputType(UV, EMaterialInputType::Float2);
	}

	void FMaterialCompiler::WorldPos(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "World Position"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 0.0, 0.0);\n");
			RegisterDeriv(ID, EDerivState::Zero);
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = WorldPosition;\n");

		// Every pixel template declares this pair, exact in the deferred lane and ddx/ddy in the forward ones.
		RegisterDeriv(ID, EDerivState::Valid, "WorldPosition_DDX", "WorldPosition_DDY", 3);
	}

	void FMaterialCompiler::CameraPos(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Camera Position"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 0.0, 0.0);\n");
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = GetCameraPosition();\n");
	}

	FString FMaterialCompiler::EmitInstanceModelMatrix(const FString& ID)
	{
		const FString MatrixVar = ID + "_M";

		// Only the deferred template declares an Inst local, so the pixel lane resolves from the index.
		const FString Source = (CurrentStage == EMaterialCompileStage::Vertex)
		                     ? FString("Inst.ModelMatrix")
		                     : FString("GetInstance(Input.InstanceIndex).ModelMatrix");

		// ObjectScale reads it three times, and each pixel-lane reference would re-fetch through a BDA load.
		GetActiveChunk().append("float4x4 " + MatrixVar + " = " + Source + ";\n");
		return MatrixVar;
	}

	void FMaterialCompiler::ObjectScale(const FString& ID, CMaterialGraphNode* Node)
	{
		// Only PBR surface passes carry the per-instance FGPUInstance; others get a neutral 1.
		if (RejectInUI(Node, "Object Scale") || CurrentMaterialType != EMaterialType::PBR)
		{
			GetActiveChunk().append("float3 " + ID + " = float3(1.0, 1.0, 1.0);\n");
			return;
		}

		const FString M = EmitInstanceModelMatrix(ID);

		// Scale = world-space length of each basis column of the object->world matrix.
		GetActiveChunk().append(
			"float3 " + ID + " = float3("
			"length(mul(" + M + ", float4(1.0, 0.0, 0.0, 0.0)).xyz), "
			"length(mul(" + M + ", float4(0.0, 1.0, 0.0, 0.0)).xyz), "
			"length(mul(" + M + ", float4(0.0, 0.0, 1.0, 0.0)).xyz));\n");
	}

	void FMaterialCompiler::ObjectPosition(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Object Position") || CurrentMaterialType != EMaterialType::PBR)
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 0.0, 0.0);\n");
			return;
		}

		const FString M = EmitInstanceModelMatrix(ID);
		GetActiveChunk().append("float3 " + ID + " = mul(" + M + ", float4(0.0, 0.0, 0.0, 1.0)).xyz;\n");
	}

	void FMaterialCompiler::EntityID(const FString& ID)
	{
		GetActiveChunk().append("float " + ID + " = float(EntityID);\n");
	}

	void FMaterialCompiler::Time(const FString& ID)
	{
		GetActiveChunk().append("float " + ID + " = GetTime();\n");
		// GetTime() is frame-uniform, so its screen-space derivative is zero.
		RegisterDeriv(ID, EDerivState::Zero);
	}

	void FMaterialCompiler::ScreenPosition(const FString& ID, bool bRaw)
	{
		if (bRaw)
		{
			GetActiveChunk().append("float2 " + ID + " = Input.Position.xy;\n");
		}
		else
		{
			GetActiveChunk().append("float2 " + ID + " = Input.Position.xy / max(float2(GetScreenSize()), float2(1.0, 1.0));\n");
		}
	}

	void FMaterialCompiler::ViewDirection(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "View Direction"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 0.0, 1.0);\n");
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = normalize(GetCameraPosition() - WorldPosition);\n");
	}

	void FMaterialCompiler::ReflectionVector(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Reflection Vector"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 0.0, 1.0);\n");
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = reflect(-normalize(GetCameraPosition() - WorldPosition), normalize(WorldNormal.xyz));\n");
	}

	void FMaterialCompiler::FragmentDepth(const FString& ID, bool bLinear, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Fragment Depth"))
		{
			GetActiveChunk().append("float " + ID + " = 0.0;\n");
			return;
		}
		if (bLinear)
		{
			GetActiveChunk().append("float " + ID + " = abs(ViewPosition.z);\n");
		}
		else
		{
			GetActiveChunk().append("float " + ID + " = Input.Position.z;\n");
		}
	}

	void FMaterialCompiler::ViewportSize(const FString& ID)
	{
		GetActiveChunk().append("float2 " + ID + " = float2(GetScreenSize());\n");
	}

	void FMaterialCompiler::AspectRatio(const FString& ID)
	{
		GetActiveChunk().append("float " + ID + " = float(GetScreenSize().x) / max(float(GetScreenSize().y), 1.0);\n");
	}

	// Other domains have no such binding, so emit a graph error rather than a shader that fails to link.
	void FMaterialCompiler::SceneColor(const FString& ID, CMaterialInput* UV)
	{
		if (CurrentMaterialType != EMaterialType::PostProcess)
		{
			EdNodeGraph::FError Error;
			Error.Name        = "SceneColor";
			Error.Description = "SceneColor is only valid in materials with MaterialType = PostProcess.";
			Error.Node        = UV ? UV->GetOwningNode() : nullptr;
			Errors.push_back(Error);
			GetActiveChunk().append("float4 " + ID + " = float4(0.0, 0.0, 0.0, 1.0);\n");
			return;
		}

		FInputValue UVValue = GetTypedInputValue(UV, "UV0");
		FString UVStr = (UV && UV->HasConnection()) ? (UVValue.Value + ".xy") : FString("UV0");
		GetActiveChunk().append("float4 " + ID + " = uSceneColor.Sample(" + UVStr + ");\n");
	}

	void FMaterialCompiler::SceneDepth(const FString& ID, CMaterialInput* UV, bool bLinear)
	{
		if (CurrentMaterialType != EMaterialType::PostProcess)
		{
			EdNodeGraph::FError Error;
			Error.Name        = "SceneDepth";
			Error.Description = "SceneDepth is only valid in materials with MaterialType = PostProcess.";
			Error.Node        = UV ? UV->GetOwningNode() : nullptr;
			Errors.push_back(Error);
			GetActiveChunk().append("float " + ID + " = 1.0;\n");
			return;
		}

		FInputValue UVValue = GetTypedInputValue(UV, "UV0");
		FString UVStr = (UV && UV->HasConnection()) ? (UVValue.Value + ".xy") : FString("UV0");
		FString Raw = "uSceneDepth.Sample(" + UVStr + ").r";
		if (bLinear)
		{
			GetActiveChunk().append("float " + ID + " = LinearizeSceneDepth(" + Raw + ");\n");
		}
		else
		{
			GetActiveChunk().append("float " + ID + " = " + Raw + ";\n");
		}
	}

	void FMaterialCompiler::SceneHDRColor(const FString& ID, CMaterialInput* UV)
	{
		if (CurrentMaterialType != EMaterialType::PostProcess)
		{
			EdNodeGraph::FError Error;
			Error.Name        = "SceneHDRColor";
			Error.Description = "SceneHDRColor is only valid in materials with MaterialType = PostProcess.";
			Error.Node        = UV ? UV->GetOwningNode() : nullptr;
			Errors.push_back(Error);
			GetActiveChunk().append("float4 " + ID + " = float4(0.0, 0.0, 0.0, 1.0);\n");
			return;
		}

		FInputValue UVValue = GetTypedInputValue(UV, "UV0");
		FString UVStr = (UV && UV->HasConnection()) ? (UVValue.Value + ".xy") : FString("UV0");
		GetActiveChunk().append("float4 " + ID + " = uHDRSceneColor.Sample(" + UVStr + ");\n");
	}

	void FMaterialCompiler::NumericConstant(const FString& ID, float Value)
	{
		GetActiveChunk().append("float " + ID + " = " + Format("{}", Value) + ";\n");
	}

	void FMaterialCompiler::CustomPrimitiveData(CMaterialExpression_CustomPrimitiveData* Node, ECustomPrimitiveDataType Type)
	{
		Node->Output->SetInputType(EMaterialInputType::Float);

		switch (Type)
		{
		case ECustomPrimitiveDataType::Float:
			GetActiveChunk().append("float " + Node->GetNodeFullName() + " = Cull.CustomData.AsFloat;\n");
			break;
		case ECustomPrimitiveDataType::Int:
			GetActiveChunk().append("int " + Node->GetNodeFullName() + " = Cull.CustomData.AsInt;\n");
			break;
		case ECustomPrimitiveDataType::UInt:
			GetActiveChunk().append("uint " + Node->GetNodeFullName() + " = Cull.CustomData.AsUInt;\n");
			break;
		case ECustomPrimitiveDataType::Color:
			GetActiveChunk().append("float4 " + Node->GetNodeFullName() + " = Cull.CustomData.AsColor;\n");
			Node->Output->SetInputType(EMaterialInputType::Float4);
			Node->Output->SetComponentMask(EComponentMask::RGBA);
			break;
		case ECustomPrimitiveDataType::Bool:
			GetActiveChunk().append("bool " + Node->GetNodeFullName() + " = Cull.CustomData.AsBool;\n");
			break;
		}
	}

	// Math Operations - binary

	void FMaterialCompiler::Multiply(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		Node->Output->InputType = EmitBinaryOp("*", A, B, Node->ConstA, Node->ConstB, true);
	}

	void FMaterialCompiler::Divide(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		Node->Output->InputType = EmitBinaryOp("/", A, B, Node->ConstA, Node->ConstB, true);
	}

	void FMaterialCompiler::Add(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		Node->Output->InputType = EmitBinaryOp("+", A, B, Node->ConstA, Node->ConstB, true);
	}

	void FMaterialCompiler::Subtract(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		Node->Output->InputType = EmitBinaryOp("-", A, B, Node->ConstA, Node->ConstB, true);
	}

	void FMaterialCompiler::Power(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		EmitBinaryFunc("pow", A, B, Node->ConstA, Node->ConstB);
	}

	void FMaterialCompiler::Mod(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		EmitBinaryFunc("fmod", A, B, Node->ConstA, Node->ConstB);
	}

	void FMaterialCompiler::Min(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		EmitBinaryFunc("min", A, B, Node->ConstA, Node->ConstB);
	}

	void FMaterialCompiler::Max(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		EmitBinaryFunc("max", A, B, Node->ConstA, Node->ConstB);
	}

	void FMaterialCompiler::Step(CMaterialInput* A, CMaterialInput* B)
	{
		CMaterialExpression_Math* Node = A->GetOwningNode<CMaterialExpression_Math>();
		EmitBinaryFunc("step", A, B, Node->ConstA, Node->ConstB);
	}

	void FMaterialCompiler::Atan2Op(CMaterialInput* Y, CMaterialInput* X)
	{
		CMaterialExpression_Math* Node = Y->GetOwningNode<CMaterialExpression_Math>();
		EmitBinaryFunc("atan2", Y, X, Node->ConstA, Node->ConstB);
	}

	// Math Operations - unary

	void FMaterialCompiler::Sin(CMaterialInput* A)        { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("sin", A, N->ConstA); }
	void FMaterialCompiler::Cos(CMaterialInput* A)        { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("cos", A, N->ConstA); }
	void FMaterialCompiler::Tan(CMaterialInput* A)        { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("tan", A, N->ConstA); }
	void FMaterialCompiler::Asin(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("asin", A, N->ConstA); }
	void FMaterialCompiler::Acos(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("acos", A, N->ConstA); }
	void FMaterialCompiler::Atan(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("atan", A, N->ConstA); }
	void FMaterialCompiler::Sinh(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("sinh", A, N->ConstA); }
	void FMaterialCompiler::Cosh(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("cosh", A, N->ConstA); }
	void FMaterialCompiler::Tanh(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("tanh", A, N->ConstA); }
	void FMaterialCompiler::Sqrt(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("sqrt", A, N->ConstA); }
	void FMaterialCompiler::Rsqrt(CMaterialInput* A)      { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("rsqrt", A, N->ConstA); }
	void FMaterialCompiler::Log(CMaterialInput* A)        { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("log", A, N->ConstA); }
	void FMaterialCompiler::Log2(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("log2", A, N->ConstA); }
	void FMaterialCompiler::Log10(CMaterialInput* A)      { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("log10", A, N->ConstA); }
	void FMaterialCompiler::Exp(CMaterialInput* A)        { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("exp", A, N->ConstA); }
	void FMaterialCompiler::Exp2(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("exp2", A, N->ConstA); }
	void FMaterialCompiler::Sign(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("sign", A, N->ConstA); }
	void FMaterialCompiler::Round(CMaterialInput* A)      { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("round", A, N->ConstA); }
	void FMaterialCompiler::Truncate(CMaterialInput* A)   { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("trunc", A, N->ConstA); }
	void FMaterialCompiler::Fract(CMaterialInput* A)      { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("frac", A, N->ConstA); }
	void FMaterialCompiler::Floor(CMaterialInput* A)      { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("floor", A, N->ConstA); }
	void FMaterialCompiler::Ceil(CMaterialInput* A)       { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("ceil", A, N->ConstA); }
	void FMaterialCompiler::Abs(CMaterialInput* A)        { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("abs", A, N->ConstA); }
	void FMaterialCompiler::Saturate(CMaterialInput* A)   { CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>(); EmitUnaryFunc("saturate", A, N->ConstA); }

	void FMaterialCompiler::OneMinus(CMaterialInput* A)
	{
		CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>();
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, N->ConstA);
		FString TypeStr = GetVectorType(AValue.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = 1.0 - " + AValue.Value + GetSwizzleForMask(AValue.Mask) + ";\n");

		// d(1 - A) = -dA. The constant contributes nothing.
		if (AValue.Deriv == EDerivState::Valid && AValue.ComponentCount <= 2)
		{
			RegisterDeriv(OwningNode, EDerivState::Valid, "-" + AValue.DDX, "-" + AValue.DDY);
		}
		else if (AValue.Deriv == EDerivState::Zero)
		{
			RegisterDeriv(OwningNode, EDerivState::Zero);
		}
		else
		{
			RegisterDeriv(OwningNode, EDerivState::Unknown);
		}

		SetOwningOutputType(A, AValue.Type);
	}

	void FMaterialCompiler::Reciprocal(CMaterialInput* A)
	{
		CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>();
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, N->ConstA);
		FString TypeStr = GetVectorType(AValue.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = 1.0 / max(" + AValue.Value + GetSwizzleForMask(AValue.Mask) + ", " + TypeStr + "(1e-6));\n");
		SetOwningOutputType(A, AValue.Type);
	}

	void FMaterialCompiler::Negate(CMaterialInput* A)
	{
		CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>();
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, N->ConstA);
		FString TypeStr = GetVectorType(AValue.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = -(" + AValue.Value + GetSwizzleForMask(AValue.Mask) + ");\n");
		SetOwningOutputType(A, AValue.Type);
	}

	void FMaterialCompiler::Square(CMaterialInput* A)
	{
		CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>();
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, N->ConstA);
		FString TypeStr = GetVectorType(AValue.Type);
		FString V = AValue.Value + GetSwizzleForMask(AValue.Mask);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = (" + V + ") * (" + V + ");\n");
		SetOwningOutputType(A, AValue.Type);
	}

	void FMaterialCompiler::DegreesToRadians(CMaterialInput* A)
	{
		CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>();
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, N->ConstA);
		FString TypeStr = GetVectorType(AValue.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = (" + AValue.Value + GetSwizzleForMask(AValue.Mask) + ") * 0.01745329252;\n");
		SetOwningOutputType(A, AValue.Type);
	}

	void FMaterialCompiler::RadiansToDegrees(CMaterialInput* A)
	{
		CMaterialExpression_Math* N = A->GetOwningNode<CMaterialExpression_Math>();
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, N->ConstA);
		FString TypeStr = GetVectorType(AValue.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = (" + AValue.Value + GetSwizzleForMask(AValue.Mask) + ") * 57.29577951;\n");
		SetOwningOutputType(A, AValue.Type);
	}

	// Math Operations - ternary

	void FMaterialCompiler::Lerp(CMaterialInput* A, CMaterialInput* B, CMaterialInput* C)
	{
		CMaterialExpression_Lerp* Node = A->GetOwningNode<CMaterialExpression_Lerp>();
		EmitTernaryFunc("lerp", A, B, C, Node->ConstA, Node->ConstB, Node->Alpha);
	}

	void FMaterialCompiler::Clamp(CMaterialInput* A, CMaterialInput* B, CMaterialInput* C)
	{
		CMaterialExpression_Clamp* Node = A->GetOwningNode<CMaterialExpression_Clamp>();
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();

		FInputValue XValue = GetTypedInputValue(C, "1.0");
		FInputValue AValue = GetTypedInputValue(A, Node->ConstA);
		FInputValue BValue = GetTypedInputValue(B, Node->ConstB);

		EMaterialInputType ResultType = DetermineResultType(AValue.Type, BValue.Type, true);
		ResultType = DetermineResultType(ResultType, XValue.Type, true);
		FString TypeStr = GetVectorType(ResultType);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = clamp(" + XValue.Value + ", " + AValue.Value + ", " + BValue.Value + ");\n");
		Node->Output->SetInputType(ResultType);
	}

	void FMaterialCompiler::SmoothStep(CMaterialInput* A, CMaterialInput* B, CMaterialInput* C)
	{
		CMaterialExpression_SmoothStep* Node = A->GetOwningNode<CMaterialExpression_SmoothStep>();
		EmitTernaryFunc("smoothstep", A, B, C, Node->ConstA, Node->ConstB, Node->X);
	}

	void FMaterialCompiler::Remap(CMaterialInput* X, CMaterialInput* InMin, CMaterialInput* InMax, CMaterialInput* OutMin, CMaterialInput* OutMax)
	{
		FString OwningNode = X->GetOwningNode()->GetNodeFullName();
		FInputValue XV = GetTypedInputValue(X, 0.5f);
		FInputValue InMinV = GetTypedInputValue(InMin, 0.0f);
		FInputValue InMaxV = GetTypedInputValue(InMax, 1.0f);
		FInputValue OutMinV = GetTypedInputValue(OutMin, 0.0f);
		FInputValue OutMaxV = GetTypedInputValue(OutMax, 1.0f);

		EMaterialInputType ResultType = DetermineResultType(XV.Type, OutMaxV.Type, true);
		FString TypeStr = GetVectorType(ResultType);

		GetActiveChunk().append(TypeStr + " " + OwningNode + " = " + OutMinV.Value + " + (" + XV.Value + " - " + InMinV.Value + ") * (" + OutMaxV.Value + " - " + OutMinV.Value + ") / max(" + InMaxV.Value + " - " + InMinV.Value + ", 1e-6);\n");
		SetOwningOutputType(X, ResultType);
	}

	// Vector operations

	void FMaterialCompiler::Normalize(CMaterialInput* A)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();

		FInputValue AValue = GetTypedInputValue(A, "float3(0.0, 0.0, 1.0)");

		if (AValue.ComponentCount < 2)
		{
			EdNodeGraph::FError Error;
			Error.Name = "Invalid Type";
			Error.Description = "Normalize requires at least a float2 input";
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			AddError(Error);

			AValue.Value = "float3(0.0, 0.0, 1.0)";
			AValue.Type = EMaterialInputType::Float3;
			AValue.ComponentCount = 3;
		}

		FString TypeStr = GetVectorType(AValue.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = normalize(" + AValue.Value + ");\n");
		SetOwningOutputType(A, AValue.Type);
	}

	void FMaterialCompiler::Distance(CMaterialInput* A, CMaterialInput* B)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();

		FInputValue AValue = GetTypedInputValue(A, "0.0");
		FInputValue BValue = GetTypedInputValue(B, "0.0");

		if (AValue.ComponentCount != BValue.ComponentCount)
		{
			EdNodeGraph::FError Error;
			Error.Name = "Type Mismatch";
			Error.Description = "Distance requires vectors of the same dimension";
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			AddError(Error);
		}

		GetActiveChunk().append("float " + OwningNode + " = distance(" + AValue.Value + ", " + BValue.Value + ");\n");
		SetOwningOutputType(A, EMaterialInputType::Float);
	}

	void FMaterialCompiler::Length(CMaterialInput* A)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, "0.0");
		GetActiveChunk().append("float " + OwningNode + " = length(" + AValue.Value + ");\n");
		SetOwningOutputType(A, EMaterialInputType::Float);
	}

	void FMaterialCompiler::Dot(CMaterialInput* A, CMaterialInput* B)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, "0.0");
		FInputValue BValue = GetTypedInputValue(B, "0.0");

		if (AValue.ComponentCount != BValue.ComponentCount)
		{
			EdNodeGraph::FError Error;
			Error.Name = "Type Mismatch";
			Error.Description = "Dot product requires vectors of the same dimension.";
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			AddError(Error);
		}

		GetActiveChunk().append("float " + OwningNode + " = dot(" + AValue.Value + ", " + BValue.Value + ");\n");
		SetOwningOutputType(A, EMaterialInputType::Float);
	}

	void FMaterialCompiler::Cross(CMaterialInput* A, CMaterialInput* B)
	{
		FString OwningNode = A->GetOwningNode()->GetNodeFullName();
		FInputValue AValue = GetTypedInputValue(A, "float3(1.0, 0.0, 0.0)");
		FInputValue BValue = GetTypedInputValue(B, "float3(0.0, 1.0, 0.0)");

		GetActiveChunk().append("float3 " + OwningNode + " = cross(" + AValue.Value + ".xyz, " + BValue.Value + ".xyz);\n");
		SetOwningOutputType(A, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::Reflect(CMaterialInput* I, CMaterialInput* N)
	{
		FString OwningNode = I->GetOwningNode()->GetNodeFullName();
		FInputValue IV = GetTypedInputValue(I, "float3(0.0, 0.0, -1.0)");
		FInputValue NV = GetTypedInputValue(N, "float3(0.0, 0.0, 1.0)");
		GetActiveChunk().append("float3 " + OwningNode + " = reflect(" + IV.Value + ".xyz, normalize(" + NV.Value + ".xyz));\n");
		SetOwningOutputType(I, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::Refract(CMaterialInput* I, CMaterialInput* N, CMaterialInput* Eta)
	{
		FString OwningNode = I->GetOwningNode()->GetNodeFullName();
		FInputValue IV = GetTypedInputValue(I, "float3(0.0, 0.0, -1.0)");
		FInputValue NV = GetTypedInputValue(N, "float3(0.0, 0.0, 1.0)");
		FInputValue EtaV = GetTypedInputValue(Eta, 1.0f);
		GetActiveChunk().append("float3 " + OwningNode + " = refract(" + IV.Value + ".xyz, normalize(" + NV.Value + ".xyz), " + EtaV.Value + ");\n");
		SetOwningOutputType(I, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::RotateAboutAxis(CMaterialInput* Position, CMaterialInput* Axis, CMaterialInput* Angle, CMaterialInput* Pivot)
	{
		FString OwningNode = Position->GetOwningNode()->GetNodeFullName();

		FInputValue PV = GetTypedInputValue(Position, "float3(0.0, 0.0, 0.0)");
		FInputValue AV = GetTypedInputValue(Axis, "float3(0.0, 0.0, 1.0)");
		FInputValue AngleV = GetTypedInputValue(Angle, 0.0f);
		FInputValue PivotV = GetTypedInputValue(Pivot, "float3(0.0, 0.0, 0.0)");

		GetActiveChunk().append("float3 " + OwningNode + "_K = normalize(" + AV.Value + ".xyz);\n");
		GetActiveChunk().append("float  " + OwningNode + "_S = sin(" + AngleV.Value + ");\n");
		GetActiveChunk().append("float  " + OwningNode + "_C = cos(" + AngleV.Value + ");\n");

		// translate to pivot space
		GetActiveChunk().append("float3 " + OwningNode + "_V = " + PV.Value + ".xyz - " + PivotV.Value + ".xyz;\n");

		// rotate
		GetActiveChunk().append(
			"float3 " + OwningNode + "_R = " + OwningNode + "_V * " + OwningNode + "_C + "
			"cross(" + OwningNode + "_K, " + OwningNode + "_V) * " + OwningNode + "_S + "
			+ OwningNode + "_K * dot(" + OwningNode + "_K, " + OwningNode + "_V) * (1.0 - " + OwningNode + "_C);\n"
		);

		// translate back
		GetActiveChunk().append("float3 " + OwningNode + " = " + OwningNode + "_R + " + PivotV.Value + ".xyz;\n");

		SetOwningOutputType(Position, EMaterialInputType::Float3);
	}

	// Color

	void FMaterialCompiler::Luminance(CMaterialInput* Color)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, "float3(1.0, 1.0, 1.0)");
		GetActiveChunk().append("float " + OwningNode + " = dot(" + C.Value + ".rgb, float3(0.2126, 0.7152, 0.0722));\n");
		SetOwningOutputType(Color, EMaterialInputType::Float);
	}

	void FMaterialCompiler::Desaturate(CMaterialInput* Color, CMaterialInput* Amount)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, "float3(1.0, 1.0, 1.0)");
		FInputValue A = GetTypedInputValue(Amount, 1.0f);
		GetActiveChunk().append("float  " + OwningNode + "_L = dot(" + C.Value + ".rgb, float3(0.2126, 0.7152, 0.0722));\n");
		GetActiveChunk().append("float3 " + OwningNode + " = lerp(" + C.Value + ".rgb, float3(" + OwningNode + "_L, " + OwningNode + "_L, " + OwningNode + "_L), saturate(" + A.Value + "));\n");
		SetOwningOutputType(Color, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::RGBToHSV(CMaterialInput* RGB)
	{
		FString OwningNode = RGB->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(RGB, "float3(1.0, 1.0, 1.0)");
		FString In = C.Value + ".rgb";
		GetActiveChunk().append("float4 " + OwningNode + "_K = float4(0.0, -1.0/3.0, 2.0/3.0, -1.0);\n");
		GetActiveChunk().append("float4 " + OwningNode + "_P = lerp(float4((" + In + ").bg, " + OwningNode + "_K.wz), float4((" + In + ").gb, " + OwningNode + "_K.xy), step((" + In + ").b, (" + In + ").g));\n");
		GetActiveChunk().append("float4 " + OwningNode + "_Q = lerp(float4(" + OwningNode + "_P.xyw, (" + In + ").r), float4((" + In + ").r, " + OwningNode + "_P.yzx), step(" + OwningNode + "_P.x, (" + In + ").r));\n");
		GetActiveChunk().append("float  " + OwningNode + "_D = " + OwningNode + "_Q.x - min(" + OwningNode + "_Q.w, " + OwningNode + "_Q.y);\n");
		GetActiveChunk().append("float3 " + OwningNode + " = float3(abs(" + OwningNode + "_Q.z + (" + OwningNode + "_Q.w - " + OwningNode + "_Q.y) / (6.0 * " + OwningNode + "_D + 1e-10)), " + OwningNode + "_D / max(" + OwningNode + "_Q.x, 1e-10), " + OwningNode + "_Q.x);\n");
		SetOwningOutputType(RGB, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::HSVToRGB(CMaterialInput* HSV)
	{
		FString OwningNode = HSV->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(HSV, "float3(0.0, 0.0, 1.0)");
		FString In = C.Value + ".xyz";
		GetActiveChunk().append("float3 " + OwningNode + "_P = abs(frac(" + In + ".xxx + float3(1.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);\n");
		GetActiveChunk().append("float3 " + OwningNode + " = " + In + ".z * lerp(float3(1.0, 1.0, 1.0), saturate(" + OwningNode + "_P - 1.0), " + In + ".y);\n");
		SetOwningOutputType(HSV, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::Posterize(CMaterialInput* Color, CMaterialInput* Steps)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, 0.5f);
		FInputValue S = GetTypedInputValue(Steps, 4.0f);
		FString TypeStr = GetVectorType(C.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = floor((" + C.Value + ") * max(" + S.Value + ", 1.0)) / max(" + S.Value + ", 1.0);\n");
		SetOwningOutputType(Color, C.Type);
	}

	void FMaterialCompiler::GammaCorrection(CMaterialInput* Color, CMaterialInput* Gamma)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, 1.0f);
		FInputValue G = GetTypedInputValue(Gamma, 2.2f);
		FString TypeStr = GetVectorType(C.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = pow(max(" + C.Value + ", " + TypeStr + "(0.0)), " + TypeStr + "(" + G.Value + "));\n");
		SetOwningOutputType(Color, C.Type);
	}

	void FMaterialCompiler::Contrast(CMaterialInput* Color, CMaterialInput* Amount)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, 0.5f);
		FInputValue A = GetTypedInputValue(Amount, 1.0f);
		FString TypeStr = GetVectorType(C.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = (" + C.Value + " - " + TypeStr + "(0.5)) * " + A.Value + " + " + TypeStr + "(0.5);\n");
		SetOwningOutputType(Color, C.Type);
	}

	void FMaterialCompiler::Brightness(CMaterialInput* Color, CMaterialInput* Amount)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, 0.5f);
		FInputValue A = GetTypedInputValue(Amount, 1.0f);
		FString TypeStr = GetVectorType(C.Type);
		GetActiveChunk().append(TypeStr + " " + OwningNode + " = (" + C.Value + ") * (" + A.Value + ");\n");
		SetOwningOutputType(Color, C.Type);
	}

	void FMaterialCompiler::Tint(CMaterialInput* Color, CMaterialInput* TintColor, CMaterialInput* Amount)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, "float3(1.0, 1.0, 1.0)");
		FInputValue T = GetTypedInputValue(TintColor, "float3(1.0, 1.0, 1.0)");
		FInputValue A = GetTypedInputValue(Amount, 1.0f);
		GetActiveChunk().append("float3 " + OwningNode + " = lerp(" + C.Value + ".rgb, (" + C.Value + ".rgb) * (" + T.Value + ".rgb), saturate(" + A.Value + "));\n");
		SetOwningOutputType(Color, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::LinearToSRGB(CMaterialInput* Color)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, "float3(1.0, 1.0, 1.0)");
		GetActiveChunk().append("float3 " + OwningNode + " = pow(max(" + C.Value + ".rgb, float3(0.0, 0.0, 0.0)), float3(1.0/2.2, 1.0/2.2, 1.0/2.2));\n");
		SetOwningOutputType(Color, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::SRGBToLinear(CMaterialInput* Color)
	{
		FString OwningNode = Color->GetOwningNode()->GetNodeFullName();
		FInputValue C = GetTypedInputValue(Color, "float3(1.0, 1.0, 1.0)");
		GetActiveChunk().append("float3 " + OwningNode + " = pow(max(" + C.Value + ".rgb, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2));\n");
		SetOwningOutputType(Color, EMaterialInputType::Float3);
	}

	// Noise / procedural

	void FMaterialCompiler::Hash11(CMaterialInput* X)
	{
		FString OwningNode = X->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(X, 0.0f);
		GetActiveChunk().append("float " + OwningNode + " = frac(sin((" + V.Value + ")) * 43758.5453);\n");
		SetOwningOutputType(X, EMaterialInputType::Float);
	}

	void FMaterialCompiler::Hash21(CMaterialInput* UV)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(UV, "float2(0.0, 0.0)");
		GetActiveChunk().append("float " + OwningNode + " = frac(sin(dot((" + V.Value + ").xy, float2(127.1, 311.7))) * 43758.5453);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float);
	}

	void FMaterialCompiler::Hash22(CMaterialInput* UV)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(UV, "float2(0.0, 0.0)");
		GetActiveChunk().append("float2 " + OwningNode + " = frac(sin(float2(dot((" + V.Value + ").xy, float2(127.1, 311.7)), dot((" + V.Value + ").xy, float2(269.5, 183.3)))) * 43758.5453);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float2);
	}

	void FMaterialCompiler::Hash33(CMaterialInput* P)
	{
		FString OwningNode = P->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(P, "float3(0.0, 0.0, 0.0)");
		GetActiveChunk().append("float3 " + OwningNode + " = frac(sin(float3(dot((" + V.Value + ").xyz, float3(127.1, 311.7, 74.7)), dot((" + V.Value + ").xyz, float3(269.5, 183.3, 246.1)), dot((" + V.Value + ").xyz, float3(113.5, 271.9, 124.6)))) * 43758.5453);\n");
		SetOwningOutputType(P, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::ValueNoise(CMaterialInput* UV)
	{
		FString N = UV->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(UV, "float2(UV0)");
		FString In = "(" + V.Value + ").xy";
		GetActiveChunk().append("float2 " + N + "_I = floor(" + In + ");\n");
		GetActiveChunk().append("float2 " + N + "_F = frac(" + In + ");\n");
		GetActiveChunk().append("float2 " + N + "_U = " + N + "_F * " + N + "_F * (3.0 - 2.0 * " + N + "_F);\n");
		GetActiveChunk().append("float  " + N + "_A = frac(sin(dot(" + N + "_I + float2(0.0, 0.0), float2(127.1, 311.7))) * 43758.5453);\n");
		GetActiveChunk().append("float  " + N + "_B = frac(sin(dot(" + N + "_I + float2(1.0, 0.0), float2(127.1, 311.7))) * 43758.5453);\n");
		GetActiveChunk().append("float  " + N + "_C = frac(sin(dot(" + N + "_I + float2(0.0, 1.0), float2(127.1, 311.7))) * 43758.5453);\n");
		GetActiveChunk().append("float  " + N + "_D = frac(sin(dot(" + N + "_I + float2(1.0, 1.0), float2(127.1, 311.7))) * 43758.5453);\n");
		GetActiveChunk().append("float " + N + " = lerp(lerp(" + N + "_A, " + N + "_B, " + N + "_U.x), lerp(" + N + "_C, " + N + "_D, " + N + "_U.x), " + N + "_U.y);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float);
	}

	void FMaterialCompiler::GradientNoise(CMaterialInput* UV)
	{
		FString N = UV->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(UV, "float2(UV0)");
		FString In = "(" + V.Value + ").xy";
		GetActiveChunk().append("float2 " + N + "_I = floor(" + In + ");\n");
		GetActiveChunk().append("float2 " + N + "_F = frac(" + In + ");\n");
		GetActiveChunk().append("float2 " + N + "_U = " + N + "_F * " + N + "_F * (3.0 - 2.0 * " + N + "_F);\n");
		GetActiveChunk().append("float2 " + N + "_GA = -1.0 + 2.0 * frac(sin(float2(dot(" + N + "_I + float2(0.0, 0.0), float2(127.1, 311.7)), dot(" + N + "_I + float2(0.0, 0.0), float2(269.5, 183.3)))) * 43758.5453);\n");
		GetActiveChunk().append("float2 " + N + "_GB = -1.0 + 2.0 * frac(sin(float2(dot(" + N + "_I + float2(1.0, 0.0), float2(127.1, 311.7)), dot(" + N + "_I + float2(1.0, 0.0), float2(269.5, 183.3)))) * 43758.5453);\n");
		GetActiveChunk().append("float2 " + N + "_GC = -1.0 + 2.0 * frac(sin(float2(dot(" + N + "_I + float2(0.0, 1.0), float2(127.1, 311.7)), dot(" + N + "_I + float2(0.0, 1.0), float2(269.5, 183.3)))) * 43758.5453);\n");
		GetActiveChunk().append("float2 " + N + "_GD = -1.0 + 2.0 * frac(sin(float2(dot(" + N + "_I + float2(1.0, 1.0), float2(127.1, 311.7)), dot(" + N + "_I + float2(1.0, 1.0), float2(269.5, 183.3)))) * 43758.5453);\n");
		GetActiveChunk().append("float " + N + " = lerp(lerp(dot(" + N + "_GA, " + N + "_F - float2(0.0, 0.0)), dot(" + N + "_GB, " + N + "_F - float2(1.0, 0.0)), " + N + "_U.x), lerp(dot(" + N + "_GC, " + N + "_F - float2(0.0, 1.0)), dot(" + N + "_GD, " + N + "_F - float2(1.0, 1.0)), " + N + "_U.x), " + N + "_U.y) * 0.5 + 0.5;\n");
		SetOwningOutputType(UV, EMaterialInputType::Float);
	}

	void FMaterialCompiler::PerlinNoise(CMaterialInput* UV)
	{
		// Use the gradient-noise variant which is closer to classic Perlin output.
		GradientNoise(UV);
	}

	void FMaterialCompiler::VoronoiNoise(CMaterialInput* UV)
	{
		FString N = UV->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(UV, "float2(UV0)");
		FString In = "(" + V.Value + ").xy";
		GetActiveChunk().append("float2 " + N + "_I = floor(" + In + ");\n");
		GetActiveChunk().append("float2 " + N + "_F = frac(" + In + ");\n");
		GetActiveChunk().append("float  " + N + "_M = 8.0;\n");
		GetActiveChunk().append("for (int " + N + "_y = -1; " + N + "_y <= 1; ++" + N + "_y)\n");
		GetActiveChunk().append("for (int " + N + "_x = -1; " + N + "_x <= 1; ++" + N + "_x)\n");
		GetActiveChunk().append("{\n");
		GetActiveChunk().append("    float2 " + N + "_G = float2(float(" + N + "_x), float(" + N + "_y));\n");
		GetActiveChunk().append("    float2 " + N + "_O = frac(sin(float2(dot(" + N + "_I + " + N + "_G, float2(127.1, 311.7)), dot(" + N + "_I + " + N + "_G, float2(269.5, 183.3)))) * 43758.5453);\n");
		GetActiveChunk().append("    float2 " + N + "_R = " + N + "_G + " + N + "_O - " + N + "_F;\n");
		GetActiveChunk().append("    float  " + N + "_DD = dot(" + N + "_R, " + N + "_R);\n");
		GetActiveChunk().append("    " + N + "_M = min(" + N + "_M, " + N + "_DD);\n");
		GetActiveChunk().append("}\n");
		GetActiveChunk().append("float " + N + " = sqrt(" + N + "_M);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float);
	}

	void FMaterialCompiler::SimpleNoise(CMaterialInput* UV)
	{
		FString N = UV->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(UV, "float2(UV0)");
		GetActiveChunk().append("float " + N + " = frac(sin(dot((" + V.Value + ").xy, float2(12.9898, 78.233))) * 43758.5453);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float);
	}

	void FMaterialCompiler::Checkerboard(CMaterialInput* UV)
	{
		FString N = UV->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(UV, "float2(UV0)");
		GetActiveChunk().append("float2 " + N + "_C = floor((" + V.Value + ").xy);\n");
		GetActiveChunk().append("float " + N + " = fmod(" + N + "_C.x + " + N + "_C.y, 2.0);\n");
		SetOwningOutputType(UV, EMaterialInputType::Float);
	}

	// Conditional

	void FMaterialCompiler::If(CMaterialInput* X, CMaterialInput* Y, CMaterialInput* GreaterThan, CMaterialInput* EqualTo, CMaterialInput* LessThan, float Threshold)
	{
		FString N = X->GetOwningNode()->GetNodeFullName();
		FInputValue XV = GetTypedInputValue(X, 0.0f);
		FInputValue YV = GetTypedInputValue(Y, 0.0f);
		FInputValue GV = GetTypedInputValue(GreaterThan, 1.0f);
		FInputValue EV = GetTypedInputValue(EqualTo, 0.5f);
		FInputValue LV = GetTypedInputValue(LessThan, 0.0f);

		EMaterialInputType ResultType = DetermineResultType(GV.Type, LV.Type, true);
		ResultType = DetermineResultType(ResultType, EV.Type, true);
		FString TypeStr = GetVectorType(ResultType);

		GetActiveChunk().append("float " + N + "_Diff = (" + XV.Value + ") - (" + YV.Value + ");\n");
		GetActiveChunk().append(TypeStr + " " + N + " = (abs(" + N + "_Diff) < " + Format("{}", Threshold) + ") ? (" + EV.Value + ") : ((" + N + "_Diff > 0.0) ? (" + GV.Value + ") : (" + LV.Value + "));\n");
		SetOwningOutputType(X, ResultType);
	}

	void FMaterialCompiler::Compare(const FString& Op, CMaterialInput* A, CMaterialInput* B)
	{
		FString N = A->GetOwningNode()->GetNodeFullName();
		FInputValue AV = GetTypedInputValue(A, 0.0f);
		FInputValue BV = GetTypedInputValue(B, 0.0f);
		GetActiveChunk().append("float " + N + " = ((" + AV.Value + ") " + Op + " (" + BV.Value + ")) ? 1.0 : 0.0;\n");
		SetOwningOutputType(A, EMaterialInputType::Float);
	}

	// Advanced shading helpers

	void FMaterialCompiler::Fresnel(CMaterialInput* Exponent, CMaterialInput* BaseReflect, CMaterialInput* Normal)
	{
		FString N = Exponent->GetOwningNode()->GetNodeFullName();
		FInputValue ExpV = GetTypedInputValue(Exponent, 5.0f);
		FInputValue BaseV = GetTypedInputValue(BaseReflect, 0.04f);
		FInputValue NV = GetTypedInputValue(Normal, "WorldNormal.xyz");

		GetActiveChunk().append("float3 " + N + "_V = normalize(GetCameraPosition() - WorldPosition);\n");
		GetActiveChunk().append("float  " + N + "_NoV = saturate(dot(normalize(" + NV.Value + ".xyz), " + N + "_V));\n");
		GetActiveChunk().append("float " + N + " = saturate((" + BaseV.Value + ") + (1.0 - (" + BaseV.Value + ")) * pow(1.0 - " + N + "_NoV, " + ExpV.Value + "));\n");
		SetOwningOutputType(Exponent, EMaterialInputType::Float);
	}

	void FMaterialCompiler::DepthFade(CMaterialInput* FadeDistance)
	{
		FString N = FadeDistance->GetOwningNode()->GetNodeFullName();
		FInputValue F = GetTypedInputValue(FadeDistance, 100.0f);
		GetActiveChunk().append("float " + N + " = saturate(abs(ViewPosition.z) / max((" + F.Value + "), 1e-4));\n");
		SetOwningOutputType(FadeDistance, EMaterialInputType::Float);
	}

	void FMaterialCompiler::NormalFromHeight(CMaterialInput* Height, CMaterialInput* Strength)
	{
		FString N = Height->GetOwningNode()->GetNodeFullName();
		FInputValue H = GetTypedInputValue(Height, 0.0f);
		FInputValue S = GetTypedInputValue(Strength, 1.0f);
		GetActiveChunk().append("float " + N + "_Hx = ddx(" + H.Value + ");\n");
		GetActiveChunk().append("float " + N + "_Hy = ddy(" + H.Value + ");\n");
		GetActiveChunk().append("float3 " + N + " = normalize(float3(-" + N + "_Hx * (" + S.Value + "), -" + N + "_Hy * (" + S.Value + "), 1.0));\n");
		SetOwningOutputType(Height, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::DeriveNormalZ(CMaterialInput* InputXY)
	{
		FString N = InputXY->GetOwningNode()->GetNodeFullName();
		FInputValue V = GetTypedInputValue(InputXY, "float2(0.0, 0.0)");
		GetActiveChunk().append("float2 " + N + "_XY = (" + V.Value + ").xy * 2.0 - 1.0;\n");
		GetActiveChunk().append("float3 " + N + " = float3(" + N + "_XY, sqrt(saturate(1.0 - dot(" + N + "_XY, " + N + "_XY))));\n");
		SetOwningOutputType(InputXY, EMaterialInputType::Float3);
	}

	void FMaterialCompiler::BlendNormals(CMaterialInput* A, CMaterialInput* B)
	{
		FString N = A->GetOwningNode()->GetNodeFullName();
		FInputValue AV = GetTypedInputValue(A, "float3(0.0, 0.0, 1.0)");
		FInputValue BV = GetTypedInputValue(B, "float3(0.0, 0.0, 1.0)");
		GetActiveChunk().append("float3 " + N + "_A = (" + AV.Value + ").xyz * float3(2.0, 2.0, 2.0) + float3(-1.0, -1.0, 0.0);\n");
		GetActiveChunk().append("float3 " + N + "_B = (" + BV.Value + ").xyz * float3(-2.0, -2.0, 2.0) + float3(1.0, 1.0, -1.0);\n");
		GetActiveChunk().append("float3 " + N + " = normalize(" + N + "_A * dot(" + N + "_A, " + N + "_B) - " + N + "_B * " + N + "_A.z) * 0.5 + 0.5;\n");
		SetOwningOutputType(A, EMaterialInputType::Float3);
	}

	// Terrain

	void FMaterialCompiler::TerrainLayerWeight(const FString& ID, uint32 LayerIndex, CMaterialGraphNode* Node)
	{
		if (CurrentMaterialType != EMaterialType::Terrain)
		{
			EdNodeGraph::FError Error;
			Error.Node = Node;
			Error.Name = "Invalid Material Type";
			Error.Description = "TerrainLayerWeight is only usable in Terrain materials.";
			AddError(Error);
			GetActiveChunk().append("float " + ID + " = 0.0;\n");
			return;
		}

		if (LayerIndex > 3)
		{
			LayerIndex = 3;
		}

		const char* Swizzle = "x";
		switch (LayerIndex)
		{
			case 0: Swizzle = "x"; break;
			case 1: Swizzle = "y"; break;
			case 2: Swizzle = "z"; break;
			case 3: Swizzle = "w"; break;
		}
		GetActiveChunk().append("float " + ID + " = GetTerrainLayerWeights4(HeightUV)." + FString(Swizzle) + ";\n");
	}

	void FMaterialCompiler::TerrainLayerWeights(const FString& ID, CMaterialGraphNode* Node)
	{
		if (CurrentMaterialType != EMaterialType::Terrain)
		{
			EdNodeGraph::FError Error;
			Error.Node = Node;
			Error.Name = "Invalid Material Type";
			Error.Description = "TerrainLayerWeights is only usable in Terrain materials.";
			AddError(Error);
			GetActiveChunk().append("float4 " + ID + " = float4(1.0, 0.0, 0.0, 0.0);\n");
			return;
		}

		GetActiveChunk().append("float4 " + ID + " = GetTerrainLayerWeights4(HeightUV);\n");
	}

	void FMaterialCompiler::TerrainLayerBlend(CMaterialInput* Layer0, CMaterialInput* Layer1, CMaterialInput* Layer2, CMaterialInput* Layer3)
	{
		FString OwningNode = Layer0->GetOwningNode()->GetNodeFullName();

		if (CurrentMaterialType != EMaterialType::Terrain)
		{
			EdNodeGraph::FError Error;
			Error.Node = Layer0->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Invalid Material Type";
			Error.Description = "TerrainLayerBlend is only usable in Terrain materials.";
			AddError(Error);
			GetActiveChunk().append("float3 " + OwningNode + " = float3(0.0);\n");
			return;
		}

		FInputValue L0 = GetTypedInputValue(Layer0, "float3(0.0)");
		FInputValue L1 = GetTypedInputValue(Layer1, "float3(0.0)");
		FInputValue L2 = GetTypedInputValue(Layer2, "float3(0.0)");
		FInputValue L3 = GetTypedInputValue(Layer3, "float3(0.0)");

		auto Coerce = [](const FInputValue& V) -> FString
		{
			if (V.ComponentCount >= 4)
			{
				return "float3(" + V.Value + GetSwizzleForMask(V.Mask) + ".xyz)";
			}
			if (V.ComponentCount == 3)
			{
				return V.Value + GetSwizzleForMask(V.Mask);
			}
			if (V.ComponentCount == 2)
			{
				return "float3(" + V.Value + GetSwizzleForMask(V.Mask) + ", 0.0)";
			}
			return "float3(" + V.Value + GetSwizzleForMask(V.Mask) + ")";
		};

		FString L0Str = Coerce(L0);
		FString L1Str = Coerce(L1);
		FString L2Str = Coerce(L2);
		FString L3Str = Coerce(L3);

		// Blend formula lives once, in TerrainData.slang::BlendTerrainLayers4 (shared with the default terrain material).
		GetActiveChunk().append("float3 " + OwningNode + " = BlendTerrainLayers4("
			+ L0Str + ", " + L1Str + ", " + L2Str + ", " + L3Str + ", HeightUV);\n");
	}

	void FMaterialCompiler::GetBoundTextures(TVector<TObjectPtr<CTexture>>& Images)
	{
		Images = BoundImages;
	}

	void FMaterialCompiler::AddRaw(const FString& Raw)
	{
		GetActiveChunk().append(Raw);
	}
}
