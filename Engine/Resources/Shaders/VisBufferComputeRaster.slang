#include "Includes/MeshletGeometry.slang"

// The camera's small opaque meshlets, a workgroup per meshlet whose lanes project its vertices and then each scan their triangles' pixels.

struct FVisBufferComputeRasterArgs
{
    // Draw-list slots, so a pixel's VisID resolves through the same draw entry a hardware meshlet's does.
    GPUSpan<uint>     List;
    // How many slots the cull took, then where this phase's slots start.
    GPUSpan<uint>     Cursors;
    // The nearest depth over the VisID per pixel, packed so one 64-bit max keeps both.
    GPUSpan<uint64_t> Target;
    uint2             TargetSize;
    uint              _Pad0;
    uint              _Pad1;
};
FVisBufferComputeRasterArgs* Pass() { return GetPassArgs<FVisBufferComputeRasterArgs>(); }

// A meshlet's work spread over one wave's worth of lanes, so its barriers cost next to nothing.
static const uint kRasterLanes = 32u;

// The cull only sends meshlets far smaller than this, so a wider box means bad bounds, and clipping it beats a lane that stalls the GPU.
static const int kMaxScanPixels = 256;

// Pixel position and depth of each meshlet vertex.
groupshared float3 gsPixels[MESHLET_MAX_VERTICES];

// Twice the signed area of A, B and P, positive when P lies counter-clockwise of A to B.
float EdgeFunction(float2 A, float2 B, float2 P)
{
    return (B.x - A.x) * (P.y - A.y) - (B.y - A.y) * (P.x - A.x);
}

// Every pixel whose center the triangle covers, edges inclusive, since the nearest depth wins wherever two triangles share one.
void RasterTriangle(uint VisID, bool bTwoSided, float3 A, float3 B, float3 C)
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

    const int2  Size = int2(Pass().TargetSize);
    const float2 Min = min(A.xy, min(B.xy, C.xy));
    const float2 Max = max(A.xy, max(B.xy, C.xy));
    const int2  Lo   = max(int2(ceil(Min - 0.5)), int2(0, 0));
    const int2  Hi   = min(min(int2(floor(Max - 0.5)), Size - 1), Lo + (kMaxScanPixels - 1));
    const float InvArea = 1.0 / Area;

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
                // Depth over w is affine in screen space, so plain screen barycentrics interpolate it exactly as the rasterizer does.
                const float Depth = (W0 * A.z + W1 * B.z + W2 * C.z) * InvArea;
                if (Depth >= 0.0 && Depth <= 1.0)
                {
                    const uint64_t Packed = (uint64_t(asuint(Depth)) << 32) | uint64_t(VisID);
                    InterlockedMax(Pass().Target.At(uint(y) * Pass().TargetSize.x + uint(x))[0], Packed);
                }
            }
        }
    }
}

[shader("compute")]
[numthreads(kRasterLanes, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupIndex)
{
    const uint Begin = Pass().Cursors.Get(1u);
    const uint End   = min(Pass().Cursors.Get(0u), Pass().List.Count);
    const float2 Size = float2(Pass().TargetSize);

    // Every skip below is uniform across the group, so the barriers stay in step.
    for (uint Item = Begin + GroupID.x; Item < End; Item += VISBUFFER_COMPUTE_RASTER_GROUPS)
    {
        const uint Slot = Pass().List.Get(Item);
        if (Slot > VISBUFFER_MAX_MESHLET_SLOTS)
        {
            continue;
        }

        const FMeshletRef Ref = ResolveMeshlet(Slot);
        const uint VertCount = Ref.M.VertexCount;
        if (VertCount == 0u)
        {
            continue;
        }

        const float4x4 Model = Ref.ModelMatrix;
        const float4x4 View  = GetCameraView();
        const float4x4 Proj  = GetCameraProjection();
        for (uint Vertex = Lane; Vertex < VertCount; Vertex += kRasterLanes)
        {
            const float3 WorldPos = mul(Model, float4(LoadStaticPosition(Ref.Header, Ref.M, Vertex), 1.0)).xyz;
            const float4 Clip     = CameraClipPosition(CameraViewPosition(WorldPos, View), Proj);
            const float3 Ndc      = Clip.xyz / Clip.w;
            gsPixels[Vertex] = float3((Ndc.xy * 0.5 + 0.5) * Size, Ndc.z);
        }
        GroupMemoryBarrierWithGroupSync();

        const bool bTwoSided = Ref.InstHasFlag(EInstanceFlags::TwoSided);
        for (uint Triangle = Lane; Triangle < Ref.M.TriangleCount; Triangle += kRasterLanes)
        {
            const uint3 Idx = UnpackMeshletTriangleSafe(Ref, Triangle);
            if (!IsDegenerateTriangleIndices(Idx))
            {
                RasterTriangle(PackVisID(Slot, Triangle), bTwoSided, gsPixels[Idx.x], gsPixels[Idx.y], gsPixels[Idx.z]);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
