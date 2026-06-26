#include "MaterialCompiler.h"

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Nodes/MaterialNodes.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNode_Reroute.h"

namespace Lumina
{
	FMaterialCompiler::FMaterialCompiler()
	{
		PixelChunks.reserve(2000);
		VertexChunks.reserve(512);
		PixelOutputChunks.reserve(512);
		VertexOutputChunks.reserve(128);
	}

	// Vertex-stage alias preamble: maps pixel-stage variable names to vertex equivalents.
	// Pixel-only references are stubbed to neutral values; WPO-sourced nodes won't reach them.
	static const char* GVertexStageAliasPreamble =
		"\t// Material graph variable aliases (vertex stage).\n"
		"\tfloat3 WorldPosition = WorldPos.xyz;\n"
		"\tfloat3 WorldNormal   = NormalWS;\n"
		"\tfloat2 UV0           = VertexData.UV;\n"
		"\tfloat4 VertexColor   = VertexData.Color;\n"
		"\tuint   MaterialIndex = Inst.MaterialIndex;\n"
		"\tuint   EntityID      = Inst.EntityID;\n"
		"\tfloat3 ViewPosition  = float3(0.0);\n";

	// Terrain variant: TerrainBaseVertexPass.slang lacks FVertexData/FGPUInstance; same alias names keep node-emit code reusable.
	static const char* GVertexStageAliasPreambleTerrain =
		"\t// Material graph variable aliases (vertex stage, terrain).\n"
		"\tfloat3 WorldPosition = WorldPos;\n"
		"\tfloat3 WorldNormal   = NormalWS;\n"
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

		// Pixel: output node declares FMaterialPixelInputs Material; only append body + assignments.
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

		// Decal: fixed unit-cube vertex stage; the box is projected onto scene depth in the pixel stage, so no WPO.
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

		// Vertex template declares FMaterialVertexInputs Material above the token; only emit assignments.
		const FString VertexPath = BasePath + (bIsTerrain ? "TerrainBaseVertexPass.slang" : "MeshletVertex.slang");
		OutVertexShader = BuildVertexShaderFromTemplate(VertexPath, MaterialType);
	}

	FString FMaterialCompiler::BuildVertexShaderFromTemplate(const FString& TemplateAbsolutePath, EMaterialType MaterialType) const
	{
		FString Loaded;
		if (!FileHelper::LoadFileIntoString(Loaded, TemplateAbsolutePath))
		{
			LOG_ERROR("Failed to find {}!", TemplateAbsolutePath);
			return Loaded;
		}

		const char* Preamble = (MaterialType == EMaterialType::Terrain)
			? GVertexStageAliasPreambleTerrain
			: GVertexStageAliasPreamble;
		FString Replacement = FString(Preamble) + VertexChunks + VertexOutputChunks;
		SubstituteToken(Loaded, "$MATERIAL_VERTEX_INPUTS", Replacement);
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
		const char* Preamble = (MaterialType == EMaterialType::Terrain)
			? GVertexStageAliasPreambleTerrain
			: GVertexStageAliasPreamble;
		FString VertexReplacement = FString(Preamble) + VertexChunks + VertexOutputChunks;
		SubstituteToken(Loaded, "$MATERIAL_VERTEX_INPUTS", VertexReplacement);

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
		return GetTypedInputValue(Input, eastl::to_string(DefaultValue));
	}

	// Returns nullptr if the reroute chain dead-ends at an unconnected input (treated same as no connection).
	static CMaterialOutput* ResolveSourceOutputThroughReroutes(CMaterialOutput* OutputPin)
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

			// Reroute owner: chase back through its single input pin's connection.
			const TVector<TObjectPtr<CEdNodeGraphPin>>& Inputs = Owner->GetInputPins();
			if (Inputs.empty())
			{
				return nullptr;
			}

			CEdNodeGraphPin* RerouteInput = Inputs[0].Get();
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
			Conn					= ResolveSourceOutputThroughReroutes(Conn);

			if (Conn == nullptr)
			{
				FString NodeName		= Input->GetOwningNode()->GetNodeFullName();
				Result.Type				= GetTypeFromComponentCount(GetComponentCount(Input->GetComponentMask()));
				Result.ComponentCount	= GetComponentCount(Input->GetComponentMask());
				Result.Value 			= DefaultValueStr;
				Result.Mask  			= Input->GetComponentMask();
				return Result;
			}

			// A function-call output pin binds its own emitted local via ResolvedVar; everything
			// else reads the owning node's single FullName variable.
			FString NodeName		= Conn->ResolvedVar.empty() ? Conn->GetOwningNode()->GetNodeFullName() : Conn->ResolvedVar;

			Result.Type				= Conn->InputType;
			Result.ComponentCount	= GetComponentCount(Result.Type);
			Result.Value 			= NodeName;
			Result.Mask  			= Conn->GetComponentMask();
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

		FString IndexString = eastl::to_string(ScalarParameters[ParamID].Index);
		GetActiveChunk().append("float " + NodeID + " = GetMaterialScalar(MaterialIndex, " + IndexString + ");\n");
	}

	void FMaterialCompiler::DefineFloat2Parameter(const FString& NodeID, const FName& ParamID, float Value[2])
	{
		if (VectorParameters.find(ParamID) == VectorParameters.end())
		{
			VectorParameters[ParamID].Index = NumVectorParams++;
			VectorParameters[ParamID].Value = FVector4(Value[0], Value[1], 0.0f, 1.0f);
		}

		FString IndexString = eastl::to_string(VectorParameters[ParamID].Index);
		GetActiveChunk().append("float2 " + NodeID + " = GetMaterialVec4(MaterialIndex, " + IndexString + ").xy;\n");
	}

	void FMaterialCompiler::DefineFloat3Parameter(const FString& NodeID, const FName& ParamID, float Value[3])
	{
		if (VectorParameters.find(ParamID) == VectorParameters.end())
		{
			VectorParameters[ParamID].Index = NumVectorParams++;
			VectorParameters[ParamID].Value = FVector4(Value[0], Value[1], Value[2], 1.0f);
		}

		FString IndexString = eastl::to_string(VectorParameters[ParamID].Index);
		GetActiveChunk().append("float3 " + NodeID + " = GetMaterialVec4(MaterialIndex, " + IndexString + ").xyz;\n");
	}

	void FMaterialCompiler::DefineFloat4Parameter(const FString& NodeID, const FName& ParamID, float Value[4])
	{
		if (VectorParameters.find(ParamID) == VectorParameters.end())
		{
			VectorParameters[ParamID].Index = NumVectorParams++;
			VectorParameters[ParamID].Value = FVector4(Value[0], Value[1], Value[2], Value[3]);
		}

		FString IndexString = eastl::to_string(VectorParameters[ParamID].Index);
		GetActiveChunk().append("float4 " + NodeID + " = GetMaterialVec4(MaterialIndex, " + IndexString + ");\n");
	}

	void FMaterialCompiler::DefineConstantFloat(const FString& ID, float Value)
	{
		FString ValueString = eastl::to_string(Value);
		GetActiveChunk().append("float " + ID + " = " + ValueString + ";\n");
	}

	void FMaterialCompiler::DefineConstantFloat2(const FString& ID, float Value[2])
	{
		FString ValueStringX = eastl::to_string(Value[0]);
		FString ValueStringY = eastl::to_string(Value[1]);
		GetActiveChunk().append("float2 " + ID + " = float2(" + ValueStringX + ", " + ValueStringY + ");\n");
	}

	void FMaterialCompiler::DefineConstantFloat3(const FString& ID, float Value[3])
	{
		FString ValueStringX = eastl::to_string(Value[0]);
		FString ValueStringY = eastl::to_string(Value[1]);
		FString ValueStringZ = eastl::to_string(Value[2]);
		GetActiveChunk().append("float3 " + ID + " = float3(" + ValueStringX + ", " + ValueStringY + ", " + ValueStringZ + ");\n");
	}

	void FMaterialCompiler::DefineConstantFloat4(const FString& ID, float Value[4])
	{
		FString ValueStringX = eastl::to_string(Value[0]);
		FString ValueStringY = eastl::to_string(Value[1]);
		FString ValueStringZ = eastl::to_string(Value[2]);
		FString ValueStringW = eastl::to_string(Value[3]);
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
			Error.Description.sprintf("MakeFloat2 requires two Float inputs, got %s and %s",
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
			Error.Description.sprintf("MakeFloat3 requires three Float inputs, got %s, %s and %s",
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
			Error.Description.sprintf("MakeFloat4 requires four Float inputs, got %s, %s, %s and %s",
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
		SetOwningOutputType(A, ResultType);
	}

	void FMaterialCompiler::ComponentMask(CMaterialInput* A)
	{
		CMaterialExpression_ComponentMask* OwningNode = A->GetOwningNode<CMaterialExpression_ComponentMask>();

		if (!A->HasConnection())
		{
			EdNodeGraph::FError Error;
			Error.Node = A->GetOwningNode<CMaterialGraphNode>();
			Error.Name = "Invalid Action";
			Error.Description.append("ComponentMask is required to have an input value");

			AddError(Error);
			GetActiveChunk().append("// ERROR: Component Mask Issue\n");
		}

		FString Swizzle = ".";
		int32 ComponentCount = 0;
		if (OwningNode->R)
		{
			Swizzle += "r";
			ComponentCount++;
		}
		if (OwningNode->G)
		{
			Swizzle += "g";
			ComponentCount++;
		}
		if (OwningNode->B)
		{
			Swizzle += "b";
			ComponentCount++;
		}
		if (OwningNode->A)
		{
			Swizzle += "a";
			ComponentCount++;
		}

		FString VectorType = GetVectorType(ComponentCount);
		FInputValue Value = GetTypedInputValue(A, "");

		GetActiveChunk().append(VectorType + " " + OwningNode->GetNodeFullName() + " = " + Value.Value + Swizzle + ";\n");
		SetOwningOutputType(A, GetTypeFromComponentCount(ComponentCount));
	}

	void FMaterialCompiler::DefineTextureSample(const FString& ID)
	{
		return;
	}

	void FMaterialCompiler::TextureSample(const FString& ID, CTexture* Texture, CMaterialInput* Input)
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

		auto It = eastl::find(BoundImages.begin(), BoundImages.end(), Texture);

		int32 Index;
		if (It != BoundImages.end())
		{
			Index = (int32)eastl::distance(BoundImages.begin(), It);
		}
		else
		{
			Index = (int32)BoundImages.size();
			BoundImages.push_back(Texture);
			NumTextureParams++;
		}

		GetActiveChunk().append("float4 " + ID + " = SampleTexture2D(GetMaterialTexture(MaterialIndex, " + eastl::to_string(Index) + "), SAMPLER_LINEAR_WRAP, " + UVStr + ");\n");
	}

	void FMaterialCompiler::TextureSampleParameter(const FString& ID, const FName& ParamID, CTexture* Texture, CMaterialInput* Input)
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

		int32 Index;
		auto Existing = TextureParameters.find(ParamID);
		if (Existing != TextureParameters.end())
		{
			Index = (int32)Existing->second.Index;
		}
		else
		{
			Index = (int32)BoundImages.size();
			BoundImages.push_back(Texture);
			TextureParameters[ParamID] = FTextureParam{ (uint16)Index, Texture };
			NumTextureParams++;
		}

		GetActiveChunk().append("float4 " + ID + " = SampleTexture2D(GetMaterialTexture(MaterialIndex, " + eastl::to_string(Index) + "), SAMPLER_LINEAR_WRAP, " + UVStr + ");\n");
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

		Stats.ScalarParameters    = NumScalarParams;
		Stats.VectorParameters    = NumVectorParams;
		Stats.TextureParameters   = NumTextureParams;
		Stats.BoundTextures       = static_cast<uint32>(BoundImages.size());
		Stats.bUsesVertexStage    = UsesVertexStage();

		// Weighted approximation of relative cost. Texture samples and noise dominate; math is cheap;
		// vertex-stage work is amortized across vertices so it counts less than per-pixel work.
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
		if (CurrentStage == EMaterialShaderStage::Pixel)
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
		GetActiveChunk().append("float3 " + ID + " = Input.TangentWS.xyz;\n");
	}

	void FMaterialCompiler::VertexBitangent(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Vertex Bitangent"))
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 1.0, 0.0);\n");
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = cross(WorldNormal.xyz, Input.TangentWS.xyz) * Input.TangentWS.w;\n");
	}

	void FMaterialCompiler::VertexColor(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Vertex Color"))
		{
			GetActiveChunk().append("float4 " + ID + " = float4(1.0, 1.0, 1.0, 1.0);\n");
			return;
		}
		GetActiveChunk().append("float4 " + ID + " = VertexColor;\n");
	}

	void FMaterialCompiler::TexCoords(const FString& ID, uint32 Index, CMaterialInput* Tiling, float UTiling, float VTiling)
	{
		// Connected Tiling pin overrides the inline UTiling/VTiling defaults.
		FInputValue TilingValue = GetTypedInputValue(Tiling, "float2(" + eastl::to_string(UTiling) + ", " + eastl::to_string(VTiling) + ")");
		GetActiveChunk().append("float2 " + ID + " = UV0 * " + TilingValue.Value + ";\n");
	}

	void FMaterialCompiler::Panner(CMaterialInput* UV, CMaterialInput* Time, CMaterialInput* Speed)
	{
		CMaterialExpression_Panner* PannerNode = UV->GetOwningNode<CMaterialExpression_Panner>();

		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue TimeValue = GetTypedInputValue(Time, "GetTime()");
		FInputValue SpeedValue = GetTypedInputValue(Speed, "float2(" + eastl::to_string(PannerNode->SpeedX) + ", " + eastl::to_string(PannerNode->SpeedY) + ")");
		const FString OwningNode = UV->GetOwningNode()->GetNodeFullName();

		GetActiveChunk().append("float2 " + OwningNode + " = " + UVValue.Value + " + " + SpeedValue.Value + " * " + TimeValue.Value + ";\n");

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

		SetOwningOutputType(UV, EMaterialInputType::Float2);
	}

	void FMaterialCompiler::TilingAndOffset(CMaterialInput* UV, CMaterialInput* Tiling, CMaterialInput* Offset)
	{
		FString OwningNode = UV->GetOwningNode()->GetNodeFullName();
		FInputValue UVValue = GetTypedInputValue(UV, "float2(UV0)");
		FInputValue TilingValue = GetTypedInputValue(Tiling, "float2(1.0, 1.0)");
		FInputValue OffsetValue = GetTypedInputValue(Offset, "float2(0.0, 0.0)");

		GetActiveChunk().append("float2 " + OwningNode + " = " + UVValue.Value + " * " + TilingValue.Value + " + " + OffsetValue.Value + ";\n");
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
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = WorldPosition;\n");
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

	void FMaterialCompiler::ObjectScale(const FString& ID, CMaterialGraphNode* Node)
	{
		// Only PBR surface passes carry the per-instance FGPUInstance; others get a neutral 1.
		if (RejectInUI(Node, "Object Scale") || CurrentMaterialType != EMaterialType::PBR)
		{
			GetActiveChunk().append("float3 " + ID + " = float3(1.0, 1.0, 1.0);\n");
			return;
		}
		// Scale = world-space length of each basis column of the object->world matrix.
		GetActiveChunk().append(
			"float3 " + ID + " = float3("
			"length(mul(Inst.ModelMatrix, float4(1.0, 0.0, 0.0, 0.0)).xyz), "
			"length(mul(Inst.ModelMatrix, float4(0.0, 1.0, 0.0, 0.0)).xyz), "
			"length(mul(Inst.ModelMatrix, float4(0.0, 0.0, 1.0, 0.0)).xyz));\n");
	}

	void FMaterialCompiler::ObjectPosition(const FString& ID, CMaterialGraphNode* Node)
	{
		if (RejectInUI(Node, "Object Position") || CurrentMaterialType != EMaterialType::PBR)
		{
			GetActiveChunk().append("float3 " + ID + " = float3(0.0, 0.0, 0.0);\n");
			return;
		}
		GetActiveChunk().append("float3 " + ID + " = mul(Inst.ModelMatrix, float4(0.0, 0.0, 0.0, 1.0)).xyz;\n");
	}

	void FMaterialCompiler::EntityID(const FString& ID)
	{
		GetActiveChunk().append("float " + ID + " = float(EntityID);\n");
	}

	void FMaterialCompiler::Time(const FString& ID)
	{
		GetActiveChunk().append("float " + ID + " = GetTime();\n");
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

	// SceneColor is only valid in PostProcess materials (samples the pass-input RT at set 2, binding 0);
	// other domains have no such binding, so emit a graph error rather than a shader that fails to link.
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
		GetActiveChunk().append("float " + ID + " = " + eastl::to_string(Value) + ";\n");
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
		GetActiveChunk().append(TypeStr + " " + N + " = (abs(" + N + "_Diff) < " + eastl::to_string(Threshold) + ") ? (" + EV.Value + ") : ((" + N + "_Diff > 0.0) ? (" + GV.Value + ") : (" + LV.Value + "));\n");
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
