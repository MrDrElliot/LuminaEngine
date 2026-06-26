#pragma once
#include "MaterialInput.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/CustomPrimitiveData.h"
#include "Renderer/MaterialTypes.h"

namespace Lumina
{
    class CMaterialExpression_CustomPrimitiveData;
    class CTexture;
    class CMaterialFunction;
    class FMaterialNodePin;
    class CMaterialGraphNode;
    class CMaterialInput;
    class CMaterialOutput;
    struct FMaterialUniforms;
    struct FMaterialParameter;
}


namespace Lumina
{
    enum class EMaterialShaderStage : uint8
    {
        Pixel,
        Vertex,
    };

    class FMaterialCompiler
    {
    public:

        struct FScalarParam
        {
            uint16 Index;
            float Value;
        };

        struct FVectorParam
        {
            uint16 Index;
            FVector4 Value;
        };

        struct FTextureParam
        {
            uint16 Index;
            TObjectPtr<CTexture> Texture;
        };

        struct FNodeOutputInfo
        {
            EMaterialInputType Type;
            EComponentMask Mask;
            FString NodeName;
        };

        struct FInputValue
        {
            FString             Value;
            EMaterialInputType  Type;
            EComponentMask      Mask;
            int32               ComponentCount;
        };

        // Aggregated cost / complexity metrics derived from the generated chunks. Computed on demand
        // by GetStats(); not maintained incrementally so it is safe to query multiple times after compile.
        struct FShaderStats
        {
            uint32 PixelInstructions     = 0;   // newline-terminated lines in the pixel chunks
            uint32 VertexInstructions    = 0;   // newline-terminated lines in the vertex chunks
            uint32 TextureSamples        = 0;   // count of ".Sample(" call sites
            uint32 MathOps               = 0;   // sin/cos/lerp/normalize/dot/...
            uint32 NoiseOps              = 0;   // value/gradient/perlin/voronoi/simple noise + hash*
            uint32 ScalarParameters      = 0;
            uint32 VectorParameters      = 0;
            uint32 TextureParameters     = 0;
            uint32 BoundTextures         = 0;   // includes static (non-parameter) texture binds
            uint32 PixelCharacters       = 0;
            uint32 VertexCharacters      = 0;
            bool   bUsesVertexStage      = false;
            // Rough relative cost: weighted sum biased toward expensive ops. Not a real GPU cycle count
            // but useful for comparing materials side-by-side.
            uint32 EstimatedCost         = 0;
        };

    public:
        FMaterialCompiler();

        // Backwards-compatible single-stage path (pixel shader only).
        FString BuildTree(size_t& StartReplacement, size_t& EndReplacement, EMaterialType MaterialType = EMaterialType::PBR) const;

        // Per-stage build: substitutes $MATERIAL_INPUTS / $MATERIAL_VERTEX_INPUTS with the pixel / vertex
        // chunks (plus a vertex-stage alias preamble so node code naming WorldPosition / UV0 / etc. is valid).
        void BuildShaders(FString& OutPixelShader, FString& OutVertexShader, EMaterialType MaterialType = EMaterialType::PBR) const;

        // Substitute $MATERIAL_VERTEX_INPUTS in a depth/shadow vertex template with the base-pass vertex
        // chunks (per-material depth/shadow shaders for WPO materials). MaterialType picks the alias preamble.
        FString BuildVertexShaderFromTemplate(const FString& TemplateAbsolutePath, EMaterialType MaterialType = EMaterialType::PBR) const;

        // Substitute BOTH material tokens in a deferred template (DeferredMaterial.slang): the vertex graph
        // ($MATERIAL_VERTEX_INPUTS, for WPO reconstruction) and the pixel graph ($MATERIAL_INPUTS, shading).
        FString BuildDeferredShaderFromTemplate(const FString& TemplateAbsolutePath, EMaterialType MaterialType = EMaterialType::PBR) const;

        // Substitute only $MATERIAL_INPUTS in a pixel template with the pixel-graph chunks (e.g. the masked
        // VisBuffer pixel shader, which runs the graph just to evaluate Opacity for the geometry-stage clip).
        FString BuildPixelShaderFromTemplate(const FString& TemplateAbsolutePath) const;

        // True when the graph fed any chunks into the vertex stage. Equivalent
        // to "WorldPositionOffset pin had a connection."
        bool UsesVertexStage() const { return !VertexChunks.empty() || !VertexOutputChunks.empty(); }

        // Stage routing: each node-emit op writes the current stage's chunk. CompileGraph flips this around
        // the two-root walk (WPO->vertex, pixel pins->pixel); shared nodes are visited once per stage.
        void SetStage(EMaterialShaderStage InStage) { CurrentStage = InStage; }
        EMaterialShaderStage GetStage() const { return CurrentStage; }
        FString& GetActiveChunk() { return CurrentStage == EMaterialShaderStage::Vertex ? VertexChunks : PixelChunks; }
        const FString& GetActiveChunk() const { return CurrentStage == EMaterialShaderStage::Vertex ? VertexChunks : PixelChunks; }

        // Output-node-direct emission helpers (bypass the stage cursor).
        void AddPixelOutput(const FString& Raw) { PixelOutputChunks.append(Raw); }
        void AddVertexOutput(const FString& Raw) { VertexOutputChunks.append(Raw); }

        // Stage gate for pixel-only nodes (ScreenPosition, FragmentDepth, ...). Returns true when emission
        // may proceed; on a vertex-stage call pushes an error at Node and returns false. Call first in GenerateDefinition.
        bool RequirePixelStage(CMaterialGraphNode* Node, const FString& NodeKindName);

        // UI materials are a fullscreen brush pass with no geometry/camera/depth/vertex attributes. Returns
        // true (and errors on Node) when the input node is unavailable in the UI domain; caller emits a default.
        bool RejectInUI(CMaterialGraphNode* Node, const char* NodeName);

        // Parameter definitions
        void DefineFloatParameter(const FString& NodeID, const FName& ParamID, float Value);
        void DefineFloat2Parameter(const FString& NodeID, const FName& ParamID, float Value[2]);
        void DefineFloat3Parameter(const FString& NodeID, const FName& ParamID, float Value[3]);
        void DefineFloat4Parameter(const FString& NodeID, const FName& ParamID, float Value[4]);

        // Constant definitions
        void DefineConstantFloat(const FString& ID, float Value);
        void DefineConstantFloat2(const FString& ID, float Value[2]);
        void DefineConstantFloat3(const FString& ID, float Value[3]);
        void DefineConstantFloat4(const FString& ID, float Value[4]);

        // Data Type operations.
        void BreakFloat2(CMaterialInput* A);
        void BreakFloat3(CMaterialInput* A);
        void BreakFloat4(CMaterialInput* A);

        void MakeFloat2(CMaterialInput* R, CMaterialInput* G);
        void MakeFloat3(CMaterialInput* R, CMaterialInput* G, CMaterialInput* B);
        void MakeFloat4(CMaterialInput* R, CMaterialInput* G, CMaterialInput* B, CMaterialInput* A);

        void Append(CMaterialInput* A, CMaterialInput* B);
        void ComponentMask(CMaterialInput* A);

        // Texture operations
        void DefineTextureSample(const FString& ID);
        void TextureSample(const FString& ID, CTexture* Texture, CMaterialInput* Input);
        void TextureSampleParameter(const FString& ID, const FName& ParamID, CTexture* Texture, CMaterialInput* Input);

        // Built-in inputs
        void VertexNormal(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void VertexTangent(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void VertexBitangent(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void VertexColor(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void TexCoords(const FString& ID, uint32 Index, CMaterialInput* Tiling, float UTiling, float VTiling);
        void Panner(CMaterialInput* UV, CMaterialInput* Time, CMaterialInput* Speed);
        void RotateUV(CMaterialInput* UV, CMaterialInput* Center, CMaterialInput* Rotation);
        void TilingAndOffset(CMaterialInput* UV, CMaterialInput* Tiling, CMaterialInput* Offset);
        void FlipBookUV(CMaterialInput* UV, CMaterialInput* NumCols, CMaterialInput* NumRows, CMaterialInput* Time, CMaterialInput* FPS);
        void PolarCoordinates(CMaterialInput* UV, CMaterialInput* Center);
        void TwirlUV(CMaterialInput* UV, CMaterialInput* Center, CMaterialInput* Strength);
        void WorldPos(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void CameraPos(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void ObjectScale(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void ObjectPosition(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void EntityID(const FString& ID);
        void Time(const FString& ID);
        void ScreenPosition(const FString& ID, bool bRaw);
        void ViewDirection(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void ReflectionVector(const FString& ID, CMaterialGraphNode* Node = nullptr);
        void FragmentDepth(const FString& ID, bool bLinear, CMaterialGraphNode* Node = nullptr);
        void ViewportSize(const FString& ID);
        void AspectRatio(const FString& ID);
        void SceneColor(const FString& ID, CMaterialInput* UV);
        void SceneDepth(const FString& ID, CMaterialInput* UV, bool bLinear);
        void SceneHDRColor(const FString& ID, CMaterialInput* UV);
        void NumericConstant(const FString& ID, float Value);
        void CustomPrimitiveData(CMaterialExpression_CustomPrimitiveData* Node, ECustomPrimitiveDataType Type);

        // Math operations - binary
        void Multiply(CMaterialInput* A, CMaterialInput* B);
        void Divide(CMaterialInput* A, CMaterialInput* B);
        void Add(CMaterialInput* A, CMaterialInput* B);
        void Subtract(CMaterialInput* A, CMaterialInput* B);
        void Power(CMaterialInput* A, CMaterialInput* B);
        void Mod(CMaterialInput* A, CMaterialInput* B);
        void Min(CMaterialInput* A, CMaterialInput* B);
        void Max(CMaterialInput* A, CMaterialInput* B);
        void Step(CMaterialInput* A, CMaterialInput* B);
        void Atan2Op(CMaterialInput* Y, CMaterialInput* X);

        // Math operations - unary
        void Sin(CMaterialInput* A);
        void Cos(CMaterialInput* A);
        void Tan(CMaterialInput* A);
        void Asin(CMaterialInput* A);
        void Acos(CMaterialInput* A);
        void Atan(CMaterialInput* A);
        void Sinh(CMaterialInput* A);
        void Cosh(CMaterialInput* A);
        void Tanh(CMaterialInput* A);
        void Sqrt(CMaterialInput* A);
        void Rsqrt(CMaterialInput* A);
        void Log(CMaterialInput* A);
        void Log2(CMaterialInput* A);
        void Log10(CMaterialInput* A);
        void Exp(CMaterialInput* A);
        void Exp2(CMaterialInput* A);
        void Sign(CMaterialInput* A);
        void OneMinus(CMaterialInput* A);
        void Reciprocal(CMaterialInput* A);
        void Round(CMaterialInput* A);
        void Truncate(CMaterialInput* A);
        void Negate(CMaterialInput* A);
        void Square(CMaterialInput* A);
        void DegreesToRadians(CMaterialInput* A);
        void RadiansToDegrees(CMaterialInput* A);
        void Fract(CMaterialInput* A);
        void Floor(CMaterialInput* A);
        void Ceil(CMaterialInput* A);
        void Abs(CMaterialInput* A);
        void Saturate(CMaterialInput* A);

        // Math operations - ternary
        void Lerp(CMaterialInput* A, CMaterialInput* B, CMaterialInput* C);
        void Clamp(CMaterialInput* A, CMaterialInput* B, CMaterialInput* C);
        void SmoothStep(CMaterialInput* A, CMaterialInput* B, CMaterialInput* C);
        void Remap(CMaterialInput* X, CMaterialInput* InMin, CMaterialInput* InMax, CMaterialInput* OutMin, CMaterialInput* OutMax);

        // Vector operations
        void Normalize(CMaterialInput* A);
        void Distance(CMaterialInput* A, CMaterialInput* B);
        void Length(CMaterialInput* A);
        void Dot(CMaterialInput* A, CMaterialInput* B);
        void Cross(CMaterialInput* A, CMaterialInput* B);
        void Reflect(CMaterialInput* I, CMaterialInput* N);
        void Refract(CMaterialInput* I, CMaterialInput* N, CMaterialInput* Eta);
        void RotateAboutAxis(CMaterialInput* Position, CMaterialInput* Axis, CMaterialInput* Angle, CMaterialInput* Pivot);

        // Color operations
        void Desaturate(CMaterialInput* Color, CMaterialInput* Amount);
        void Luminance(CMaterialInput* Color);
        void RGBToHSV(CMaterialInput* RGB);
        void HSVToRGB(CMaterialInput* HSV);
        void Posterize(CMaterialInput* Color, CMaterialInput* Steps);
        void GammaCorrection(CMaterialInput* Color, CMaterialInput* Gamma);
        void Contrast(CMaterialInput* Color, CMaterialInput* Amount);
        void Brightness(CMaterialInput* Color, CMaterialInput* Amount);
        void Tint(CMaterialInput* Color, CMaterialInput* TintColor, CMaterialInput* Amount);
        void LinearToSRGB(CMaterialInput* Color);
        void SRGBToLinear(CMaterialInput* Color);

        // Noise / procedural
        void Hash11(CMaterialInput* X);
        void Hash21(CMaterialInput* UV);
        void Hash22(CMaterialInput* UV);
        void Hash33(CMaterialInput* P);
        void ValueNoise(CMaterialInput* UV);
        void GradientNoise(CMaterialInput* UV);
        void PerlinNoise(CMaterialInput* UV);
        void VoronoiNoise(CMaterialInput* UV);
        void SimpleNoise(CMaterialInput* UV);
        void Checkerboard(CMaterialInput* UV);

        // Conditional / comparison
        void If(CMaterialInput* X, CMaterialInput* Y, CMaterialInput* GreaterThan, CMaterialInput* EqualTo, CMaterialInput* LessThan, float Threshold);
        void Compare(const FString& Op, CMaterialInput* A, CMaterialInput* B);

        // Advanced shading helpers
        void Fresnel(CMaterialInput* Exponent, CMaterialInput* BaseReflect, CMaterialInput* Normal);
        void DepthFade(CMaterialInput* FadeDistance);
        void NormalFromHeight(CMaterialInput* Height, CMaterialInput* Strength);
        void DeriveNormalZ(CMaterialInput* InputXY);
        void BlendNormals(CMaterialInput* A, CMaterialInput* B);

        // Terrain-only helpers (emit an error on non-terrain materials so the graph reports it).
        void TerrainLayerWeight(const FString& ID, uint32 LayerIndex, CMaterialGraphNode* Node);
        void TerrainLayerWeights(const FString& ID, CMaterialGraphNode* Node);
        void TerrainLayerBlend(CMaterialInput* Layer0, CMaterialInput* Layer1, CMaterialInput* Layer2, CMaterialInput* Layer3);

        void SetMaterialType(EMaterialType InType) { CurrentMaterialType = InType; }
        EMaterialType GetMaterialType() const { return CurrentMaterialType; }

        void NewLine();
        void AddRaw(const FString& Raw);

        void GetBoundTextures(TVector<TObjectPtr<CTexture>>& Images);

        /** Export the dynamic parameter manifest discovered during compile and seed default values into the uniform block. */
        void GetParameters(TVector<FMaterialParameter>& OutParams, FMaterialUniforms& OutUniforms) const;

        // Computes shader complexity / cost metrics from the current chunk state. Call after
        // CompileGraph so the chunks are populated. Cheap (single linear scan per chunk).
        FShaderStats GetStats() const;

        FORCEINLINE bool HasErrors() const { return !Errors.empty(); }
        FORCEINLINE void AddError(const EdNodeGraph::FError& Error) { Errors.push_back(Error); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetErrors() const { return Errors; }

        FInputValue GetTypedInputValue(CMaterialInput* Input, float DefaultValue = 0.0f);
        FInputValue GetTypedInputValue(CMaterialInput* Input, const FString& DefaultValueStr);

        static int32 GetComponentCount(EComponentMask Mask);
        static int32 GetComponentCount(EMaterialInputType Type);

        // HLSL scalar/vector type name ("float", "float2", ...) for an input type. Public so the
        // material-function call node can declare its argument/result locals.
        static FString GetHLSLTypeName(EMaterialInputType Type);

        // Inlined function nodes get variable-name prefixes so repeated/nested calls never collide;
        // a call node pushes a per-call prefix (composed with the current one, so nesting accumulates).
        void PushInlinePrefix(const FString& Prefix) { InlinePrefixStack.push_back(Prefix); }
        void PopInlinePrefix() { if (!InlinePrefixStack.empty()) { InlinePrefixStack.pop_back(); } }
        const FString& GetCurrentInlinePrefix() const;

        // Recursion guard: returns false when Function is already inlined higher on the stack (self-call);
        // caller emits an error and bails. Pair a true result with EndInlineFunction.
        bool BeginInlineFunction(CMaterialFunction* Function);
        void EndInlineFunction(CMaterialFunction* Function);

    private:

        EMaterialInputType DetermineResultType(EMaterialInputType A, EMaterialInputType B, bool IsComponentWise = true);

        NODISCARD EMaterialInputType EmitBinaryOp(const FString& Op, CMaterialInput* A, CMaterialInput* B, float DefaultA, float DefaultB, bool IsComponentWise = true);

        // Generic helpers used by most node operations to keep call sites tiny.
        EMaterialInputType EmitUnaryFunc(const FString& Func, CMaterialInput* A, float DefaultA);
        EMaterialInputType EmitBinaryFunc(const FString& Func, CMaterialInput* A, CMaterialInput* B, float DefaultA, float DefaultB);
        EMaterialInputType EmitTernaryFunc(const FString& Func, CMaterialInput* A, CMaterialInput* B, CMaterialInput* C, float DA, float DB, float DC);

        // Sets the owning node's output type so downstream nodes see the right width/swizzle.
        void SetOwningOutputType(CMaterialInput* AnyInputOnNode, EMaterialInputType Type);

    private:

        // Per-stage graph body chunks. The active chunk for general node
        // emission is selected by CurrentStage.
        FString PixelChunks;
        FString VertexChunks;

        // Output-node assignments, kept separate so they always land at the end of the substituted block
        // regardless of topo order and the output node can target each stage without driving the cursor.
        FString PixelOutputChunks;
        FString VertexOutputChunks;

        EMaterialShaderStage CurrentStage = EMaterialShaderStage::Pixel;

        TVector<TObjectPtr<CTexture>> BoundImages;
        TVector<EdNodeGraph::FError> Errors;

        THashMap<FName, FScalarParam>  ScalarParameters;
        THashMap<FName, FVectorParam>  VectorParameters;
        THashMap<FName, FTextureParam> TextureParameters;

        uint16 NumScalarParams = 0;
        uint16 NumVectorParams = 0;
        uint16 NumTextureParams = 0;

        EMaterialType CurrentMaterialType = EMaterialType::PBR;

        // Active material-function inlining state (see PushInlinePrefix / BeginInlineFunction).
        TVector<FString>            InlinePrefixStack;
        TVector<CMaterialFunction*> InlineFunctionStack;
    };
}
