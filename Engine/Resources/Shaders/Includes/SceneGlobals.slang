#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// The mip count rides in the top byte, so a roughness LOD range needs no per-pixel GetDimensions.
struct FMippedTextureCube
{
    uint Packed;

    FTextureCube Texture() { return FTextureCube(Packed & 0x00FFFFFFu); }
    uint MipCount()        { return Packed >> 24; }
};

struct FMippedTextureCubeArray
{
    uint Packed;

    FTextureCubeArray Texture() { return FTextureCubeArray(Packed & 0x00FFFFFFu); }
    uint MipCount()             { return Packed >> 24; }
};

// Must match FSceneRoot in SceneRenderTypes.h (416 B, scalar layout).
struct FSceneRoot
{
    GPUSpan<FBoneTransform>      Bones;
    // Last frame's copies, which can be shorter than the live arrays.
    GPUSpan<FBoneTransform>      PrevBones;
    GPUSpan<FTransform3x4>       PrevRetainedTransforms;
    GPUSpan<FCluster>            Clusters;
    // Per-view light bitmasks, GetClusterMaskWords() words per cluster.
    GPUSpan<uint>                ClusterLightMasks;
    // Per cluster, its first non-empty mask word in the low 16 bits and one past its last in the high 16.
    GPUSpan<uint>                ClusterWordRanges;
    FMaterialTable               Materials;
    GPUSpan<FMaterialCollection> Collections;
    GPUSpan<FBillboardInstance>  Billboards;
    GPUSpan<FCullView>           CullViews;
    GPUSpan<FMeshletDraw>        MeshletDrawList;
    GPUSpan<FPreSkinnedVertex>   PreSkinnedVertices;
    GPUSpan<FWidgetInstance>     Widgets;
    GPUSpan<FGPUReflectionProbe> ReflectionProbes;
    GPUSpan<FGPUSpline>          Splines;
    GPUSpan<FGPUSplinePoint>     SplinePoints;   // each header carries its slice
    GPUSpan<FGPUSplineSample>    SplineSamples;
    // Instances hold a slot into this, never an address, so the slab can grow and be republished each frame.
    GPUSpan<FMeshletHeader>      MeshletHeaders;
    // One uint per material slot, see RequestMaterialResolution.
    GPUSpan<uint>                StreamingFeedback;
    // Object-space meshlet spheres for this frame's poses, written by SkinnedMeshletBounds.slang.
    GPUSpan<FMeshletSphere>      SkinnedMeshletBounds;
    // Indexed by retained slot, and the cull reads only SkinnedBoundsBase out of it.
    GPUSpan<FSkinnedFrameData>   SkinnedFrameData;
    // Same index space as SkinnedMeshletBounds, so one base addresses both.
    GPUSpan<FMeshletCone>        SkinnedMeshletCones;
    // Parallel to PreSkinnedVertices, and empty when no view wants motion vectors.
    GPUSpan<FPrevSkinnedPosition> PreSkinnedPrevPositions;

    FTexture2D         BRDFLut;
    FTextureCube       SkyIrradiance;
    FMippedTextureCube SkyPrefilter;
    FTexture2D         ShadowCascadeTexture;
    FTexture2D         ShadowAtlasTexture;
    // Full resolution, for sharp near-mirror reflections.
    FTextureCube            SkyCube;
    FMippedTextureCubeArray ProbeCubeArray;
    uint                    _Pad0;
};

FSceneRoot* GetSceneRoot() { return GetSceneRootPointer<FSceneRoot>(); }

// Each pass declares its args struct and wraps this in a file-local Pass().
Ptr<T> GetPassArgs<T>()
{
    return GetArgs<T>();
}

FSceneGlobalData*   Scene()                 { return GetSceneDataPointer<FSceneGlobalData>(); }

// A post-process material samples at display resolution, so it opts out with MATERIAL_NO_MIP_BIAS.
float GetMaterialMipBias()
{
#if defined(MATERIAL_NO_MIP_BIAS)
    return 0.0;
#else
    return Scene().TextureMipBias;
#endif
}
FLightData*         LightData()             { return GetLightsPointer<FLightData>(); }
FGPUInstance*       Instances()             { return GetInstancesPointer<FGPUInstance>(); }

GPUSpan<FBoneTransform>      Bones()                 { return GetSceneRoot().Bones; }
GPUSpan<FBoneTransform>      PrevBones()             { return GetSceneRoot().PrevBones; }
GPUSpan<FTransform3x4>       PrevRetainedTransforms(){ return GetSceneRoot().PrevRetainedTransforms; }
GPUSpan<FCluster>            Clusters()              { return GetSceneRoot().Clusters; }
GPUSpan<uint>                ClusterLightMasks()     { return GetSceneRoot().ClusterLightMasks; }
GPUSpan<uint>                ClusterWordRanges()     { return GetSceneRoot().ClusterWordRanges; }
FMaterialTable               Materials()             { return GetSceneRoot().Materials; }
GPUSpan<FMaterialCollection> Collections()           { return GetSceneRoot().Collections; }
GPUSpan<FBillboardInstance>  Billboards()            { return GetSceneRoot().Billboards; }
GPUSpan<FCullView>           CullViews()             { return GetSceneRoot().CullViews; }

// One word, where Get would fetch all 128 bytes of the view to test a flag.
bool CullViewHasFlag(uint ViewIndex, uint Flag)
{
    return (asuint(CullViews().At(ViewIndex).ViewOriginAndFlags.w) & Flag) == Flag;
}
GPUSpan<FMeshletHeader>      MeshletHeaders()        { return GetSceneRoot().MeshletHeaders; }

// Slot 0 is the null header and the slot is clamped, so this always returns something readable.
extension FGPUInstance
{
    property FMeshletHeader* MeshletHeader
    {
        get { return MeshletHeaders().At(min(MeshletHeaderSlot, MeshletHeaders().Count - 1u)); }
    }
}

extension FInstanceStatic
{
    property FMeshletHeader* MeshletHeader
    {
        get { return MeshletHeaders().At(min(MeshletHeaderSlot, MeshletHeaders().Count - 1u)); }
    }
}
GPUSpan<FMeshletDraw>        MeshletDrawList()       { return GetSceneRoot().MeshletDrawList; }
GPUSpan<FPreSkinnedVertex>   PreSkinnedVertices()    { return GetSceneRoot().PreSkinnedVertices; }
GPUSpan<FPrevSkinnedPosition> PreSkinnedPrevPositions() { return GetSceneRoot().PreSkinnedPrevPositions; }
GPUSpan<FMeshletSphere>      SkinnedMeshletBounds()  { return GetSceneRoot().SkinnedMeshletBounds; }
GPUSpan<FSkinnedFrameData>   SkinnedFrameData()      { return GetSceneRoot().SkinnedFrameData; }
GPUSpan<FMeshletCone>        SkinnedMeshletCones()   { return GetSceneRoot().SkinnedMeshletCones; }
GPUSpan<FWidgetInstance>     Widgets()               { return GetSceneRoot().Widgets; }
GPUSpan<FGPUSpline>          Splines()               { return GetSceneRoot().Splines; }
GPUSpan<FGPUSplinePoint>     SplinePoints()          { return GetSceneRoot().SplinePoints; }
GPUSpan<FGPUSplineSample>    SplineSamples()         { return GetSceneRoot().SplineSamples; }
GPUSpan<FGPUReflectionProbe> ReflectionProbes()      { return GetSceneRoot().ReflectionProbes; }
uint                NumSplines()            { return GetSceneRoot().Splines.Count; }

// Streaming feedback bit N means some pixel needs this material's textures at 2^N or more texels across.

// Zero gradients mean a constant UV attribute (no second UV set), so they report nothing rather than pin mip 0.
uint StreamingResolutionBit(float2 Ddx, float2 Ddy)
{
    // Kept squared and read from the float exponent, sparing two sqrts and a log2 in an SFU-bound shader.
    const float RhoSq = max(dot(Ddx, Ddx), dot(Ddy, Ddy));
    if (RhoSq <= 0.0f)
    {
        return 0u;
    }

    // With E the unbiased exponent of RhoSq, ceil(-0.5 * log2(RhoSq)) is exactly -floor(E/2).
    const int Exponent = (int)((asuint(RhoSq) >> 23) & 0xFFu) - 127;
    return 1u << (uint)clamp(-(Exponent >> 1), 0, 31);
}

// Call once per pixel per material lane, not once per sample.
void RequestMaterialResolution(uint MaterialSlot, float2 UV0Ddx, float2 UV0Ddy, float2 UV1Ddx, float2 UV1Ddy)
{
    // A stale or corrupt slot faults the MMU, so range-check rather than test only the ~0u sentinel.
    if (!GetSceneRoot().StreamingFeedback.Contains(MaterialSlot))
    {
        return;
    }

#ifndef SHADING_COMPUTE
    // A helper lane's atomic is discarded, so one must never be the lane elected to publish the mask.
    if (IsHelperLane())
    {
        return;
    }
#endif

    // Scaled like the samples, so streaming loads the sharper mips a negative bias reaches for.
    const float GradScale = exp2(GetMaterialMipBias());
    const uint Mask = StreamingResolutionBit(UV0Ddx * GradScale, UV0Ddy * GradScale) | StreamingResolutionBit(UV1Ddx * GradScale, UV1Ddy * GradScale);

    // One pass per distinct slot, with a wave-wide exit since a per-lane exit lets AMD's compiler merge every slot into one.
    bool bPending = true;
    [loop]
    while (WaveActiveAnyTrue(bPending))
    {
        if (bPending)
        {
            const uint Candidate = WaveReadLaneFirst(MaterialSlot);
            if (Candidate == MaterialSlot)
            {
                const uint WaveMask = WaveActiveBitOr(Mask);
                if (WaveIsFirstLane())
                {
                    InterlockedOr(GetSceneRoot().StreamingFeedback.At(Candidate)[0], WaveMask);
                }
                bPending = false;
            }
        }
    }
}

// Strips the +Inf/NaN texels of HDR panoramas, branchless since bloom calls it 15x.
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

// Karis firefly suppression, the identity for a single sample so mirror reflections are untouched.
float3 FireflyTonemap(float3 C)   { return C / (1.0 + Luminance(C)); }
float3 FireflyUntonemap(float3 C) { return C / max(1.0 - Luminance(C), 1e-4); }

// Where a static meshlet's local vertex sits in the mesh's shared streams; mirrors Lumina::GetStaticVertexIndex.
uint StaticVertexIndex(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    if (M.bShortVertexRefs)
    {
        const uint Word = Header.VertexRefs[M.VertexOffset + (VertexLocalIdx >> 1u)];
        return M.BaseVertex + ((VertexLocalIdx & 1u) != 0u ? (Word >> 16u) : (Word & 0xFFFFu));
    }
    return M.BaseVertex + Header.VertexRefs[M.VertexOffset + VertexLocalIdx];
}

float3 LoadStaticPositionAt(FMeshletHeader* Header, uint VertexIndex)
{
    return DecodeMeshPosition(Header.PositionGrid, loadAligned<8>(Header.Positions + VertexIndex));
}

float3 LoadStaticPosition(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    return LoadStaticPositionAt(Header, StaticVertexIndex(Header, M, VertexLocalIdx));
}

// An absent UV1 reads as UV0 and an absent color as white, which is what the importer would have written.
void LoadOptionalStaticStreams(FMeshletHeader* Header, uint VertexIndex, inout FVertexData Data)
{
    const uint Flags = Header.VertexStreamFlags;
    Data.UV1   = (Flags & MESH_VERTEX_STREAM_UV1) != 0u ? UnpackUV(Header.VertexUV1s[VertexIndex]) : Data.UV;
    Data.Color = (Flags & MESH_VERTEX_STREAM_COLOR) != 0u ? UnpackColor(Header.VertexColors[VertexIndex]) : float4(1.0, 1.0, 1.0, 1.0);
}

// Field by field through the pointer, so whatever a pass leaves unused is never fetched at all.
FVertexData LoadStaticVertex(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    const uint             VertexIndex = StaticVertexIndex(Header, M, VertexLocalIdx);
    FMeshVertexAttributes* V           = reinterpret<FMeshVertexAttributes*>(Header.Vertices) + VertexIndex;

    FVertexData Data;
    Data.Position = LoadStaticPositionAt(Header, VertexIndex);
    Data.Normal   = UnpackNormal(V.Normal);
    Data.Tangent  = UnpackTangent(V.Tangent);
    Data.UV       = UnpackUV(V.UV);
    LoadOptionalStaticStreams(Header, VertexIndex, Data);
    return Data;
}

// One load of the attributes, for a pass that consumes every one and would otherwise issue many narrow loads.
FVertexData LoadStaticVertexWhole(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    const uint                  VertexIndex = StaticVertexIndex(Header, M, VertexLocalIdx);
    const FMeshVertexAttributes V           = reinterpret<FMeshVertexAttributes*>(Header.Vertices)[VertexIndex];

    FVertexData Data;
    Data.Position = LoadStaticPositionAt(Header, VertexIndex);
    Data.Normal   = UnpackNormal(V.Normal);
    Data.Tangent  = UnpackTangent(V.Tangent);
    Data.UV       = UnpackUV(V.UV);
    LoadOptionalStaticStreams(Header, VertexIndex, Data);
    return Data;
}

// Bind-pose position for a mesh that may be skinned, whose vertices are copied per meshlet instead of shared.
float3 LoadBindPosePosition(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx)
{
    if (Header.BonePaletteCount != 0u)
    {
        FMeshletSkinnedVertex* V = reinterpret<FMeshletSkinnedVertex*>(Header.Vertices) + (M.VertexOffset + VertexLocalIdx);
        return DecodeMeshPosition(Header.PositionGrid, V.Position);
    }
    return LoadStaticPosition(Header, M, VertexLocalIdx);
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

// Reads the pre-skinned buffer that Skinning.slang wrote this frame.
FVertexData LoadSkinnedVertex(FMeshlet M, uint VertexLocalIdx, uint SkinnedVertexBase)
{
    return DecodePreSkinnedVertex(PreSkinnedVertices().GetAligned(SkinnedVertexBase + M.VertexOffset + VertexLocalIdx));
}

float3 LoadSkinnedPosition(FMeshletHeader* Header, FMeshlet M, uint VertexLocalIdx, uint SkinnedVertexBase)
{
    FPreSkinnedVertex S = PreSkinnedVertices().GetAligned(SkinnedVertexBase + M.VertexOffset + VertexLocalIdx);
    return float3(S.Px, S.Py, S.Pz);
}

// The blended affine rows of one vertex's skinning matrix, stored as the same 3x4 as FBoneTransform.
struct FSkinRows
{
    float4 R0;
    float4 R1;
    float4 R2;
};

// Leaves a vertex in bind pose, the correct answer whenever no usable influence exists.
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

// Renormalizes, since an unnormalized set drags the vertex toward the model origin by (1 - sum).
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

// Slots address the owning meshlet's bone palette, not the skeleton.
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

    S.Position = DecodeMeshPosition(Header.PositionGrid, V.Position);
    S.Normal   = UnpackNormal(V.Normal);
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

    if (MeshletIndex < Header.BonePaletteCount)
    {
        Palette = Header.BonePalettes[MeshletIndex];
    }
    return Palette;
}

// Slot and bone index are unbounded imported data, so both clamp. Requires Palette.Count > 0.
uint ResolvePaletteBone(FMeshletHeader* Header, FMeshletBonePalette Palette, uint Slot, uint BoneOffset)
{
    const uint Local  = min(Slot, Palette.Count - 1u);
    const uint Global = BoneOffset + Header.BoneIndices[Palette.Offset + Local];
    return min(Global, Scene().CullData.BoneNum - 1u);
}

// The caller's arena does not matter here since PrevBones is gated by its own, shorter, count.
bool CanSkinMeshlet(FMeshletBonePalette Palette, uint BoneOffset)
{
    return (Scene().CullData.BoneNum != 0u) && (Palette.Count != 0u)
        && (BoneOffset != kNoBoneSlice);
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
    Out.UV      = S.UV;     // carried so the draw VS never reads the source
    Out.UV1     = S.UV1;
    Out.Color   = S.Color;
    return Out;
}

// The per-pass overflow path, where there is no workgroup to share a staged palette across.
FPreSkinnedVertex SkinVertexWith(GPUSpan<FBoneTransform> BoneArray, FMeshletHeader* Header, FMeshlet M,
                                 uint MeshletIndex, uint VertexLocalIdx, uint BoneOffset)
{
    const FSkinSource S = LoadSkinSource(Header, M, VertexLocalIdx);
    const FMeshletBonePalette Palette = GetBonePalette(Header, MeshletIndex);

    FSkinRows Rows = IdentitySkinRows();
    if (CanSkinMeshlet(Palette, BoneOffset))
    {
        Rows = BlendSkinRows(
            BoneArray.GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.x, BoneOffset)),
            BoneArray.GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.y, BoneOffset)),
            BoneArray.GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.z, BoneOffset)),
            BoneArray.GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.w, BoneOffset)),
            S.Weights);
    }

    return SkinWithRows(S, Rows);
}

FPreSkinnedVertex SkinVertex(FMeshletHeader* Header, FMeshlet M, uint MeshletIndex, uint VertexLocalIdx,
                             uint BoneOffset)
{
    return SkinVertexWith(Bones(), Header, M, MeshletIndex, VertexLocalIdx, BoneOffset);
}

float  GetMaterialScalar(uint MatIndex, uint Index)  { return Materials().Scalar(MatIndex, Index); }
float4 GetMaterialVec4(uint MatIndex, uint Index)    { return Materials().Vec4(MatIndex, Index); }
uint   GetMaterialTexture(uint MatIndex, uint Index) { return Materials().Texture(MatIndex, Index); }
uint   GetMaterialFlags(uint MatIndex)               { return Materials().GetHeader(MatIndex).Flags; }
float  GetMaterialOpacityClip(uint MatIndex)         { return Materials().GetHeader(MatIndex).OpacityClipValue; }

// Slot is which of the material's own bindings (0 or 1), not the collection's index in the table.
uint GetMaterialCollectionIndex(uint MatIndex, uint Slot) { return Materials().GetHeader(MatIndex).CollectionIndex(Slot); }

float GetCollectionScalar(uint MatIndex, uint Slot, uint Index)
{
    const uint CollectionIndex = GetMaterialCollectionIndex(MatIndex, Slot);
    if (!Collections().Contains(CollectionIndex))
    {
        return 0.0;
    }
    return Collections().At(CollectionIndex).Scalars[Index];
}

float4 GetCollectionVec4(uint MatIndex, uint Slot, uint Index)
{
    const uint CollectionIndex = GetMaterialCollectionIndex(MatIndex, Slot);
    if (!Collections().Contains(CollectionIndex))
    {
        return float4(0.0);
    }
    return loadAligned<16>(&Collections().At(CollectionIndex).Vectors[Index]);
}

FGPUInstance GetInstance(uint InstanceID) { return Instances()[InstanceID]; }

// FGPUInstance is 128 B. A caller after one word reads it through these, not off a by-value GetInstance.
uint GetInstanceMaterialIndex(uint InstanceID) { return Instances()[InstanceID].MaterialIndex; }
uint GetInstanceEntityID(uint InstanceID)      { return Instances()[InstanceID].EntityID; }

FLight GetLightAt(uint Index)       { return LightData().Lights.GetAligned(Index); }
uint   GetNumLights()               { return LightData().Lights.Count;  }

// Where a direction or world position lands in a light function's tile, in that tile's own 0-1 space.
float2 LightFunctionTileUV(FLight Light, FLightFunction Function, float3 L, float3 WorldPos)
{
    if (HasFlag(Light.Flags, ELightFlags::Directional))
    {
        return frac(float2(dot(WorldPos, Function.Right.xyz), dot(WorldPos, Function.Up.xyz)) * Function.Right.w);
    }

    // L points at the light, so the light sees the surface along -L.
    const float3 FromLight = -L;
    if (HasFlag(Light.Flags, ELightFlags::Spot))
    {
        const float  Depth = max(dot(FromLight, -Light.Direction), 1e-4);
        const float2 Plane = float2(dot(FromLight, Function.Right.xyz), dot(FromLight, Function.Up.xyz)) / Depth;
        return Plane * (Function.Right.w * 0.5) + 0.5;
    }
    return OctEncode01(FromLight);
}

// The light function's color mask for this light at WorldPos, or white for a light without one.
float3 LightFunctionMask(FLight Light, float3 L, float3 WorldPos)
{
    if (!HasFlag(Light.Flags, ELightFlags::LightFunction))
    {
        return float3(1.0);
    }

    const uint Slot = (Light.Flags >> LIGHT_FUNCTION_SLOT_SHIFT) & LIGHT_FUNCTION_SLOT_MASK;
    if (!LightData().LightFunctions.Contains(Slot))
    {
        return float3(1.0);
    }

    // Inset by half a texel, so bilinear filtering never reads the neighboring tile.
    const float  HalfTexel = 0.5 / float(LIGHT_FUNCTION_TILE_SIZE);
    const float2 TileUV    = clamp(LightFunctionTileUV(Light, LightData().LightFunctions.Get(Slot), L, WorldPos), HalfTexel, 1.0 - HalfTexel);
    const float2 Tile      = float2(Slot % LIGHT_FUNCTION_ATLAS_TILES, Slot / LIGHT_FUNCTION_ATLAS_TILES);
    const float2 AtlasUV   = (Tile + TileUV) / float(LIGHT_FUNCTION_ATLAS_TILES);
    return LightData().LightFunctionAtlas.SampleLevel(Samplers::LinearClamp, AtlasUV, 0.0).rgb;
}
float3 GetSunDirection()            { return LightData().SunDirection;  }
float3 GetSkySunDirection()         { return LightData().SkySunDirection; }
float3 GetAmbientLightColor()       { return LightData().AmbientLight.xyz; }
float  GetAmbientLightIntensity()   { return LightData().AmbientLight.w;   }
bool   HasIBL()                     { return LightData().bHasIBL != 0u;     }

// AmbientLight.xyz is only a sun-height lerp, so prefer the baked skylight cube.
float3 GetVolumetricAmbientRadiance()
{
    float3 Ambient = GetAmbientLightColor();
    if (HasIBL())
    {
        // The convolution is cosine-importance-sampled and mean-averaged, so this is already radiance.
        FTextureCube Irradiance = GetSceneRoot().SkyIrradiance;
        float3       Up    = Irradiance.SampleLevel(Samplers::LinearClamp, float3(0.0,  1.0, 0.0), 0.0).rgb;
        float3       Down  = Irradiance.SampleLevel(Samplers::LinearClamp, float3(0.0, -1.0, 0.0), 0.0).rgb;

        // Averaging the two hemisphere means approximates the full-sphere mean a medium scatters.
        Ambient = (Up + Down) * 0.5;
    }
    return Ambient * GetAmbientLightIntensity();
}

// An INDEX_NONE or stale ShadowDataIndex is rejected by the span's count rather than special-cased.
float4x4 GetShadowViewProjection(int ShadowDataIndex, uint ViewIndex)
{
    if (!LightData().Shadows.Contains((uint)ShadowDataIndex))
    {
        return (float4x4)0.0;
    }
    return loadAligned<16>(&LightData().Shadows.At((uint)ShadowDataIndex).ViewProjection[min(ViewIndex, 5u)]);
}

FLightShadow GetShadowTile(int ShadowDataIndex, uint Face)
{
    if (!LightData().Shadows.Contains((uint)ShadowDataIndex))
    {
        FLightShadow Empty = {};
        return Empty;
    }
    return loadAligned<16>(&LightData().Shadows.At((uint)ShadowDataIndex).Shadow[min(Face, 5u)]);
}

float    GetTime()                  { return Scene().Time;       }
float    GetPrevTime()              { return Scene().PrevTime;   }

// Negative means "use this frame's clock"; the motion-vector lane sets it to re-evaluate WPO at t-1.
static float MaterialTimeOverride = -1.0;
float MaterialTime() { return MaterialTimeOverride >= 0.0 ? MaterialTimeOverride : Scene().Time; }
float    GetDeltaTime()             { return Scene().DeltaTime;  }
float    GetNearPlane()             { return Scene().NearPlane;  }
float    GetFarPlane()              { return Scene().FarPlane;   }

// The one reverse-Z depth linearization, which the SceneDepth node emits so every material template needs it.
float LinearizeSceneDepth(float RawDepth)
{
    float ZNear = GetNearPlane();
    float ZFar  = GetFarPlane();
    return ZNear * ZFar / max(ZNear + RawDepth * (ZFar - ZNear), 1e-6);
}
uint2    GetScreenSize()            { return Scene().ScreenSize.xy; }
// What the view is shown at, which is larger than GetScreenSize while an upscaler restores the difference.
uint2    GetDisplaySize()           { return Scene().ScreenSize.zw; }

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
// A jittered pixel center minus this is the surface's unjittered UV, which is what the unjittered previous matrix pairs with.
float2   GetTemporalJitterUV()      { return float2(Scene().TemporalJitterU, Scene().TemporalJitterV); }

// [3][3] is 0 for perspective and 1 for orthographic, and being diagonal it reads the same in either convention.
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

// Mirrors the tile math in ClusterBuild/LightCull, and the clamps keep fragments nearer than ZNear inside Clusters().
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

// One bit per light, rounded up to whole words and clamped to what the pool holds for this grid, as LightCull wrote them.
uint GetClusterMaskWords()
{
    const uint3 Grid        = Scene().GridSize.xyz;
    const uint  NumClusters = max(Grid.x * Grid.y * Grid.z, 1u);
    return min((GetNumLights() + 31u) / 32u, ClusterLightMasks().Count / NumClusters);
}

// The cluster's non-empty mask words as [first, end), so a walk never loads a word with no light in it.
uint2 GetClusterWordRange(uint ClusterIndex)
{
    if (!ClusterWordRanges().Contains(ClusterIndex))
    {
        return uint2(0u, 0u);
    }
    const uint Packed = ClusterWordRanges().At(ClusterIndex)[0];
    return uint2(Packed & 0xFFFFu, min(Packed >> 16u, GetClusterMaskWords()));
}

// Bit b of word w is light 32 * w + b.
uint GetClusterMaskWord(uint ClusterIndex, uint Word, uint MaskWords)
{
    const uint Index = ClusterIndex * MaskWords + Word;
    return ClusterLightMasks().Contains(Index) ? ClusterLightMasks().At(Index)[0] : 0u;
}

uint GetClusterLightCount(uint ClusterIndex)
{
    const uint  MaskWords = GetClusterMaskWords();
    const uint2 Range     = GetClusterWordRange(ClusterIndex);
    uint Count = 0u;
    for (uint Word = Range.x; Word < Range.y; ++Word)
    {
        Count += countbits(GetClusterMaskWord(ClusterIndex, Word, MaskWords));
    }
    return Count;
}

float3 LightComplexityColor(uint Count)
{
    if (Count == 0u)  return float3(0.0, 0.0, 0.0);
    if (Count <= 2u)  return float3(0.0, 0.25, 0.6);
    if (Count <= 4u)  return float3(0.0, 0.6, 0.3);
    if (Count <= 8u)  return float3(0.7, 0.8, 0.0);
    if (Count <= 16u) return float3(1.0, 0.5, 0.0);
    return float3(1.0, 0.0, 0.0);
}

// Green is one triangle per quad, red is one live lane in four.
float3 QuadEfficiencyColor(float Efficiency)
{
    if (Efficiency >= 0.99) return float3(0.0, 0.55, 0.15);
    if (Efficiency >= 0.74) return float3(0.55, 0.75, 0.0);
    if (Efficiency >= 0.49) return float3(1.0, 0.65, 0.0);
    if (Efficiency >= 0.32) return float3(1.0, 0.30, 0.0);
    return float3(1.0, 0.0, 0.0);
}

float3 ClusterDebugColor(uint ClusterIndex, uint LightCount)
{
    uint H = ClusterIndex * 2654435761u;
    H ^= H >> 16; H *= 2246822519u;
    H ^= H >> 13; H *= 3266489917u;
    H ^= H >> 16;

    float3 Hue = float3(
        float( H        & 0xFFu) / 255.0,
        float((H >>  8) & 0xFFu) / 255.0,
        float((H >> 16) & 0xFFu) / 255.0);

    return Hue * ((LightCount == 0u) ? 0.25 : 1.0);
}

#include "MaterialSampling.slang"
