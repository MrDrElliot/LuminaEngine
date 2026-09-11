// GPU grass scatter. One thread per candidate point over a camera-centred grid, appending survivors to a
// per-species instance buffer. Nothing per-blade ever reaches the CPU, which is what makes millions of
// them affordable: the CPU only sets the region and the species constants.
//
// Rejection order is cheapest-first (grid ring, then weight, then slope), so the common empty cell costs
// one texture fetch. Dispatch: one thread per cell of a GridSide x GridSide grid.

#include "Includes/SceneGlobals.slang"
#include "Includes/TerrainCommon.slang"

#define LOCAL_SIZE_X 8
#define LOCAL_SIZE_Y 8

// Writes straight into the retained instance arrays the ordinary cull consumes, in a block the CPU
// reserved and never writes again. That is what lets grass ride the normal VisBuffer/deferred path
// rather than needing a geometry pass of its own.
struct FGrassScatterPushConstants
{
    // Sliced to this species' reserved block, so an index here is block-local and the span's own count
    // IS the capacity. Nothing in this shader can name a slot outside the block any more.
    GPUSpan<FInstanceCullEntry> OutCullEntries;
    GPUSpan<FTransform3x4>      OutTransforms;
    GPUSpan<FInstanceStatic>    OutStatic;
    GPUSpan<uint>               OutCursor;      // blades appended so far in this species' block

    uint HeightmapIndex;        // bindless 2D
    uint NormalIndex;           // bindless 2D
    uint LayerWeightsIndex;     // bindless 2D array
    uint LayerIndex;            // slice gating this species

    float2 CameraXZ;
    float  Radius;              // cull distance; the grid is a square inscribing this circle
    float  CellSize;            // world units between candidates, from the species density

    float  MinWeight;
    float  MaxSlopeCos;         // cos(MaxSlopeDegrees); reject when normal.y falls below it
    float  ScaleMin;
    float  ScaleMax;

    float  AlignToNormal;
    float  ZOffset;
    uint   bRandomYaw;
    uint   Seed;

    uint   GridSide;            // candidates per axis
    uint   _PadMaxInstances;

    // Constant for the whole species: one mesh, one material, so the CPU interns these once.
    uint   _PadInstanceSlotBase;
    uint   DrawIDAndFlags;
    uint   SurfaceDescIndex;
    uint   MeshletHeaderSlot;
    uint   MaterialIndex;
    uint   EntityID;
    float  MeshBoundsRadius;
    float  MaxDrawDistance;

    // Taken straight from the terrain rather than through its params buffer: these four are all the
    // scatter needs, and the CPU already has them when it sets up the dispatch.
    float2 TerrainOriginXZ;
    float  TileWorldSize;
    float  MaxHeight;
    float  TerrainBaseY;
    uint   _Pad0;
    uint   _Pad1;
    uint   _Pad2;
};

FGrassScatterPushConstants* Pass() { return GetPassArgs<FGrassScatterPushConstants>(); }

// PCG-style integer hash. Deterministic in the cell index, so a blade keeps its place as the camera
// moves and the same cell is regenerated -- without that, grass crawls whenever the region shifts.
uint HashUint(uint X)
{
    X ^= X >> 17; X *= 0xED5AD4BBu;
    X ^= X >> 11; X *= 0xAC4C1B51u;
    X ^= X >> 15; X *= 0x31848BABu;
    X ^= X >> 14;
    return X;
}

float Hash01(uint X) { return float(HashUint(X) & 0x00FFFFFFu) / 16777216.0; }

float3 RotateByQuat(float4 Q, float3 V)
{
    return V + 2.0 * cross(Q.xyz, cross(Q.xyz, V) + Q.w * V);
}

[shader("compute")]
[numthreads(LOCAL_SIZE_X, LOCAL_SIZE_Y, 1)]
void main(uint3 Tid : SV_DispatchThreadID)
{
    FGrassScatterPushConstants* P = Pass();

    if (Tid.x >= P->GridSide || Tid.y >= P->GridSide)
    {
        return;
    }

    // Cell centre in world space, snapped to a global lattice so the pattern is camera-independent.
    const float HalfExtent = P->Radius;
    const float2 GridOrigin = floor((P->CameraXZ - HalfExtent) / P->CellSize) * P->CellSize;
    const uint CellId = Tid.y * P->GridSide + Tid.x;

    // Two independent streams per cell, so jitter does not correlate with the accept test.
    const uint LatticeX = (uint)(GridOrigin.x / P->CellSize) + Tid.x;
    const uint LatticeY = (uint)(GridOrigin.y / P->CellSize) + Tid.y;
    const uint CellHash = HashUint(LatticeX * 73856093u ^ LatticeY * 19349663u ^ P->Seed);

    const float JitterX = Hash01(CellHash * 3u + 0u) - 0.5;
    const float JitterZ = Hash01(CellHash * 3u + 1u) - 0.5;

    float2 WorldXZ;
    WorldXZ.x = GridOrigin.x + (float(Tid.x) + 0.5 + JitterX) * P->CellSize;
    WorldXZ.y = GridOrigin.y + (float(Tid.y) + 0.5 + JitterZ) * P->CellSize;

    // Round region, so grass does not pop in at the corners of the dispatch square.
    if (distance(WorldXZ, P->CameraXZ) > P->Radius)
    {
        return;
    }

    // World -> heightmap UV. Outside the tile there is nothing to sample.
    const float2 Local = (WorldXZ - P->TerrainOriginXZ) / P->TileWorldSize + 0.5;
    if (any(Local < 0.0) || any(Local > 1.0))
    {
        return;
    }

    // The painted layer is the whole point: weight gates where this species may grow at all.
    const float Weight = SampleTexture2DArrayLevel(P->LayerWeightsIndex, SAMPLER_LINEAR_CLAMP, Local, P->LayerIndex, 0).r;
    if (Weight < P->MinWeight)
    {
        return;
    }

    // Stochastic thinning by weight, so coverage fades with the paint instead of ending on a hard edge.
    if (Hash01(CellHash * 3u + 2u) > Weight)
    {
        return;
    }

    const float3 Normal = normalize(SampleTexture2DLevel(P->NormalIndex, SAMPLER_LINEAR_CLAMP, Local, 0).xyz * 2.0 - 1.0);
    if (Normal.y < P->MaxSlopeCos)
    {
        return;
    }

    const float Height = SampleTexture2DLevel(P->HeightmapIndex, SAMPLER_LINEAR_CLAMP, Local, 0).r;

    const float3 Position = float3(WorldXZ.x,
                                   P->TerrainBaseY + Height * P->MaxHeight + P->ZOffset,
                                   WorldXZ.y);

    const float Scale = lerp(P->ScaleMin, P->ScaleMax, Hash01(CellHash ^ 0x9E3779B9u));

    // Yaw about world up, then tilted toward the surface normal by AlignToNormal.
    const float Yaw = P->bRandomYaw != 0u ? Hash01(CellHash ^ 0x85EBCA6Bu) * 6.2831853 : 0.0;
    const float4 YawQuat = float4(0.0, sin(Yaw * 0.5), 0.0, cos(Yaw * 0.5));

    const float3 Up = float3(0.0, 1.0, 0.0);
    const float3 Target = normalize(lerp(Up, Normal, saturate(P->AlignToNormal)));
    const float3 Axis = cross(Up, Target);
    const float  AxisLen = length(Axis);

    float4 TiltQuat = float4(0.0, 0.0, 0.0, 1.0);
    if (AxisLen > 1e-5)
    {
        const float Angle = asin(saturate(AxisLen));
        TiltQuat = float4(normalize(Axis) * sin(Angle * 0.5), cos(Angle * 0.5));
    }

    // Tilt applied in world space, so it does not rotate with the yaw.
    const float4 Q = float4(
        TiltQuat.w * YawQuat.xyz + YawQuat.w * TiltQuat.xyz + cross(TiltQuat.xyz, YawQuat.xyz),
        TiltQuat.w * YawQuat.w - dot(TiltQuat.xyz, YawQuat.xyz));

    uint Slot;
    InterlockedAdd(SpanAt(&P->OutCursor, 0u)[0], 1u, Slot);
    if (!SpanContains(&P->OutCullEntries, Slot))
    {
        // Over budget: hold the cursor at capacity so nothing writes past the reserved block.
        InterlockedMin(SpanAt(&P->OutCursor, 0u)[0], P->OutCullEntries.Count);
        return;
    }

    // Rotation matrix from the quaternion, scaled. Written as the 3x4 the cull and raster already read.
    const float3 C0 = RotateByQuat(Q, float3(Scale, 0.0, 0.0));
    const float3 C1 = RotateByQuat(Q, float3(0.0, Scale, 0.0));
    const float3 C2 = RotateByQuat(Q, float3(0.0, 0.0, Scale));

    FTransform3x4 Xf;
    Xf.Row0 = float4(C0.x, C1.x, C2.x, Position.x);
    Xf.Row1 = float4(C0.y, C1.y, C2.y, Position.y);
    Xf.Row2 = float4(C0.z, C1.z, C2.z, Position.z);
    SpanSet(&P->OutTransforms, Slot, Xf);

    FInstanceCullEntry Cull;
    Cull.SphereBounds     = float4(Position, P->MeshBoundsRadius * Scale);
    // Active is added here rather than by the CPU, so a slot the scatter skipped stays free.
    Cull.DrawIDAndFlags   = P->DrawIDAndFlags | ((uint)EInstanceFlags::Active << 16);
    Cull.SurfaceDescIndex = P->SurfaceDescIndex;
    Cull.MaxDrawDistance  = P->MaxDrawDistance;
    Cull.ForcedLODIndex   = -1;
    SpanSet(&P->OutCullEntries, Slot, Cull);

    FInstanceStatic Static;
    Static.MeshletHeaderSlot       = P->MeshletHeaderSlot;
    Static.CustomData              = (FCustomPrimitiveData)0;
    Static.MaterialIndex           = P->MaterialIndex;
    Static.EntityID                = P->EntityID;
    Static.BoneOffset              = 0u;
    Static.SkinnedVertexBase       = kNoPreSkinBase;
    Static.ShadowSkinnedVertexBase = kNoPreSkinBase;
    Static._Pad0                   = 0u;
    SpanSet(&P->OutStatic, Slot, Static);
}
