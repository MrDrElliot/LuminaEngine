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
    struct SKeyedCurve;
}


namespace Lumina
{
    // Which codegen chunk the compiler is currently emitting into (pixel vs vertex graph). Distinct from
    // the runtime EMaterialShaderStage (Material.h), which enumerates every COMPILED stage on the asset.
    enum class EMaterialCompileStage : uint8
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

        // Whether a value's screen-space derivative is known analytically.
        //
        // The deferred pass reconstructs its surface from the VisBuffer, so a 2x2 quad there can straddle
        // unrelated triangles and implicit ddx/ddy are garbage. It has to sample with explicit gradients,
        // which means every UV chain needs its derivative carried alongside it. Forward lanes have correct
        // implicit derivatives and ignore all of this (see SampleTexture2DAuto).
        enum class EDerivState : uint8
        {
            Zero,       // constant / parameter / uniform -- derivative is identically zero, emit nothing
            Valid,      // <Value>_DDX / <Value>_DDY exist and are exact
            Unknown,    // not derivable (noise, arbitrary funcs) -- consumers fall back to UV0's gradient
        };

        struct FInputValue
        {
            FString             Value;
            EMaterialInputType  Type;
            EComponentMask      Mask;
            int32               ComponentCount;
            // Filled from DerivByVar by GetTypedInputValue. A literal default has no producing node and
            // therefore no derivative, which is Zero rather than Unknown.
            EDerivState         Deriv = EDerivState::Zero;
            FString             DDX;
            FString             DDY;
        };

        // What a producing node published about its own derivative.
        struct FDerivInfo
        {
            EDerivState State = EDerivState::Unknown;
            FString     DDX;
            FString     DDY;
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
            // Texture samples whose UV chain had no derivable gradient, so they fell back to UV0's. Correct
            // in the forward lanes (implicit derivatives) but APPROXIMATE in the deferred VisBuffer pass:
            // the mip is picked from UV0's rate of change rather than the sampled UV's. A tiled or otherwise
            // transformed UV that lands here samples too fine a mip, which costs texture bandwidth and
            // aliases. Non-zero means some node in that chain has no derivative rule yet.
            uint32 UVGradientFallbacks   = 0;
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

        // The body substituted for $MATERIAL_VERTEX_INPUTS. Always assigns WorldPositionOffset, and emits
        // NOTHING else when the graph has no WPO, so every such material generates an identical stage.
        FString BuildVertexStageBody(EMaterialType MaterialType) const;

        // Stage routing: each node-emit op writes the current stage's chunk. CompileGraph flips this around
        // the two-root walk (WPO->vertex, pixel pins->pixel); shared nodes are visited once per stage.
        // Clears the parameter-fetch dedupe map: the two stages emit into separate scopes, so a variable
        // hoisted in one is not reachable from the other.
        void SetStage(EMaterialCompileStage InStage) { CurrentStage = InStage; EmittedParamFetches.clear(); }
        EMaterialCompileStage GetStage() const { return CurrentStage; }
        FString& GetActiveChunk() { return CurrentStage == EMaterialCompileStage::Vertex ? VertexChunks : PixelChunks; }
        const FString& GetActiveChunk() const { return CurrentStage == EMaterialCompileStage::Vertex ? VertexChunks : PixelChunks; }

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
        // Node is the sampling node, carried only so a UV-gradient fallback warning can name and focus it.
        void TextureSample(const FString& ID, CTexture* Texture, CMaterialInput* Input, CEdGraphNode* Node = nullptr);
        void TextureSampleParameter(const FString& ID, const FName& ParamID, CTexture* Texture, CMaterialInput* Input, CEdGraphNode* Node = nullptr);

        // Raises a non-fatal warning when UVValue carries no analytic derivative, so the deferred pass has
        // to sample it with UV0's gradient. No-op when the derivative is valid.
        void WarnUVGradientFallback(const FInputValue& UVValue, CEdGraphNode* Node);

        // Claim a material texture slot WITHOUT emitting a sample, for nodes that need the bindless index
        // itself (a ray-march samples the same texture many times at its own UVs/mip). Deduped exactly
        // like TextureSample's binding, so a texture used by both paths still binds once.
        int32 BindTexture(CTexture* Texture);
        int32 BindTextureParameter(const FName& ParamID, CTexture* Texture);

        // Texture2DArray sample (Includes/GlobalRHI.slang). NumLayers is the asset's layer count, used
        // to clamp the Slice input at compile time; 0 means "unknown", which skips the clamp.
        void TextureSampleArray(CMaterialGraphNode* Node, int32 TextureIndex, uint32 NumLayers,
                                CMaterialInput* UV, CMaterialInput* Slice);

        // Curve operations. The curve is baked into shader constants at compile time, so no bindings
        // are involved and an edited curve only takes effect on the next material recompile.
        void CurveSample(const FString& ID, const SKeyedCurve& Curve, CMaterialInput* TimeInput);

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

        // Parallax Occlusion Mapping (Includes/ParallaxOcclusion.slang). Emits the height-field march and
        // binds its two outputs by ResolvedVar: the displaced UV, and the sun self-shadow term.
        struct FParallaxInputs
        {
            CMaterialInput* UV           = nullptr;
            CMaterialInput* HeightScale  = nullptr;
            CMaterialInput* MinSamples   = nullptr;
            CMaterialInput* MaxSamples   = nullptr;
            CMaterialInput* LODThreshold = nullptr;
            CMaterialInput* ShadowSamples = nullptr;
            CMaterialInput* ShadowSoftness = nullptr;
        };
        void ParallaxOcclusionMapping(CMaterialGraphNode* Node, int32 HeightTextureIndex, const FParallaxInputs& Inputs,
                                      CMaterialOutput* UVOut, CMaterialOutput* ShadowOut, CMaterialOutput* HeightOut);

        // Mesh distance field (Includes/DistanceField.slang). All three read the CURRENT primitive's own
        // baked SDF volume through its meshlet header, so they are surface-domain, pixel-stage nodes; the
        // helpers below emit the shared per-node preamble that resolves the instance and its volume.
        //
        // A material may use several of these; the preamble is emitted once per node and each binds its
        // own outputs by ResolvedVar, so no cross-node ordering assumption exists.
        void MeshDistanceField(CMaterialGraphNode* Node, CMaterialInput* Position,
                               CMaterialOutput* DistanceOut, CMaterialOutput* GradientOut, CMaterialOutput* ValidOut);

        struct FDistanceFieldOcclusionInputs
        {
            CMaterialInput* Normal    = nullptr;
            CMaterialInput* Radius    = nullptr;
            CMaterialInput* ConeAngle = nullptr;
            CMaterialInput* Intensity = nullptr;
        };
        void MeshDistanceFieldOcclusion(CMaterialGraphNode* Node, const FDistanceFieldOcclusionInputs& Inputs,
                                        int32 StepCount, CMaterialOutput* OcclusionOut);

        void MeshDistanceFieldThickness(CMaterialGraphNode* Node, CMaterialInput* Normal, CMaterialInput* MaxDistance,
                                        int32 StepCount, CMaterialOutput* ThicknessOut, CMaterialOutput* NormalizedOut);

        // Procedural wind vertex displacement (Includes/Wind.slang). Vertex-stage only: the offset it
        // produces is meaningful solely on the path from WorldPositionOffset. Octaves is baked into the
        // emitted call so the fBm loop unrolls; bLODGate picks the distance fade over a constant 1.
        struct FWindInputs
        {
            CMaterialInput* Position   = nullptr;
            CMaterialInput* Direction  = nullptr;
            CMaterialInput* Strength   = nullptr;
            CMaterialInput* Speed      = nullptr;
            CMaterialInput* Frequency  = nullptr;
            CMaterialInput* Lacunarity = nullptr;
            CMaterialInput* Gain       = nullptr;
            CMaterialInput* Mask       = nullptr;
            CMaterialInput* Phase      = nullptr;
            CMaterialInput* Gustiness  = nullptr;
            CMaterialInput* FadeStart  = nullptr;
            CMaterialInput* FadeEnd    = nullptr;
        };
        void WindAnimation(CMaterialGraphNode* Node, const FWindInputs& Inputs, int32 Octaves, bool bLODGate,
                           CMaterialOutput* OffsetOut, CMaterialOutput* WeightOut, CMaterialOutput* NoiseOut);

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

        // Masked materials run the whole pixel graph a second time in the VisBuffer masked pre-pass (just
        // to evaluate Opacity), so expensive nodes are paid for twice. Nodes that care warn on it.
        void SetMasked(bool bInMasked) { bMasked = bInMasked; }
        bool IsMasked() const { return bMasked; }

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

        // Warnings: the graph compiled, but something about it will cost quality or performance at runtime.
        //
        // A SEPARATE vector rather than a severity flag on FError, because HasErrors() is what decides
        // whether the compile succeeded -- every caller of CompileMaterialGraph branches on it. A warning
        // pushed into Errors would fail the material outright, which is the opposite of what a warning is.
        FORCEINLINE void AddWarning(const EdNodeGraph::FError& Warning) { Warnings.push_back(Warning); }
        FORCEINLINE const TVector<EdNodeGraph::FError>& GetWarnings() const { return Warnings; }

        FInputValue GetTypedInputValue(CMaterialInput* Input, float DefaultValue = 0.0f);
        FInputValue GetTypedInputValue(CMaterialInput* Input, const FString& DefaultValueStr);

        // Emit "<TypeStr> <NodeID> = <FetchExpr>;", or an alias to the first variable that already holds
        // <FetchExpr> in this stage. Material parameters are loop- and pixel-invariant, so a repeat fetch is
        // pure redundant load traffic; the alias is a register copy the backend coalesces away.
        void EmitDedupedParamFetch(const FString& TypeStr, const FString& NodeID, const FString& FetchExpr);

        // Publish ID's screen-space derivative. Valid emits the two companion chunks (<ID>_DDX/_DDY) from
        // the supplied expressions; Zero and Unknown emit nothing and only record the state.
        // Call AFTER the value chunk, so the expressions can reference it.
        void RegisterDeriv(const FString& ID, EDerivState State,
                           const FString& DdxExpr = FString(), const FString& DdyExpr = FString());

        // The derivative of a value the caller is about to multiply by a derivative-free scale, i.e. the
        // affine UV case (TexCoords, Panner). Returns Unknown unchanged so it keeps propagating.
        void RegisterScaledDeriv(const FString& ID, const FInputValue& Source, const FString& ScaleExpr);

        // What a texture sample should pass for explicit gradients. Valid -> the value's own pair;
        // anything else -> UV0's, which is what every sample used before this existed.
        void GetUVGradients(const FInputValue& UV, FString& OutDdx, FString& OutDdy) const;

        // Chain rule for + - * / at the EmitBinaryOp choke point. AExpr/BExpr are the already-swizzled
        // operand expressions. Only carries a derivative for UV-shaped values (<= 2 components); anything
        // wider cannot feed a texture coordinate and its companion chunks would not be float2.
        void RegisterBinaryDeriv(const FString& ID, const FString& Op,
                                 const FInputValue& A, const FString& AExpr,
                                 const FInputValue& B, const FString& BExpr,
                                 EMaterialInputType ResultType);

        // Walks back through any passthrough nodes (plain reroutes and named reroutes) to the output
        // pin that actually produces the value. Returns nullptr when the chain dead-ends unconnected
        // or a named reroute resolves to nothing, which callers must treat as "no connection".
        //
        // Anything that reads a connected pin's owning node has to go through this. A reroute emits no
        // variable of its own, so binding to its node name yields an identifier that was never declared.
        static CMaterialOutput* ResolveThroughReroutes(CMaterialOutput* OutputPin);

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

        // Emits "float4x4 <ID>_M = <this stage's instance>.ModelMatrix;" and returns the local's name.
        // The vertex and pixel lanes reach the instance differently; see the definition.
        FString EmitInstanceModelMatrix(const FString& ID);

    private:

        // Per-stage graph body chunks. The active chunk for general node
        // emission is selected by CurrentStage.
        FString PixelChunks;
        FString VertexChunks;

        // Output-node assignments, kept separate so they always land at the end of the substituted block
        // regardless of topo order and the output node can target each stage without driving the cursor.
        FString PixelOutputChunks;
        FString VertexOutputChunks;

        EMaterialCompileStage CurrentStage = EMaterialCompileStage::Pixel;
        bool bMasked = false;

        TVector<TObjectPtr<CTexture>> BoundImages;
        TVector<EdNodeGraph::FError> Errors;
        TVector<EdNodeGraph::FError> Warnings;   // non-fatal; see AddWarning

        THashMap<FName, FScalarParam>  ScalarParameters;
        THashMap<FName, FVectorParam>  VectorParameters;
        THashMap<FName, FTextureParam> TextureParameters;

        // Derivative published per emitted variable name (the key is FInputValue::Value, i.e. the producing
        // node's full name). Absent = Unknown, which is the safe default: consumers fall back to UV0's
        // gradient, exactly what every sample did before derivative propagation existed.
        THashMap<FString, FDerivInfo> DerivByVar;

        // Counted by GetUVGradients (const, hence mutable) and surfaced as FShaderStats::UVGradientFallbacks.
        mutable uint32 UVGradientFallbackCount = 0;

        // Parameter fetches already emitted in the CURRENT stage, keyed by the fetch expression. Two graph
        // nodes reading the same material parameter emit the same right-hand side; the second and later ones
        // alias the first instead of re-issuing the load. Cleared per stage in SetStage, because the pixel
        // and vertex chunks are separate scopes and a variable from one is not in scope in the other.
        THashMap<FString, FString> EmittedParamFetches;

        uint16 NumScalarParams = 0;
        uint16 NumVectorParams = 0;
        uint16 NumTextureParams = 0;

        EMaterialType CurrentMaterialType = EMaterialType::PBR;

        // Active material-function inlining state (see PushInlinePrefix / BeginInlineFunction).
        TVector<FString>            InlinePrefixStack;
        TVector<CMaterialFunction*> InlineFunctionStack;
    };
}
