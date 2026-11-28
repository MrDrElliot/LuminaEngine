#include "Includes/VisBufferSurface.slang"

// What r.Debug.Stats shows for the camera, counted off what the culls wrote this frame. Dispatched only while stats are collected.

#define RENDER_STATS_GROUP_SIZE 64u

struct FRenderStatsArgs
{
    uint NumViews;
    uint NumDraws;
    uint NumWorkerGroups;
    uint ShadowComputeLayerCapacity;
    // Camera buckets first, since cull view 0 is the camera.
    GPUSpan<FRenderBucket>      Buckets;
    GPUSpan<FInstanceCullEntry> RetainedCullEntries;
    GPUSpan<uint>               InstanceCount;
    GPUSpan<uint2>              InstanceViewRanges;
    // The drawn proxy args, whose second word counts ProxyList().
    GPUSpan<uint>               ProxyDrawArgs;
    // The shadow compute raster's static and dynamic append cursors, empty while it is off.
    GPUSpan<uint>               ShadowComputeCursors;
    // Candidates, camera instances, listed retained proxies, meshlets, triangles, shadow meshlets, impostors, far proxies.
    GPUSpan<uint>               OutStats;
};
FRenderStatsArgs* Pass() { return GetPassArgs<FRenderStatsArgs>(); }

// Called by every lane, since the sum is a wave operation.
void AddStat(uint Word, uint Value)
{
    const uint Sum = WaveActiveSum(Value);
    if (WaveIsFirstLane() && Sum != 0u)
    {
        InterlockedAdd(Pass().OutStats.At(Word)[0], Sum);
    }
}

[shader("compute")]
[numthreads(RENDER_STATS_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    const uint Group = GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS;
    const uint Lane  = LocalID.x;

    uint Candidates = 0u, CameraInstances = 0u, ListedRetained = 0u, Meshlets = 0u, Triangles = 0u;
    uint ShadowMeshlets = 0u, Impostors = 0u, FarProxies = 0u;

    if (Group < Pass().NumDraws)
    {
        // The front holds the hardware draws and the back what the camera's compute raster took, as MeshletCull slots them.
        const FRenderBucket Bucket = Pass().Buckets.Get(Group);
        const uint Front = min(Bucket.DrawCursor, Bucket.DrawCapacity);
        const uint Back  = min(Bucket.DrawBackCursor, Bucket.DrawCapacity - Front);
        Meshlets = Lane == 0u ? Front + Back : 0u;
        for (uint i = Lane; i < Front + Back; i += RENDER_STATS_GROUP_SIZE)
        {
            const uint Slot = Bucket.DrawBase + (i < Front ? i : Bucket.DrawCapacity - 1u - (i - Front));
            Triangles += ResolveMeshlet(Slot).M.TriangleCount;
        }
    }
    else if (Group < Pass().NumDraws + Pass().NumWorkerGroups)
    {
        const uint Stride = Pass().NumWorkerGroups * RENDER_STATS_GROUP_SIZE;
        const uint Worker = (Group - Pass().NumDraws) * RENDER_STATS_GROUP_SIZE + Lane;

        for (uint s = Worker; s < Pass().RetainedCullEntries.Count; s += Stride)
        {
            const FInstanceCullEntry Cull = Pass().RetainedCullEntries.Get(s);
            Candidates += (Cull.HasFlag(EInstanceFlags::Active) && Cull.HasFlag(EInstanceFlags::HasGeometry) && !Cull.HasFlag(EInstanceFlags::ShadowOnly)) ? 1u : 0u;
        }

        const uint NumViews = max(Pass().NumViews, 1u);
        const uint Visible  = min(Pass().InstanceCount.Get(0u), Pass().InstanceViewRanges.Count / NumViews);
        for (uint i = Worker; i < Visible; i += Stride)
        {
            CameraInstances += Pass().InstanceViewRanges.Get(i * NumViews).y > 0u ? 1u : 0u;
        }

        const uint Listed = Pass().ProxyDrawArgs.Count < 2u ? 0u : min(Pass().ProxyDrawArgs.Get(1u), ProxyList().Count);
        for (uint i = Worker; i < Listed; i += Stride)
        {
            const uint      Entry = ProxyList().Get(i);
            const FVisProxy P     = LoadProxyEntry(Entry);
            const bool      bImpostor = P.bValid && MeshletHeaderAt(P.Static.MeshletHeaderSlot).bHasImpostor;
            Impostors      += bImpostor ? 1u : 0u;
            FarProxies     += P.bValid && !bImpostor ? 1u : 0u;
            ListedRetained += (Entry & PROXY_ENTRY_POINT_BIT) == 0u ? 1u : 0u;
        }

        for (uint b = Pass().NumDraws + Worker; b < Pass().Buckets.Count; b += Stride)
        {
            if (CullViewHasFlag(b / Pass().NumDraws, CULL_VIEW_FLAG_CAST_SHADOW_ONLY))
            {
                FRenderBucket* Bucket = Pass().Buckets.At(b);
                ShadowMeshlets += min(Bucket.DrawCursor + Bucket.DrawBackCursor, Bucket.DrawCapacity);
            }
        }

        if (Worker < Pass().ShadowComputeCursors.Count)
        {
            ShadowMeshlets += min(Pass().ShadowComputeCursors.Get(Worker), Pass().ShadowComputeLayerCapacity);
        }
    }

    AddStat(0u, Candidates);
    AddStat(1u, CameraInstances);
    AddStat(2u, ListedRetained);
    AddStat(3u, Meshlets);
    AddStat(4u, Triangles);
    AddStat(5u, ShadowMeshlets);
    AddStat(6u, Impostors);
    AddStat(7u, FarProxies);
}
