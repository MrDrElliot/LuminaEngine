#pragma once
#include "Includes/SceneGlobals.slang"

// GPU vertex pre-skinning: one workgroup per meshlet skins its vertices once into
// PreSkinnedVertices(), which all draw passes then read instead of re-skinning per pass.
//
// Driven off RETAINED slots, not the visible instance buffer: skinned instances compact through
// CullInstances like everything else, so their position in the visible buffer is decided on the GPU and is
// not addressable from here. Everything this needs is per-slot anyway -- the header and bone base from the
// retained payload, the LOD range and the pre-skin slice from the per-frame side data.
//
// The (slot, block, meshlet) domain is derived from the running bases BuildSkinWork wrote, rather than read
// out of a CPU-built descriptor array. That array held one entry PER MESHLET and was rebuilt and re-uploaded
// every frame; everything in it was already reachable from these two buffers.

struct FPushConstants
{
    // Pairs in WorkBase: NumSlots * 2 (surface block, shadow block). Bounds the search below.
    uint  NumPairs;
    uint  VertexCapacity;    // elements in PreSkinnedVertices(); a store past it would page-fault

    uint*              WorkBase;
    uint*              SlotList;
    FSkinnedFrameData* SkinnedData;    // indexed by RETAINED slot
    FInstanceStatic*   RetainedStatic; // ditto; carries the header pointer and the bone arena base
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Staged once per meshlet, so a vertex's four influences are LDS reads, not scattered 48 B gathers.
groupshared FBoneTransform GroupBones[SKIN_GROUP_PALETTE_BONES];

// Flat workgroup -> owning (slot, block) pair. WorkBase is a running prefix, so this inverts it the same way
// MeshletCull's FindBucketForWork inverts CullWorkBase. Empty pairs repeat the previous base, so the search
// lands on the LAST pair with that base -- which is the one that actually owns the range.
uint FindPairForWork(uint WorkIndex, uint NumPairs)
{
    uint Lo = 0u;
    uint Hi = NumPairs;
    while (Lo + 1u < Hi)
    {
        const uint Mid = (Lo + Hi) >> 1u;
        if (Pass().WorkBase[Mid] <= WorkIndex)
        {
            Lo = Mid;
        }
        else
        {
            Hi = Mid;
        }
    }
    return Lo;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    // Undoes the Y fold BuildSkinWork applies past MAX_DISPATCH_AXIS groups.
    const uint WorkIndex = GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS;
    const uint NumPairs  = Pass().NumPairs;

    // The root carries PreSkinnedVertices only when its buffer exists, and the retained payload's header
    // pointer is whatever the last bind left there. ResolveMeshlet refuses a null header for the same reason;
    // this pass used to reach straight through both. The store was already bounded -- the reads that feed it
    // were not, and it is a read that faults.
    if (NumPairs == 0u || Pass().WorkBase == nullptr || Pass().SlotList == nullptr
        || Pass().SkinnedData == nullptr || Pass().RetainedStatic == nullptr
        || PreSkinnedVertices() == nullptr)
    {
        return;
    }

    const uint Pair    = FindPairForWork(WorkIndex, NumPairs);
    const bool bShadow = (Pair & 1u) != 0u;
    const uint Slot    = Pass().SlotList[Pair >> 1u];

    const FSkinnedFrameData Data = Pass().SkinnedData[Slot];

    // REACHED, never copied into a local: FInstanceStatic carries a buffer-device-address pointer, and a
    // by-value copy makes Slang emit a Function OpVariable that SPIR-V requires to carry Aliased or Restrict
    // and it emits with neither. Invalid module, accepted by every driver, device-lost frames later.
    FInstanceStatic* Static = Pass().RetainedStatic + Slot;

    // A header always exists -- an instance carries a slot and slot 0 is the null header -- so the
    // question is whether it describes geometry. Zero for a mesh that has died or never uploaded.
    FMeshletHeader* Header = Static.MeshletHeader;
    if (Header.MeshletCount == 0u)
    {
        return;
    }

    // The grid is folded up to a rectangle, so the tail workgroups own no meshlet. Also catches a WorkIndex
    // past the last pair's range, which the search cannot reject on its own.
    const uint LocalMeshlet = WorkIndex - Pass().WorkBase[Pair];
    const uint BlockCount   = bShadow ? Data.ShadowMeshletCount : Data.SurfaceMeshletCount;
    if (LocalMeshlet >= BlockCount)
    {
        return;
    }

    const uint MeshletIndex = (bShadow ? Data.ShadowMeshletOffset : Data.SurfaceMeshletOffset) + LocalMeshlet;
    if (MeshletIndex >= min(Data.MeshletTotalCount, Header.MeshletCount))
    {
        // Malformed LOD table, or an instance still naming a mesh whose geometry is gone. Either way an
        // unbounded index walks Header.Meshlets past its allocation, and only the header knows its length.
        return;
    }

    const uint SkinnedVertexBase = bShadow ? Data.ShadowSkinnedVertexBase : Data.SkinnedVertexBase;
    if (SkinnedVertexBase == kNoPreSkinBase)
    {
        return;   // this block lost the pre-skin budget; the draw path skins it inline instead
    }

    // Every reject above is workgroup-UNIFORM, which is what lets the barrier below sit after them.
    const FMeshletBonePalette Palette = GetBonePalette(Header, MeshletIndex);
    const bool bCanSkin = CanSkinMeshlet(Palette);
    const bool bStaged  = bCanSkin && (Palette.Count <= (uint)SKIN_GROUP_PALETTE_BONES);

    if (bStaged)
    {
        for (uint i = Lane; i < Palette.Count; i += 64u)
        {
            GroupBones[i] = Bones()[ResolvePaletteBone(Header, Palette, i, Static.BoneOffset)];
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // SkinnedVertexBase folds in the span start so the write lands in the compacted slice
    // (same index the draw VS reads). 64 lanes per meshlet.
    FMeshlet M = Header.Meshlets[MeshletIndex];
    if (Lane < M.VertexCount)
    {
        uint OutIndex = SkinnedVertexBase + M.VertexOffset + Lane;

        // The CPU budget should keep every index in range; this is the backstop that turns a
        // sizing bug into a missing mesh instead of a device-lost page fault.
        if (OutIndex < Pass().VertexCapacity)
        {
            const FSkinSource S = LoadSkinSource(Header, M, Lane);

            FSkinRows Rows = IdentitySkinRows();
            if (bStaged)
            {
                const uint4 PaletteSlot = min(S.Slots, uint4(Palette.Count - 1u));
                Rows = BlendSkinRows(GroupBones[PaletteSlot.x], GroupBones[PaletteSlot.y],
                                     GroupBones[PaletteSlot.z], GroupBones[PaletteSlot.w], S.Weights);
            }
            else if (bCanSkin)
            {
                Rows = BlendSkinRows(
                    Bones()[ResolvePaletteBone(Header, Palette, S.Slots.x, Static.BoneOffset)],
                    Bones()[ResolvePaletteBone(Header, Palette, S.Slots.y, Static.BoneOffset)],
                    Bones()[ResolvePaletteBone(Header, Palette, S.Slots.z, Static.BoneOffset)],
                    Bones()[ResolvePaletteBone(Header, Palette, S.Slots.w, Static.BoneOffset)],
                    S.Weights);
            }

            PreSkinnedVertices()[OutIndex] = SkinWithRows(S, Rows);
        }
    }
}
