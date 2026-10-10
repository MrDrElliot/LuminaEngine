module RHI;

// Unused heap views are stripped from each shader's SPIR-V, so one module can alias the storage heap as every image type.

[[vk::binding(0, 0)]] internal SamplerState                            SamplerHeap[];
// Only Samplers::Shadow is created with compareEnable, so this view is meaningful at that slot alone.
[[vk::binding(0, 0)]] internal SamplerComparisonState                  ComparisonSamplerHeap[];

[[vk::binding(1, 0)]] internal Texture2D                               Texture2DHeap[];
[[vk::binding(1, 0)]] internal Texture2D<uint>                         Texture2DUIntHeap[];
[[vk::binding(1, 0)]] internal Texture2DArray                          Texture2DArrayHeap[];
[[vk::binding(1, 0)]] internal Texture3D                               Texture3DHeap[];
[[vk::binding(1, 0)]] internal TextureCube                             TextureCubeHeap[];
[[vk::binding(1, 0)]] internal TextureCubeArray                        TextureCubeArrayHeap[];

[[vk::binding(2, 0)]] internal RWTexture2D<float4>                     RWTexture2DHeap[];
[[vk::binding(2, 0)]] internal globallycoherent RWTexture2D<float4>    RWTexture2DCoherentHeap[];
[[vk::binding(2, 0)]] internal RWTexture3D<float4>                     RWTexture3DHeap[];
// Cube storage views are illegal in Vulkan, so cube targets are registered as 2D arrays and written by face.
[[vk::binding(2, 0)]] internal RWTexture2DArray<float4>                RWTexture2DArrayHeap[];

internal struct FRootConstants
{
    uint64_t Args;
    uint64_t SceneRoot;
    uint64_t SceneData;
    uint64_t Instances;
    uint64_t Lights;
};
[[vk::push_constant]] internal FRootConstants Root;

// A pointer, so a pass's field reads stay narrow loads through an access chain.
public Ptr<T> GetArgs<T>()
{
    return (T*)Root.Args;
}

// One return each, since a pointer picked through a branch lands in a Function variable, which is invalid SPIR-V.
public Ptr<T> GetSceneRootPointer<T>()  { return (T*)Root.SceneRoot; }
public Ptr<T> GetSceneDataPointer<T>()  { return (T*)Root.SceneData; }
public Ptr<T> GetInstancesPointer<T>()  { return (T*)Root.Instances; }
public Ptr<T> GetLightsPointer<T>()     { return (T*)Root.Lights; }

// GPU-AV instruments a typed pointer load with a null of pointer type, which SPIR-V forbids, so nested addresses stay uint64_t.
public T LoadBlock<T>(uint64_t Address)
{
    return ((T*)Address)[0];
}

// Mirror of RHI::TGPUSpan<T>, holding a raw address so it is an ordinary value Slang may copy into a local.
public struct GPUSpan<T>
{
    internal uint64_t Address;
    public   uint     Count;
    internal uint     _Pad;

    public bool IsEmpty()               { return Count == 0u; }
    public bool Contains(uint Index)    { return Index < Count; }

    // Unchecked, for atomics, in-place field writes and narrow field reads, so bound the index first.
    public Ptr<T> At(uint Index)        { return (T*)Address + Index; }

    // Reads past the end resolve to zero rather than to whatever the allocator has since put there.
    public T Get(uint Index)
    {
        if (Index >= Count)
        {
            T Zero = {};
            return Zero;
        }
        return ((T*)Address)[Index];
    }

    public T GetAligned(uint Index)
    {
        if (Index >= Count)
        {
            T Zero = {};
            return Zero;
        }
        return loadAligned<16>((T*)Address + Index);
    }

    // A dropped store is a missing draw, where an unchecked one is a device loss.
    public void Set(uint Index, T Value)
    {
        if (Index < Count)
        {
            ((T*)Address)[Index] = Value;
        }
    }
}

public static const uint kInvalidResourceIndex = 0xFFFFFFFFu;

public struct FSampler
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
}

public struct FComparisonSampler
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
}

// Registration order, kept in lockstep with RHI::Core::Initialize.
public static const uint SAMPLER_LINEAR_WRAP   = 0;
public static const uint SAMPLER_LINEAR_CLAMP  = 1;
public static const uint SAMPLER_LINEAR_MIRROR = 2;
public static const uint SAMPLER_POINT_WRAP    = 3;
public static const uint SAMPLER_POINT_CLAMP   = 4;
public static const uint SAMPLER_ANISO_WRAP    = 5;
public static const uint SAMPLER_ANISO_CLAMP   = 6;
public static const uint SAMPLER_SHADOW        = 7;
public static const uint SAMPLER_MIN_REDUCTION = 8;
public static const uint SAMPLER_MAX_REDUCTION = 9;
public static const uint SAMPLER_POINT_MIRROR  = 10;
public static const uint SAMPLER_ANISO_MIRROR  = 11;

public namespace Samplers
{
    public static const FSampler LinearWrap   = FSampler(SAMPLER_LINEAR_WRAP);
    public static const FSampler LinearClamp  = FSampler(SAMPLER_LINEAR_CLAMP);
    public static const FSampler LinearMirror = FSampler(SAMPLER_LINEAR_MIRROR);
    public static const FSampler PointWrap    = FSampler(SAMPLER_POINT_WRAP);
    public static const FSampler PointClamp   = FSampler(SAMPLER_POINT_CLAMP);
    public static const FSampler PointMirror  = FSampler(SAMPLER_POINT_MIRROR);
    public static const FSampler AnisoWrap    = FSampler(SAMPLER_ANISO_WRAP);
    public static const FSampler AnisoClamp   = FSampler(SAMPLER_ANISO_CLAMP);
    public static const FSampler AnisoMirror  = FSampler(SAMPLER_ANISO_MIRROR);
    // Bilinear taps that return the 2x2 minimum or maximum instead of the blend.
    public static const FSampler MinReduction = FSampler(SAMPLER_MIN_REDUCTION);
    public static const FSampler MaxReduction = FSampler(SAMPLER_MAX_REDUCTION);
    public static const FComparisonSampler Shadow = FComparisonSampler(SAMPLER_SHADOW);
}

internal SamplerState Resolve(FSampler S)                         { return SamplerHeap[NonUniformResourceIndex(S.Index)]; }
internal SamplerComparisonState Resolve(FComparisonSampler S)     { return ComparisonSamplerHeap[NonUniformResourceIndex(S.Index)]; }

public struct FTexture2D
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    internal Texture2D Get() { return Texture2DHeap[NonUniformResourceIndex(Index)]; }

    public float4 Sample(FSampler S, float2 UV)                                     { return Get().Sample(Resolve(S), UV); }
    public float4 SampleBias(FSampler S, float2 UV, float Bias)                     { return Get().SampleBias(Resolve(S), UV, Bias); }
    public float4 SampleGrad(FSampler S, float2 UV, float2 Ddx, float2 Ddy)         { return Get().SampleGrad(Resolve(S), UV, Ddx, Ddy); }
    public float4 SampleLevel(FSampler S, float2 UV, float Lod)                     { return Get().SampleLevel(Resolve(S), UV, Lod); }
    public float4 SampleLevel(FSampler S, float2 UV, float Lod, constexpr int2 Offset)       { return Get().SampleLevel(Resolve(S), UV, Lod, Offset); }
    public float  SampleCmp(FComparisonSampler S, float2 UV, float Compare)         { return Get().SampleCmp(Resolve(S), UV, Compare); }
    public float  SampleCmpLevelZero(FComparisonSampler S, float2 UV, float Compare) { return Get().SampleCmpLevelZero(Resolve(S), UV, Compare); }
    public float4 GatherRed(FSampler S, float2 UV)                                  { return Get().GatherRed(Resolve(S), UV); }
    public float4 GatherRed(FSampler S, float2 UV, constexpr int2 Offset)                    { return Get().GatherRed(Resolve(S), UV, Offset); }
    public float4 Load(int2 Texel, int Mip = 0)                                     { return Get().Load(int3(Texel, Mip)); }
    public float4 Load(int3 TexelAndMip)                                            { return Get().Load(TexelAndMip); }
    public float4 Load(int3 TexelAndMip, constexpr int2 Offset)                              { return Get().Load(TexelAndMip, Offset); }
    public void   GetDimensions(out uint Width, out uint Height)                    { Get().GetDimensions(Width, Height); }
    public void   GetDimensions(out float Width, out float Height)                  { Get().GetDimensions(Width, Height); }
    public void   GetDimensions(uint Mip, out uint Width, out uint Height, out uint Levels) { Get().GetDimensions(Mip, Width, Height, Levels); }
}

// An integer view of the same heap, for R32_UINT targets such as the visibility buffer.
public struct FTexture2DUInt
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    public uint Load(int2 Texel, int Mip = 0) { return Texture2DUIntHeap[NonUniformResourceIndex(Index)].Load(int3(Texel, Mip)); }
}

// The layer is a discrete index, never filtered between slices.
public struct FTexture2DArray
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    internal Texture2DArray Get() { return Texture2DArrayHeap[NonUniformResourceIndex(Index)]; }

    public float4 Sample(FSampler S, float3 UVSlice)                                { return Get().Sample(Resolve(S), UVSlice); }
    public float4 SampleBias(FSampler S, float3 UVSlice, float Bias)                { return Get().SampleBias(Resolve(S), UVSlice, Bias); }
    public float4 SampleGrad(FSampler S, float3 UVSlice, float2 Ddx, float2 Ddy)    { return Get().SampleGrad(Resolve(S), UVSlice, Ddx, Ddy); }
    public float4 SampleLevel(FSampler S, float3 UVSlice, float Lod)                { return Get().SampleLevel(Resolve(S), UVSlice, Lod); }
    public float4 Load(int4 TexelSliceMip)                                          { return Get().Load(TexelSliceMip); }
    public void   GetDimensions(out uint Width, out uint Height, out uint Slices)   { Get().GetDimensions(Width, Height, Slices); }
}

public struct FTexture3D
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    internal Texture3D Get() { return Texture3DHeap[NonUniformResourceIndex(Index)]; }

    public float4 Sample(FSampler S, float3 UVW)                                    { return Get().Sample(Resolve(S), UVW); }
    public float4 SampleLevel(FSampler S, float3 UVW, float Lod)                    { return Get().SampleLevel(Resolve(S), UVW, Lod); }
    public float4 Load(int3 Texel, int Mip = 0)                                     { return Get().Load(int4(Texel, Mip)); }
    public float4 Load(int4 TexelAndMip)                                            { return Get().Load(TexelAndMip); }
    public void   GetDimensions(out uint Width, out uint Height, out uint Depth)    { Get().GetDimensions(Width, Height, Depth); }
}

public struct FTextureCube
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    internal TextureCube Get() { return TextureCubeHeap[NonUniformResourceIndex(Index)]; }

    public float4 Sample(FSampler S, float3 Direction)                              { return Get().Sample(Resolve(S), Direction); }
    public float4 SampleLevel(FSampler S, float3 Direction, float Lod)              { return Get().SampleLevel(Resolve(S), Direction, Lod); }
    public void   GetDimensions(out uint Width, out uint Height)                    { Get().GetDimensions(Width, Height); }
}

public struct FTextureCubeArray
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    public float4 SampleLevel(FSampler S, float3 Direction, uint Slice, float Lod)
    {
        return TextureCubeArrayHeap[NonUniformResourceIndex(Index)].SampleLevel(Resolve(S), float4(Direction, (float)Slice), Lod);
    }
}

public struct FRWTexture2D
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    public __subscript(uint2 Texel) -> float4
    {
        get { return RWTexture2DHeap[NonUniformResourceIndex(Index)][Texel]; }
        [nonmutating] set { RWTexture2DHeap[NonUniformResourceIndex(Index)][Texel] = newValue; }
    }

    public void GetDimensions(out uint Width, out uint Height) { RWTexture2DHeap[NonUniformResourceIndex(Index)].GetDimensions(Width, Height); }
}

// Coherent across workgroups, for single-pass reductions that hand results from one group to the next.
public struct FRWTexture2DCoherent
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    public __subscript(uint2 Texel) -> float4
    {
        get { return RWTexture2DCoherentHeap[NonUniformResourceIndex(Index)][Texel]; }
        [nonmutating] set { RWTexture2DCoherentHeap[NonUniformResourceIndex(Index)][Texel] = newValue; }
    }
}

public struct FRWTexture3D
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    public __subscript(uint3 Texel) -> float4
    {
        get { return RWTexture3DHeap[NonUniformResourceIndex(Index)][Texel]; }
        [nonmutating] set { RWTexture3DHeap[NonUniformResourceIndex(Index)][Texel] = newValue; }
    }

    public void GetDimensions(out uint Width, out uint Height, out uint Depth) { RWTexture3DHeap[NonUniformResourceIndex(Index)].GetDimensions(Width, Height, Depth); }
}

// A cube target written one face at a time, the face being the array slice.
public struct FRWTexture2DArray
{
    public uint Index;
    public __init(uint InIndex) { Index = InIndex; }
    public bool IsValid() { return Index != kInvalidResourceIndex; }

    public __subscript(uint3 TexelAndSlice) -> float4
    {
        get { return RWTexture2DArrayHeap[NonUniformResourceIndex(Index)][TexelAndSlice]; }
        [nonmutating] set { RWTexture2DArrayHeap[NonUniformResourceIndex(Index)][TexelAndSlice] = newValue; }
    }

    public void GetDimensions(out uint Width, out uint Height, out uint Slices) { RWTexture2DArrayHeap[NonUniformResourceIndex(Index)].GetDimensions(Width, Height, Slices); }
}
