#include "Includes/AppendBuffer.slang"
#include "Includes/MeshletCullCore.slang"
#include "Includes/MeshletGeometry.slang"

// The meshlet cull, as the amplification stage of the draw that consumes it. One workgroup is one
// FMeshletBlock: up to MESHLET_TASK_GROUP_SIZE meshlets of ONE instance, so every per-instance load is
// workgroup-uniform and only the meshlet bounds are scattered.
//
// RULE: no lane may return early. This shader ends in a group barrier and a uniform DispatchMesh, and a
// lane that left before the barrier makes it non-uniform -- undefined behaviour NVIDIA tolerates and AMD
// hangs on, with no page fault and no failing lane to inspect. Every reject sets bKeep = false.

// The push block this stage shares with the mesh stage it dispatches into.
#define Pass MeshletPass

groupshared FVisTaskPayload gsPayload;
// Groupshared rather than a wave intrinsic: a 32-thread workgroup is a full wave on some devices and a
// partial one on others, and the compaction has to be exact either way.
groupshared uint gsKeepBits;

uint EmitMeshletSlot(uint ArgIdx, uint InstanceID, uint MeshletIdx)
{
    FRenderBucket* Bucket = Pass().Buckets + ArgIdx;
    const uint Slot = AppendReserve(&Bucket.DrawCursor, Bucket.DrawCapacity, Bucket.DrawBase);
    if (Slot == kAppendFull)
    {
        return kAppendFull;
    }

    FMeshletDraw Draw;
    Draw.InstanceID        = InstanceID;
    Draw.PackedIndexAndTag = FMeshletDraw::Pack(MeshletIdx, Scene().CullData.MeshletDrawTag);
    MeshletDrawList()[Slot] = Draw;

    return Slot;
}

[shader("amplification")]
[numthreads(MESHLET_TASK_GROUP_SIZE, 1, 1)]
void TaskMain(uint3 GroupID : SV_GroupID, uint ThreadID : SV_GroupIndex, uint DrawIndex : SV_DrawIndex)
{
    // ArgBase is the BUCKET. DrawIndex selects which sub-draw slice of that bucket's block region this
    // workgroup belongs to, which is how a bucket larger than maxTaskWorkGroupCount[0] still dispatches
    // in full instead of being truncated.
    const uint ArgIdx    = Pass().ArgBase;
    const uint BlockSlot = DrawIndex * Pass().MaxTaskGroups + GroupID.x;
    const FCullView View = CullViews()[Pass().CullViewIndex];

    if (ThreadID == 0u)
    {
        gsKeepBits = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Workgroup-uniform, because a block is one instance.
    const FMeshletBlock Block = Pass().BlockList[Pass().Buckets[ArgIdx].BlockBase + BlockSlot];
    const uint InstanceID = Block.InstanceID;
    const uint MeshletIdx = Block.MeshletBase + ThreadID;
    bool       bKeep      = ThreadID < Block.Count;

    FGPUInstance* Inst = Instances() + InstanceID;
    bKeep = bKeep && (Instances() != nullptr) && (Inst.MeshletHeader != nullptr);

    FMeshletRef Ref = (FMeshletRef)0;
    uint MeshletSlot = 0u;

    if (bKeep)
    {
        const FMeshletCullContext Ctx = MakeMeshletCullContext(Inst);
        FMeshletBoundsCache Cache     = MakeMeshletBoundsCache();

        bKeep = TestMeshletAgainstView(Ctx, Inst, View, MeshletIdx, Cache);
    }

    if (bKeep)
    {
        MeshletSlot = EmitMeshletSlot(ArgIdx, InstanceID, MeshletIdx);
        bKeep = MeshletSlot != kAppendFull;
    }

    if (bKeep)
    {
        // Here rather than in the mesh shader: 32 lanes walk 32 dependent pointer chains concurrently
        // instead of one mesh workgroup parking 64 lanes on its own.
        Ref   = ResolveMeshlet(MeshletSlot, View.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY));
        bKeep = Ref.M.VertexCount > 0u && Ref.M.TriangleCount > 0u;
    }

    if (bKeep)
    {
        InterlockedOr(gsKeepBits, 1u << ThreadID);
    }

    GroupMemoryBarrierWithGroupSync();

    const uint KeepBits = gsKeepBits;
    const uint Total    = countbits(KeepBits);

    if (bKeep)
    {
        // (1 << 0) - 1 == 0 and (1 << 31) - 1 == 0x7FFFFFFF, so both ends of the word are exact.
        const uint Out = countbits(KeepBits & ((1u << ThreadID) - 1u));

        gsPayload.Entries[Out].M                = Ref.M;
        gsPayload.Entries[Out].MeshletSlot      = MeshletSlot;
        gsPayload.Entries[Out].GlobalMeshletIdx = Ref.GlobalMeshletIdx;

        gsPayload.InstanceID        = Ref.InstanceID;
        gsPayload.SkinnedVertexBase = Ref.SkinnedVertexBase;
    }

    // Uniform: every lane read the same word after the same barrier. Zero workgroups is legal.
    DispatchMesh(Total, 1, 1, gsPayload);
}
