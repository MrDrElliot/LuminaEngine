#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// Must match FSceneRoot in SceneRenderTypes.h (200 B, scalar layout); the hottest pointers ride in FRHIRoot.
struct FSceneRoot
{
    FBoneTransform*      Bones;
    // Last frame's pose and transform, copied before this frame's incremental uploads overwrite them.
    FBoneTransform*      PrevBones;
    FTransform3x4*       PrevRetainedTransforms;
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
    // Object-space meshlet spheres for this frame's poses, written by SkinnedMeshletBounds.slang.
    FMeshletSphere*      SkinnedMeshletBoundsPtr;
    // Indexed by retained slot, and the cull reads only SkinnedBoundsBase out of it.
    FSkinnedFrameData*   SkinnedFrameDataPtr;
    // Same index space as SkinnedMeshletBoundsPtr, so one base addresses both.
    FMeshletCone*        SkinnedMeshletConesPtr;
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
    // Entries in the snapshots above, which lag a frame and can be shorter than the live arrays.
    uint  PrevBoneCount;
    uint  PrevRetainedTransformCount;
};

// gRHI.Args (GlobalRHI's single 8-byte push-constant pointer) points to this transient block.
// Straight from the push constant, so this is free. Field access auto-derefs, which keeps FSceneRoot
// from being loaded by value (PhysicalStorageBuffer pointer fields trip spirv-val).
FSceneRoot* GetSceneRoot() { return (FSceneRoot*)gRHI.SceneRoot; }

// Per-pass args for scene passes; each pass declares its struct and wraps this in a file-local Pass().
Ptr<T> GetPassArgs<T>()
{
    return (T*)gRHI.Args;
}

// Scene buffer accessors (pointers; field/index access auto-derefs).
FSceneGlobalData*   Scene()                 { return (FSceneGlobalData*)gRHI.SceneData; }
FLightData*         LightData()             { return (FLightData*)gRHI.Lights; }
FGPUInstance*       Instances()             { return (FGPUInstance*)gRHI.Instances; }
FBoneTransform*     Bones()                 { return GetSceneRoot().Bones; }
FBoneTransform*     PrevBones()             { return GetSceneRoot().PrevBones; }
FTransform3x4*      PrevRetainedTransforms(){ return GetSceneRoot().PrevRetainedTransforms; }
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
FMeshletSphere*     SkinnedMeshletBounds()  { return GetSceneRoot().SkinnedMeshletBoundsPtr; }
FSkinnedFrameData*  SkinnedFrameData()      { return GetSceneRoot().SkinnedFrameDataPtr; }
FMeshletCone*       SkinnedMeshletCones()   { return GetSceneRoot().SkinnedMeshletConesPtr; }
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
    Data.Normal   = UnpackNormal(S.Normal);
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

// The blended affine rows of one vertex's skinning matrix, stored as the same 3x4 as FBoneTransform.
struct FSkinRows
{
    float4 R0;
    float4 R1;
    float4 R2;
};

// Leaves a vertex in BIND POSE, which is the correct answer whenever no usable influence exists.
FSkinRows IdentitySkinRows()
{
    FSkinRows Rows;
    Rows.R0 = float4(1.0, 0.0, 0.0, 0.0);
    Rows.R1 = float4(0.0, 1.0, 0.0, 0.0);
    Rows.R2 = float4(0.0, 0.0, 1.0, 0.0);
    return Rows;
}

FSkinRows UnpackBoneRows(FBoneTransform B)
{
    const float2 A = float2(f16tof32(B.Rot[0] & 0xFFFFu), f16tof32(B.Rot[0] >> 16u));
    const float2 C = float2(f16tof32(B.Rot[1] & 0xFFFFu), f16tof32(B.Rot[1] >> 16u));
    const float2 D = float2(f16tof32(B.Rot[2] & 0xFFFFu), f16tof32(B.Rot[2] >> 16u));
    const float2 E = float2(f16tof32(B.Rot[3] & 0xFFFFu), f16tof32(B.Rot[3] >> 16u));
    const float  F = f16tof32(B.Rot[4] & 0xFFFFu);

    FSkinRows Rows;
    Rows.R0 = float4(A.x, A.y, C.x, B.Tx);
    Rows.R1 = float4(C.y, D.x, D.y, B.Ty);
    Rows.R2 = float4(E.x, E.y, F,   B.Tz);
    return Rows;
}

// RENORMALIZES: an unnormalized set drags the vertex toward the MODEL ORIGIN by (1 - sum).
FSkinRows BlendSkinRows(FBoneTransform B0, FBoneTransform B1, FBoneTransform B2, FBoneTransform B3,
                        float4 Weights)
{
    const float WeightSum = dot(Weights, float4(1.0, 1.0, 1.0, 1.0));
    if (WeightSum <= 1e-5f)
    {
        return IdentitySkinRows();
    }

    const float4 W = Weights / WeightSum;

    const FSkinRows U0 = UnpackBoneRows(B0);
    const FSkinRows U1 = UnpackBoneRows(B1);
    const FSkinRows U2 = UnpackBoneRows(B2);
    const FSkinRows U3 = UnpackBoneRows(B3);

    // Rigid per bone, so these same rows rotate Tangent and its handedness sign is preserved.
    FSkinRows Rows;
    Rows.R0 = U0.R0 * W.x + U1.R0 * W.y + U2.R0 * W.z + U3.R0 * W.w;
    Rows.R1 = U0.R1 * W.x + U1.R1 * W.y + U2.R1 * W.z + U3.R1 * W.w;
    Rows.R2 = U0.R2 * W.x + U1.R2 * W.y + U2.R2 * W.z + U3.R2 * W.w;
    return Rows;
}

// One source vertex, decoded. Slots address the OWNING MESHLET's bone palette, not the skeleton.
struct FSkinSource
{
    float3 Position;
    float3 Normal;
    float4 Tangent;
    uint4  Slots;
    float4 Weights;
    uint   UV;
    uint   UV1;
    uint   Color;
};

FSkinSource LoadSkinSource(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    FMeshletSkinnedVertex V = reinterpret<FMeshletSkinnedVertex*>(Header.Vertices)[M.VertexOffset + VertexLocalIdx];

    FSkinSource S;

    // The SOURCE is quantized in its meshlet anchor box; the blended OUTPUT is not, hence full floats out.
    S.Position = DecodeMeshletPosition(M, V.PositionX, V.PositionY, V.PositionZ);
    S.Normal   = UnpackNormalSNorm16(V.NormalX, V.NormalY, V.NormalZ);
    S.Tangent  = UnpackTangent(V.Tangent);
    S.Slots    = UnpackUIntToUVec4(V.JointIndices);
    S.Weights  = unpack_u8u32(V.JointWeights) / float4(255.0);
    S.UV       = V.UV;
    S.UV1      = V.UV1;
    S.Color    = V.Color;
    return S;
}

// Count 0 means the mesh carries no palette, and every caller reads that as "skin to bind pose".
FMeshletBonePalette GetBonePalette(FMeshletHeader* Header, uint MeshletIndex)
{
    FMeshletBonePalette Palette;
    Palette.Offset = 0u;
    Palette.Count  = 0u;

    if (Header.BonePalettes != nullptr && Header.BoneIndices != nullptr && MeshletIndex < Header.MeshletCount)
    {
        Palette = Header.BonePalettes[MeshletIndex];
    }
    return Palette;
}

// Both clamps matter: slot and bone index are unbounded imported data. Requires Palette.Count > 0.
uint ResolvePaletteBone(FMeshletHeader* Header, FMeshletBonePalette Palette, uint Slot, uint BoneOffset)
{
    const uint Local  = min(Slot, Palette.Count - 1u);
    const uint Global = BoneOffset + Header.BoneIndices[Palette.Offset + Local];
    return min(Global, Scene().CullData.BoneNum - 1u);
}

// True when the bone arena and this meshlet's palette can both actually be read.
bool CanSkinMeshletWith(FBoneTransform* BoneArray, FMeshletBonePalette Palette, uint BoneOffset)
{
    return (BoneArray != nullptr) && (Scene().CullData.BoneNum != 0u) && (Palette.Count != 0u)
        && (BoneOffset != kNoBoneSlice);
}

bool CanSkinMeshlet(FMeshletBonePalette Palette, uint BoneOffset)
{
    return CanSkinMeshletWith(Bones(), Palette, BoneOffset);
}

FPreSkinnedVertex SkinWithRows(FSkinSource S, FSkinRows Rows)
{
    float4 P = float4(S.Position, 1.0);
    float3 SkinnedPos     = float3(dot(Rows.R0, P), dot(Rows.R1, P), dot(Rows.R2, P));
    float3 SkinnedNormal  = normalize(float3(dot(Rows.R0.xyz, S.Normal),      dot(Rows.R1.xyz, S.Normal),      dot(Rows.R2.xyz, S.Normal)));
    float3 SkinnedTangent = normalize(float3(dot(Rows.R0.xyz, S.Tangent.xyz), dot(Rows.R1.xyz, S.Tangent.xyz), dot(Rows.R2.xyz, S.Tangent.xyz)));

    FPreSkinnedVertex Out;
    Out.Px      = SkinnedPos.x;
    Out.Py      = SkinnedPos.y;
    Out.Pz      = SkinnedPos.z;
    Out.Normal  = PackNormal(SkinnedNormal);
    Out.Tangent = PackTangent(float4(SkinnedTangent, S.Tangent.w));
    Out.UV      = S.UV;     // skin-invariant; carried so the draw VS never reads the source
    Out.UV1     = S.UV1;
    Out.Color   = S.Color;
    return Out;
}

// The per-pass overflow path, where there is no workgroup to share a staged palette across.
FPreSkinnedVertex SkinVertexWith(FBoneTransform* BoneArray, FMeshletHeader* Header, FMeshlet M,
                                 uint MeshletIndex, uint VertexLocalIdx, uint BoneOffset)
{
    const FSkinSource S = LoadSkinSource(Header, M, VertexLocalIdx);
    const FMeshletBonePalette Palette = GetBonePalette(Header, MeshletIndex);

    FSkinRows Rows = IdentitySkinRows();
    if (CanSkinMeshletWith(BoneArray, Palette, BoneOffset))
    {
        Rows = BlendSkinRows(
            BoneArray[ResolvePaletteBone(Header, Palette, S.Slots.x, BoneOffset)],
            BoneArray[ResolvePaletteBone(Header, Palette, S.Slots.y, BoneOffset)],
            BoneArray[ResolvePaletteBone(Header, Palette, S.Slots.z, BoneOffset)],
            BoneArray[ResolvePaletteBone(Header, Palette, S.Slots.w, BoneOffset)],
            S.Weights);
    }

    return SkinWithRows(S, Rows);
}

FPreSkinnedVertex SkinVertex(FMeshletHeader* Header, FMeshlet M, uint MeshletIndex, uint VertexLocalIdx,
                             uint BoneOffset)
{
    return SkinVertexWith(Bones(), Header, M, MeshletIndex, VertexLocalIdx, BoneOffset);
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
float    GetPrevTime()              { return Scene().PrevTime;   }

// Negative means "use this frame's clock"; the motion-vector lane sets it to re-evaluate WPO at t-1.
static float gMaterialVertexTime = -1.0;
float MaterialTime() { return gMaterialVertexTime >= 0.0 ? gMaterialVertexTime : Scene().Time; }
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
float4x4 GetPrevViewProjection()    { return Scene().CameraView.PrevViewProjection;    }

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
    return Amplitude * sin(MaterialTime() * Speed);
}

float3 ReconstructWorldPos(float2 UV, float Depth, float4x4 InvProj, float4x4 InvView)
{
    float4 Clip  = float4(UV * 2.0 - 1.0, Depth, 1.0);
    float4 View  = mul(InvProj, Clip);
    View        /= View.w;
    float4 World = mul(InvView, View);
    return World.xyz;
}

// Jimenez's interleaved gradient noise, shifted per frame so two subsamples see decorrelated patterns.
float InterleavedGradientNoise(float2 PixelPos)
{
    // Zero phase reproduces the fixed screen-space pattern exactly, which is what a lone frame wants.
    const float2 Shifted = PixelPos + 5.588238 * float(Scene().TemporalPhase);
    return frac(52.9829189 * frac(dot(Shifted, float2(0.06711056, 0.00583715))));
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