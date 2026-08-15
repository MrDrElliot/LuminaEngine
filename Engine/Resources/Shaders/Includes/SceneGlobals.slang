#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// Device addresses of every scene buffer + bindless indices for the IBL/shadow textures. Must match
// FSceneRoot in SceneRenderTypes.h (160 B, scalar layout).
struct FSceneRoot
{
    FSceneGlobalData*    SceneData;
    FLightData*          Lights;
    FGPUInstance*        Instances;
    FBoneTransform*      Bones;
    FCluster*            Clusters;
    FMaterialUniforms*   Materials;
    FBillboardInstance*  Billboards;
    FCullView*           CullViews;
    FMeshletDraw*        MeshletDrawListPtr;
    FPreSkinnedVertex*   PreSkinnedVerticesPtr;
    FWidgetInstance*     WidgetsPtr;
    FGPUReflectionProbe* ReflectionProbesPtr;
    FGPUSpline*          SplinesPtr;        // one header per uploaded spline (bSendToGPU)
    FGPUSplinePoint*     SplinePointsPtr;   // shared control-point array; headers carry the slice
    FGPUSplineSample*    SplineSamplesPtr;  // shared arc-length tables
    // Process-wide meshlet header slab. Instances carry a SLOT into this, never an address, so this is
    // the only meshlet-header address that exists anywhere -- and it is republished every frame, which
    // is what lets the slab grow without invalidating a single cached copy. See MeshletHeaderSlab.h.
    FMeshletHeader*      MeshletHeadersPtr;
    // Texture-streaming feedback, one uint per MATERIAL slot. See RequestMaterialResolution.
    uint*                StreamingFeedbackPtr;
    uint  BRDFLutIndex;          // bindless 2D SRV
    uint  SkyIrradianceIndex;    // bindless cube SRV
    // [31:24] = prefilter cube mip count (see SamplePrefilter), [23:0] = bindless cube SRV index.
    // Packed so the per-pixel mip count rides along without a second field.
    uint  SkyPrefilterIndex;
    uint  ShadowCascadeIndex;    // bindless 2D SRV (cascade atlas)
    uint  ShadowAtlasIndex;      // bindless 2D SRV (spot/point atlas)
    uint  SkyCubeIndex;          // bindless cube SRV (full-res sky; sharp near-mirror reflections)
    // [31:24] = probe prefilter mip count, [23:0] = bindless cube-array SRV over every probe's slices.
    uint  ProbeCubeArrayIndex;
    uint  NumReflectionProbes;
    uint  NumSplines;
    uint  StreamingFeedbackCount;
};

// gRHI.Args (GlobalRHI's single 8-byte push-constant pointer) points to this transient block.
struct FRootConstants
{
    FSceneRoot* Root;     // per-frame scene root (transient)
    uint64_t    PassAddr; // per-pass args block (transient), 0 if none
};

// Never load FSceneRoot by value (PhysicalStorageBuffer pointer fields trip spirv-val); return the
// pointer and let field access auto-deref. Slang CSEs this to one root load per function.
FSceneRoot* GetSceneRoot() { return ((FRootConstants*)gRHI.Args).Root; }

// Per-pass args for scene passes; each pass declares its struct and wraps this in a file-local Pass().
Ptr<T> GetPassArgs<T>()
{
    return (T*)((FRootConstants*)gRHI.Args).PassAddr;
}

// Scene buffer accessors (pointers; field/index access auto-derefs).
FSceneGlobalData*   Scene()                 { return GetSceneRoot().SceneData; }
FLightData*         LightData()             { return GetSceneRoot().Lights; }
FGPUInstance*       Instances()             { return GetSceneRoot().Instances; }
FBoneTransform*     Bones()                 { return GetSceneRoot().Bones; }
FCluster*           Clusters()              { return GetSceneRoot().Clusters; }
FMaterialUniforms*  Materials()             { return GetSceneRoot().Materials; }
FBillboardInstance* Billboards()            { return GetSceneRoot().Billboards; }
FCullView*          CullViews()             { return GetSceneRoot().CullViews; }
FMeshletHeader*     MeshletHeaders()        { return GetSceneRoot().MeshletHeadersPtr; }

// Both instance structs carry a header SLOT, but every consumer wants the header. Extensions rather than
// members on the structs themselves: FGPUInstance and FInstanceStatic live in Common.slang, which is
// included BEFORE the scene root exists and so cannot reach it.
//
// Slot 0 is the null header -- no geometry pointers, MeshletCount 0 -- so this always returns something
// readable, including for an instance the CPU never filled and one still naming a mesh that has died.
// That is the whole reason no caller null-checks what comes back.
extension FGPUInstance
{
    property FMeshletHeader* MeshletHeader { get { return MeshletHeaders() + MeshletHeaderSlot; } }
}

extension FInstanceStatic
{
    property FMeshletHeader* MeshletHeader { get { return MeshletHeaders() + MeshletHeaderSlot; } }
}
FMeshletDraw*       MeshletDrawList()       { return GetSceneRoot().MeshletDrawListPtr; }
FPreSkinnedVertex*  PreSkinnedVertices()    { return GetSceneRoot().PreSkinnedVerticesPtr; }
FWidgetInstance*    Widgets()               { return GetSceneRoot().WidgetsPtr; }
FGPUSpline*         Splines()               { return GetSceneRoot().SplinesPtr; }
FGPUSplinePoint*    SplinePoints()          { return GetSceneRoot().SplinePointsPtr; }
FGPUSplineSample*   SplineSamples()         { return GetSceneRoot().SplineSamplesPtr; }
uint                NumSplines()            { return GetSceneRoot().NumSplines; }

// ---------------------------------------------------------------------------------------------------
// Texture-streaming feedback
//
// The GPU already computes the exact LOD it samples; the CPU can only estimate it from bounds,
// distance and texel density, and every one of those is a place to be wrong. So the material lanes
// report what they actually sampled and residency follows THAT.
//
// Keyed by MATERIAL SLOT, one uint per slot, bit N = "some pixel needs this material's textures at
// >= 2^N texels across". ABSOLUTE, not relative to what is resident: the UV derivative alone gives the
// required texel count (one texel per pixel means the texture must be 1/rho texels across its UV range),
// so the shader never needs a texture's size and never touches its descriptor to report.
//
// Per material rather than per texture, which is what makes it ONE atomic per pixel instead of one per
// texture sample. It costs nothing in accuracy: the request is a required RESOLUTION, and the CPU turns
// that into a mip per texture using each texture's own dimensions -- so a 4K albedo and a 512 normal on
// one material correctly land on different mips from the same report.
//
// OR is the right operator -- atomic-friendly, order-independent, and the largest request is just the
// highest set bit. Cleared each frame after readback.
// ---------------------------------------------------------------------------------------------------

// One-hot required resolution for one UV set, or 0 when that set carries no gradient at all.
//
// Zero gradients mean a CONSTANT uv attribute -- a mesh with no second uv set, whose UV1 locals are all
// zero -- not an infinitely magnified surface. Reporting the clamped maximum for those would pin every
// such material's textures at mip 0 forever, so they report nothing instead.
uint StreamingResolutionBit(float2 Ddx, float2 Ddy)
{
    // Kept SQUARED and read straight out of the float exponent: a handful of integer ops rather than two
    // sqrts and a log2. This runs in the deferred material shader, which is register- and SFU-bound.
    const float RhoSq = max(dot(Ddx, Ddx), dot(Ddy, Ddy));
    if (RhoSq <= 0.0f)
    {
        return 0u;
    }

    // For a normal positive float the biased exponent is floor(log2(x)) exactly. We want
    // ceil(log2(1/Rho)) = ceil(-0.5 * log2(RhoSq)); with log2(RhoSq) in [E, E+1) that is exactly
    // -floor(E/2), and >> on a signed int floors. Not an approximation -- it agrees with the ceil(log2())
    // form on every input, including the powers of two where the buckets change.
    const int Exponent = (int)((asuint(RhoSq) >> 23) & 0xFFu) - 127;
    return 1u << (uint)clamp(-(Exponent >> 1), 0, 31);
}

// Called ONCE per pixel by each material lane, not once per sample.
void RequestMaterialResolution(uint MaterialSlot, float2 UV0Ddx, float2 UV0Ddy, float2 UV1Ddx, float2 UV1Ddy)
{
    uint* Feedback = GetSceneRoot().StreamingFeedbackPtr;

    // Range, not just the ~0u sentinel: a stale or corrupt slot is simply out of range, and both index the
    // same buffer, so both have to be rejected before the write. Getting this wrong is an MMU fault, which
    // is how it was found. Uniform across the wave, so this branch costs no divergence.
    if (Feedback == nullptr || MaterialSlot >= GetSceneRoot().StreamingFeedbackCount)
    {
        return;
    }

    const uint Mask = StreamingResolutionBit(UV0Ddx, UV0Ddy) | StreamingResolutionBit(UV1Ddx, UV1Ddy);

    // Unconditional wave collapse: MaterialSlot is wave-uniform in every lane that calls this. The
    // deferred pass is dispatched PER MATERIAL over that material's own pixel list, and a pixel-shader
    // wave is filled from one draw -- and the material is what selects the pipeline, so one draw is one
    // material. One atomic per wave, not per lane.
    const uint WaveMask = WaveActiveBitOr(Mask);
    if (WaveIsFirstLane())
    {
        InterlockedOr(Feedback[MaterialSlot], WaveMask);
    }
}

// HDR panoramas carry +Inf/NaN texels that poison the IBL cubes and spread white through bloom.
// Branchless deliberately: isfinite() short-circuits to two branches per call, and bloom calls it 15x.
float SanitizeRadianceChannel(float X)
{
    const bool bFinite = !isinf(X) & !isnan(X);
    return bFinite ? clamp(X, 0.0, 64000.0) : 0.0;
}

float3 SanitizeRadiance(float3 C)
{
    return float3(SanitizeRadianceChannel(C.r),
                  SanitizeRadianceChannel(C.g),
                  SanitizeRadianceChannel(C.b));
}

// Karis tone-map-then-average firefly suppression for GGX importance sampling. Identity for a single
// sample, so mirror reflections are untouched -- only rough multi-sample gathers get smoothed.
float3 FireflyTonemap(float3 C)   { return C / (1.0 + Luminance(C)); }
float3 FireflyUntonemap(float3 C) { return C / max(1.0 - Luminance(C), 1e-4); }

// Vertex loaders.

FVertexData LoadStaticVertex(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    FMeshletVertex V = reinterpret<FMeshletVertex*>(Header.Vertices)[M.VertexOffset + VertexLocalIdx];

    FVertexData Data;
    Data.Position = DecodeMeshletPosition(M, V.PositionX, V.PositionY, V.PositionZ);
    Data.Normal   = UnpackNormalSNorm16(V.NormalX, V.NormalY, V.NormalZ);
    Data.Tangent  = UnpackTangent(V.Tangent);
    Data.UV       = UnpackUV(V.UV);
    Data.UV1      = UnpackUV(V.UV1);
    Data.Color    = UnpackColor(V.Color);
    return Data;
}

FVertexData DecodePreSkinnedVertex(FPreSkinnedVertex S)
{
    FVertexData Data;
    Data.Position = float3(S.Px, S.Py, S.Pz);
    Data.Normal   = UnpackNormalSNorm16(S.NormalX, S.NormalY, S.NormalZ);
    Data.Tangent  = UnpackTangent(S.Tangent);
    Data.UV       = UnpackUV(S.UV);
    Data.UV1      = UnpackUV(S.UV1);
    Data.Color    = UnpackColor(S.Color);
    return Data;
}

// Skinned reads come entirely from the pre-skinned buffer (Skinning.slang did the blend this frame)
// -- a single structured load like the static path, no per-pass skinning.
FVertexData LoadSkinnedVertex(FMeshlet M, uint VertexLocalIdx, uint SkinnedVertexBase)
{
    return DecodePreSkinnedVertex(PreSkinnedVertices()[SkinnedVertexBase + M.VertexOffset + VertexLocalIdx]);
}

float3 LoadStaticPosition(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    FMeshletVertex V = reinterpret<FMeshletVertex*>(Header.Vertices)[M.VertexOffset + VertexLocalIdx];
    return DecodeMeshletPosition(M, V.PositionX, V.PositionY, V.PositionZ);
}

float3 LoadSkinnedPosition(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx, uint SkinnedVertexBase)
{
    FPreSkinnedVertex S = PreSkinnedVertices()[SkinnedVertexBase + M.VertexOffset + VertexLocalIdx];
    return float3(S.Px, S.Py, S.Pz);
}

// Performs the bone blend for one source vertex; used only by Skinning.slang to fill
// the pre-skinned buffer. Output is object-space (the draw VS still applies ModelMatrix).
FPreSkinnedVertex SkinVertex(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx, uint BoneOffset)
{
    FMeshletSkinnedVertex V = reinterpret<FMeshletSkinnedVertex*>(Header.Vertices)[M.VertexOffset + VertexLocalIdx];

    // The SOURCE stream is still quantized in its meshlet anchor box; the blended OUTPUT is not, which is
    // why FPreSkinnedVertex keeps full-precision floats.
    float3 LocalPos     = DecodeMeshletPosition(M, V.PositionX, V.PositionY, V.PositionZ);
    float3 LocalNormal  = UnpackNormalSNorm16(V.NormalX, V.NormalY, V.NormalZ);
    float4 LocalTangent = UnpackTangent(V.Tangent);

    uint4  JointIndices = UnpackUIntToUVec4(V.JointIndices);
    float4 Weights      = unpack_u8u32(V.JointWeights) / float4(255.0);

    // RENORMALIZE. Importers quantize weights to u8 and keep only four influences, so an unnormalized set
    // scales the blended affine matrix and drags the vertex toward the MODEL ORIGIN by (1 - sum).
    const float WeightSum = dot(Weights, float4(1.0, 1.0, 1.0, 1.0));

    float4 R0;
    float4 R1;
    float4 R2;

    // A joint index is vertex data and nothing bounds it, so a mesh skinned against the wrong skeleton
    // walks off the bone array -- an unmapped read costs the device. Clamping keeps it visibly wrong.
    const uint BoneCount   = Scene().CullData.BoneNum;
    const bool bBonesValid = (Bones() != nullptr) && (BoneCount != 0u);

    if (bBonesValid && WeightSum > 1e-5f)
    {
        Weights /= WeightSum;

        const uint LastBone = BoneCount - 1u;
        const uint4 Joint   = min(BoneOffset + JointIndices, uint4(LastBone, LastBone, LastBone, LastBone));

        FBoneTransform B0 = Bones()[Joint.x];
        FBoneTransform B1 = Bones()[Joint.y];
        FBoneTransform B2 = Bones()[Joint.z];
        FBoneTransform B3 = Bones()[Joint.w];

        // Linear blend of the row-stored skinning matrices, then transform by explicit row dots.
        // Rigid per bone, so the same 3x3 rows rotate Tangent and its handedness sign is preserved.
        R0 = B0.Row0 * Weights.x + B1.Row0 * Weights.y + B2.Row0 * Weights.z + B3.Row0 * Weights.w;
        R1 = B0.Row1 * Weights.x + B1.Row1 * Weights.y + B2.Row1 * Weights.z + B3.Row1 * Weights.w;
        R2 = B0.Row2 * Weights.x + B1.Row2 * Weights.y + B2.Row2 * Weights.z + B3.Row2 * Weights.w;
    }
    else
    {
        // No usable influence at all. Identity rows leave the vertex in BIND POSE, which makes every
        // expression below correct with no special-casing and cannot produce a zero vector to normalize.
        R0 = float4(1.0, 0.0, 0.0, 0.0);
        R1 = float4(0.0, 1.0, 0.0, 0.0);
        R2 = float4(0.0, 0.0, 1.0, 0.0);
    }

    float4 P = float4(LocalPos, 1.0);
    float3 SkinnedPos     = float3(dot(R0, P), dot(R1, P), dot(R2, P));
    float3 SkinnedNormal  = normalize(float3(dot(R0.xyz, LocalNormal),      dot(R1.xyz, LocalNormal),      dot(R2.xyz, LocalNormal)));
    float3 SkinnedTangent = normalize(float3(dot(R0.xyz, LocalTangent.xyz), dot(R1.xyz, LocalTangent.xyz), dot(R2.xyz, LocalTangent.xyz)));

    FPreSkinnedVertex Out;
    Out.Px      = SkinnedPos.x;
    Out.Py      = SkinnedPos.y;
    Out.Pz      = SkinnedPos.z;
    Out.NormalX = FloatToSNorm16(SkinnedNormal.x);
    Out.NormalY = FloatToSNorm16(SkinnedNormal.y);
    Out.NormalZ = FloatToSNorm16(SkinnedNormal.z);
    Out.Tangent = PackTangent(float4(SkinnedTangent, LocalTangent.w));
    Out.UV      = V.UV;     // skin-invariant; carried so the draw VS never reads the source
    Out.UV1     = V.UV1;
    Out.Color   = V.Color;
    return Out;
}

float  GetMaterialScalar (uint MatIndex, uint Index) { return Materials()[MatIndex].Scalars[Index];  }
float4 GetMaterialVec4   (uint MatIndex, uint Index) { return Materials()[MatIndex].Vectors[Index];  }
uint   GetMaterialTexture(uint MatIndex, uint Index) { return Materials()[MatIndex].Textures[Index]; }

FGPUInstance GetInstance(uint InstanceID) { return Instances()[InstanceID]; }

FLight GetLightAt(uint Index)       { return LightData().Lights[Index]; }
uint   GetNumLights()               { return LightData().NumLights;     }
float3 GetSunDirection()            { return LightData().SunDirection;  }
float3 GetAmbientLightColor()       { return LightData().AmbientLight.xyz; }
float  GetAmbientLightIntensity()   { return LightData().AmbientLight.w;   }
bool   HasIBL()                     { return LightData().bHasIBL != 0u;     }

// Media ambient: AmbientLight.xyz is only a sun-height lerp, so prefer the baked skylight cube.
float3 GetVolumetricAmbientRadiance()
{
    float3 Ambient = GetAmbientLightColor();
    if (HasIBL())
    {
        // The convolution is cosine-importance-sampled and mean-averaged, so this is already radiance.
        uint   Index = GetSceneRoot().SkyIrradianceIndex;
        float3 Up    = SampleTextureCubeLevel(Index, SAMPLER_LINEAR_CLAMP, float3(0.0,  1.0, 0.0), 0.0).rgb;
        float3 Down  = SampleTextureCubeLevel(Index, SAMPLER_LINEAR_CLAMP, float3(0.0, -1.0, 0.0), 0.0).rgb;

        // Averaging the two hemisphere means approximates the full-sphere mean a medium scatters.
        Ambient = (Up + Down) * 0.5;
    }
    return Ambient * GetAmbientLightIntensity();
}

// Callers must check Light.ShadowDataIndex != INDEX_NONE first.
float4x4 GetShadowViewProjection(int ShadowDataIndex, uint ViewIndex)
{
    return LightData().Shadows[ShadowDataIndex].ViewProjection[ViewIndex];
}

FLightShadow GetShadowTile(int ShadowDataIndex, uint Face)
{
    return LightData().Shadows[ShadowDataIndex].Shadow[Face];
}

float    GetTime()                  { return Scene().Time;       }
float    GetDeltaTime()             { return Scene().DeltaTime;  }
float    GetNearPlane()             { return Scene().NearPlane;  }
float    GetFarPlane()              { return Scene().FarPlane;   }

// Raw reverse-Z NDC depth -> linear view distance. The single scene-depth linearization: the SceneDepth
// node emits calls to it, so it must stay visible in every material template.
float LinearizeSceneDepth(float RawDepth)
{
    float ZNear = GetNearPlane();
    float ZFar  = GetFarPlane();
    return ZNear * ZFar / max(ZFar - RawDepth * (ZFar - ZNear), 1e-6);
}
uint2    GetScreenSize()            { return Scene().ScreenSize.xy; }

float3   GetCameraPosition()        { return Scene().CameraView.CameraPosition.xyz; }
float3   GetCameraUp()              { return Scene().CameraView.CameraUp.xyz;       }
float3   GetCameraRight()           { return Scene().CameraView.CameraRight.xyz;    }
float3   GetCameraForward()         { return Scene().CameraView.CameraForward.xyz;  }
float4x4 GetCameraView()            { return Scene().CameraView.CameraView;         }
// The cull's camera. Identical to the render camera unless culling is frozen; see FCullData.
float3   GetCullCameraPosition()    { return Scene().CullData.CullCameraPosition.xyz;  }
float4x4 GetCullCameraView()        { return Scene().CullData.CullCameraView;          }
float4x4 GetCullCameraProjection()  { return Scene().CullData.CullCameraProjection;    }
float4x4 GetInverseCameraView()     { return Scene().CameraView.InverseCameraView;  }
float4x4 GetCameraProjection()      { return Scene().CameraView.CameraProjection;   }
float4x4 GetInverseCameraProjection(){ return Scene().CameraView.InverseCameraProjection; }

// [3][3] is 0 for a perspective projection and 1 for an orthographic one, so the mode needs no
// extra plumbing in the scene struct. Diagonal, so it reads the same under either matrix convention.
bool IsCameraOrthographic() { return Scene().CameraView.CameraProjection[3][3] != 0.0; }

float3 WorldToView(float3 WorldPos)
{
    return mul(GetCameraView(), float4(WorldPos, 1.0)).xyz;
}

float3 NormalWorldToView(float3 Normal)
{
    return mul((float3x3)GetCameraView(), Normal);
}

float4 ViewToClip(float3 ViewPos)
{
    return mul(GetCameraProjection(), float4(ViewPos, 1.0));
}

float4 WorldToClip(float3 WorldPos)
{
    return mul(GetCameraProjection(), mul(GetCameraView(), float4(WorldPos, 1.0)));
}

float SineWave(float Speed, float Amplitude)
{
    return Amplitude * sin(GetTime() * Speed);
}

float3 ReconstructWorldPos(float2 UV, float Depth, float4x4 InvProj, float4x4 InvView)
{
    float4 Clip  = float4(UV * 2.0 - 1.0, Depth, 1.0);
    float4 View  = mul(InvProj, Clip);
    View        /= View.w;
    float4 World = mul(InvView, View);
    return World.xyz;
}

float3 ReconstructViewPos(float2 UV, float Depth, float4x4 InvProj)
{
    float4 Clip = float4(UV * 2.0 - 1.0, Depth, 1.0);
    float4 View = mul(InvProj, Clip);
    return View.xyz / View.w;
}

// Froxel index for a fragment from view-space depth + screen position. Mirrors the tile math in
// ClusterBuild/LightCull; clamps stop edge fragments below ZNear from indexing past Clusters().
uint ComputeClusterIndex(float ViewZ, float2 ScreenPos)
{
    uint3  Grid       = Scene().GridSize.xyz;
    uint2  ScreenSize = Scene().ScreenSize.xy;
    float  ZNear      = GetNearPlane();
    float  ZFar       = GetFarPlane();

    float  ZForTile = max(abs(ViewZ), ZNear);
    float  ZTileF   = (log(ZForTile / ZNear) * float(Grid.z)) / log(ZFar / ZNear);
    uint   ZTile    = (uint)clamp(ZTileF, 0.0, float(Grid.z) - 1.0);
    float2 TileSize = float2(ScreenSize) / float2(Grid.xy);
    uint2  TileXY   = (uint2)clamp(ScreenPos / TileSize, float2(0.0, 0.0), float2(Grid.xy) - float2(1.0, 1.0));
    return TileXY.x + TileXY.y * Grid.x + ZTile * Grid.x * Grid.y;
}

// Discrete heatmap for the "Light Complexity" view: how many lights a froxel
// has to shade. Bands chosen so the transition between counts reads at a glance.
float3 LightComplexityColor(uint Count)
{
    if (Count == 0u)  return float3(0.0, 0.0, 0.0);
    if (Count <= 2u)  return float3(0.0, 0.25, 0.6);
    if (Count <= 4u)  return float3(0.0, 0.6, 0.3);
    if (Count <= 8u)  return float3(0.7, 0.8, 0.0);
    if (Count <= 16u) return float3(1.0, 0.5, 0.0);
    return float3(1.0, 0.0, 0.0);
}

// Per-froxel hashed hue for the "Light Clusters" view. Empty froxels are dimmed; froxels at the
// LIGHTS_PER_CLUSTER write cap turn white so light-list saturation is obvious.
float3 ClusterDebugColor(uint ClusterIndex, uint LightCount)
{
    if (LightCount >= (uint)LIGHTS_PER_CLUSTER)
    {
        return float3(1.0, 1.0, 1.0);
    }

    uint H = ClusterIndex * 2654435761u;
    H ^= H >> 16; H *= 2246822519u;
    H ^= H >> 13; H *= 3266489917u;
    H ^= H >> 16;

    float3 Hue = float3(
        float( H        & 0xFFu) / 255.0,
        float((H >>  8) & 0xFFu) / 255.0,
        float((H >> 16) & 0xFFu) / 255.0);

    // Dim empty froxels so populated ones pop against the grid.
    return Hue * ((LightCount == 0u) ? 0.25 : 1.0);
}