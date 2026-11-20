#include "Includes/MeshletGeometry.slang"
#include "Includes/VirtualShadow.slang"

// Small opaque sun shadow casters, a workgroup per meshlet whose lanes transform its vertices and then each scan their triangles' texels.

struct FComputeRasterArgs
{
    // Both layers' entries, the dynamic layer's from VSM_COMPUTE_RASTER_CAPACITY on, with a cursor each.
    GPUSpan<uint2> List;
    GPUSpan<uint>  Cursors;
    // Static views draw before the static layer is copied into the dynamic one, and dynamic views after it.
    uint           bStatic;
    uint           _Pad0;
    uint           _Pad1;
    uint           _Pad2;
};
FComputeRasterArgs* Pass() { return GetPassArgs<FComputeRasterArgs>(); }

// Virtual texel position and depth of each meshlet vertex in the entry's level.
groupshared float3 gsTexels[MESHLET_MAX_VERTICES];

// The cull only sends meshlets far smaller than this, so a wider box means bad bounds, and clipping it beats a lane that stalls the GPU.
static const int kMaxScanTexels = 256;

// Twice the signed area of A, B and P, positive when P lies counter-clockwise of A to B.
float EdgeFunction(float2 A, float2 B, float2 P)
{
    return (B.x - A.x) * (P.y - A.y) - (B.y - A.y) * (P.x - A.x);
}

// A meshlet's work spread over one wave's worth of lanes, so its barriers cost next to nothing.
static const uint kRasterLanes = 32u;

// The page last written, which a small triangle almost never leaves.
struct FPageCache
{
    int2 Page;
    uint Entry;
};

// Writes every texel whose center the triangle covers, edges inclusive, since the nearest depth wins wherever two triangles share one.
void RasterTriangle(uint Level, int2 PageOrigin, bool bStatic, bool bTwoSided, float3 A, float3 B, float3 C, inout FPageCache Cache)
{
    float Area = EdgeFunction(A.xy, B.xy, C.xy);

    // Front faces are counter-clockwise, as in the hardware path, and a zero or NaN area draws nothing.
    if (!(abs(Area) > 0.0) || (!bTwoSided && !(Area > 0.0)))
    {
        return;
    }
    if (Area < 0.0)
    {
        const float3 Swap = B;
        B    = C;
        C    = Swap;
        Area = -Area;
    }

    const float2 Min = min(A.xy, min(B.xy, C.xy));
    const float2 Max = max(A.xy, max(B.xy, C.xy));
    const int2   Lo  = max(int2(ceil(Min - 0.5)), int2(0, 0));
    const int2   Hi  = min(min(int2(floor(Max - 0.5)), int2(VSM_VIRTUAL_SIZE - 1u, VSM_VIRTUAL_SIZE - 1u)), Lo + (kMaxScanTexels - 1));
    const float  InvArea = 1.0 / Area;

    for (int y = Lo.y; y <= Hi.y; ++y)
    {
        for (int x = Lo.x; x <= Hi.x; ++x)
        {
            const float2 P  = float2(x, y) + 0.5;
            const float  W0 = EdgeFunction(B.xy, C.xy, P);
            const float  W1 = EdgeFunction(C.xy, A.xy, P);
            const float  W2 = EdgeFunction(A.xy, B.xy, P);
            if (W0 >= 0.0 && W1 >= 0.0 && W2 >= 0.0)
            {
                const uint2 Texel = uint2(x, y);
                const int2  Page  = PageOrigin + int2(Texel >> VSM_PAGE_SHIFT);
                if (any(Page != Cache.Page))
                {
                    Cache.Page  = Page;
                    Cache.Entry = VirtualShadowPageEntry(Level, Page);
                }
                WriteVirtualShadowPageTexel(Cache.Entry, bStatic, Texel, (W0 * A.z + W1 * B.z + W2 * C.z) * InvArea);
            }
        }
    }
}

[shader("compute")]
[numthreads(kRasterLanes, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupIndex)
{
    const bool bStatic   = Pass().bStatic != 0u;
    const uint Layer     = bStatic ? 0u : 1u;
    const uint Base      = Layer * VSM_COMPUTE_RASTER_CAPACITY;
    const uint Count     = Pass().Cursors.Contains(Layer) ? min(Pass().Cursors.Get(Layer), VSM_COMPUTE_RASTER_CAPACITY) : 0u;
    const uint LevelMask = (1u << VSM_COMPUTE_RASTER_LEVEL_BITS) - 1u;

    // Every skip below is uniform across the group, so the barriers stay in step.
    for (uint EntryIndex = GroupID.x; EntryIndex < Count; EntryIndex += VSM_COMPUTE_RASTER_GROUPS)
    {
        const uint2 Entry = Pass().List.Get(Base + EntryIndex);
        FMeshletRef Ref = ResolveInstanceMeshlet(Entry.x, Entry.y >> VSM_COMPUTE_RASTER_LEVEL_BITS);
        const uint VertCount = Ref.M.VertexCount;
        if (VertCount == 0u)
        {
            continue;
        }

        const uint Level = Entry.y & LevelMask;
        const FVirtualShadowLevel Lv = VirtualShadowLevel(Level);
        const float4x4 Model = Ref.ModelMatrix;
        for (uint Vertex = Lane; Vertex < VertCount; Vertex += kRasterLanes)
        {
            const float3 WorldPos = mul(Model, float4(LoadStaticPosition(Ref.Header, Ref.M, Vertex), 1.0)).xyz;
            // Orthographic, so clip space is already the normalized device space.
            const float3 Ndc = mul(Lv.ViewProjection, float4(WorldPos, 1.0)).xyz;
            gsTexels[Vertex] = float3((Ndc.xy * 0.5 + 0.5) * float(VSM_VIRTUAL_SIZE), Ndc.z);
        }
        GroupMemoryBarrierWithGroupSync();

        const bool bTwoSided = Ref.InstHasFlag(EInstanceFlags::TwoSided);
        FPageCache Cache;
        Cache.Page  = int2(0x7FFFFFFF, 0x7FFFFFFF);
        Cache.Entry = 0u;
        for (uint Triangle = Lane; Triangle < Ref.M.TriangleCount; Triangle += kRasterLanes)
        {
            const uint3 Idx = UnpackMeshletTriangleSafe(Ref, Triangle);
            if (!IsDegenerateTriangleIndices(Idx))
            {
                RasterTriangle(Level, Lv.PageOrigin, bStatic, bTwoSided, gsTexels[Idx.x], gsTexels[Idx.y], gsTexels[Idx.z], Cache);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
