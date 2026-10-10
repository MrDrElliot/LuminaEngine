#pragma once
#include "Includes/SceneGlobals.slang"

// Driven off retained slots, since a skinned instance's place in the visible buffer is decided on the GPU.

struct FPushConstants
{
    // One running base per entry of SlotList, which bounds the search below.
    GPUSpan<uint>              WorkBase;
    GPUSpan<uint>              SlotList;
    GPUSpan<FSkinnedFrameData> SkinnedData;    // indexed by retained slot
    GPUSpan<FInstanceStatic>   RetainedStatic; // ditto; carries the header pointer and the bone arena base

    GPUSpan<FPreSkinnedVertex> OutVertices;
    // Last frame's pose for the same vertices, empty when no view wants motion vectors.
    GPUSpan<FPrevSkinnedPosition> OutPrevPositions;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Staged once per meshlet, so a vertex's four influences are LDS reads, not scattered 48 B gathers.
groupshared FBoneTransform GroupBones[SKIN_GROUP_PALETTE_BONES];

// Inverts the WorkBase prefix like MeshletCull's FindBucketForWork; empty slots repeat a base, so the last one with it owns the range.
uint FindSlotEntryForWork(uint WorkIndex, uint NumEntries)
{
    uint Lo = 0u;
    uint Hi = NumEntries;
    while (Lo + 1u < Hi)
    {
        const uint Mid = (Lo + Hi) >> 1u;
        if (Pass().WorkBase.Get(Mid) <= WorkIndex)
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
    const uint NumEntries = Pass().WorkBase.Count;

    if (NumEntries == 0u)
    {
        return;
    }

    const uint Entry = FindSlotEntryForWork(WorkIndex, NumEntries);
    const uint Slot  = Pass().SlotList.Get(Entry);

    const FSkinnedFrameData Data = Pass().SkinnedData.Get(Slot);

    // Safe by value because FInstanceStatic carries a header slot, not a device address.
    const FInstanceStatic Static = Pass().RetainedStatic.GetAligned(Slot);

    // Slot 0 is the null header, so a dead or never-uploaded mesh reads zero meshlets.
    FMeshletHeader* Header = Static.MeshletHeader;
    if (Header.MeshletCount == 0u)
    {
        return;
    }

    // Rejects folded tail groups and a WorkIndex past the last slot's range, which the search cannot.
    const uint LocalMeshlet = WorkIndex - Pass().WorkBase.Get(Entry);
    if (LocalMeshlet >= Data.SurfaceMeshletCount)
    {
        return;
    }

    const uint MeshletIndex = Data.SurfaceMeshletOffset + LocalMeshlet;
    if (MeshletIndex >= min(Data.MeshletTotalCount, Header.MeshletCount))
    {
        // Malformed LOD table or dead mesh, and only the header knows its meshlet count.
        return;
    }

    const uint SkinnedVertexBase = Data.SkinnedVertexBase;
    if (SkinnedVertexBase == kNoPreSkinBase || Data.BoneOffset == kNoBoneSlice)
    {
        return;   // no pre-skin budget or no bone slice; the draw path skins it inline instead
    }

    // Every reject above is workgroup-uniform, which lets the barrier below sit after them.
    const FMeshletBonePalette Palette = GetBonePalette(Header, MeshletIndex);
    const bool bCanSkin = CanSkinMeshlet(Palette, Data.BoneOffset);
    const bool bStaged  = bCanSkin && (Palette.Count <= (uint)SKIN_GROUP_PALETTE_BONES);

    if (bStaged)
    {
        for (uint i = Lane; i < Palette.Count; i += 64u)
        {
            GroupBones[i] = Bones().GetAligned(ResolvePaletteBone(Header, Palette, i, Data.BoneOffset));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // SkinnedVertexBase folds in the span start, matching the index the draw VS reads.
    FMeshlet M = loadAligned<16>(Header.Meshlets + MeshletIndex);
    if (Lane < M.VertexCount)
    {
        uint OutIndex = SkinnedVertexBase + M.VertexOffset + Lane;

        // Backstop that turns a CPU sizing bug into a missing mesh instead of a device-lost page fault.
        if (Pass().OutVertices.Contains(OutIndex))
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
                    Bones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.x, Data.BoneOffset)),
                    Bones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.y, Data.BoneOffset)),
                    Bones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.z, Data.BoneOffset)),
                    Bones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.w, Data.BoneOffset)),
                    S.Weights);
            }

            const FPreSkinnedVertex Current = SkinWithRows(S, Rows);
            Pass().OutVertices.Set(OutIndex, Current);

            if (Pass().OutPrevPositions.Contains(OutIndex))
            {
                // A shorter snapshot cannot be indexed safely, because ResolvePaletteBone clamps against the live count.
                const bool bPrevPoseUsable = bCanSkin && PrevBones().Count != 0u && PrevBones().Count >= Scene().CullData.BoneNum;
                float3 Prev = float3(Current.Px, Current.Py, Current.Pz);
                if (bPrevPoseUsable)
                {
                    const FSkinRows PrevRows = BlendSkinRows(
                        PrevBones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.x, Data.BoneOffset)),
                        PrevBones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.y, Data.BoneOffset)),
                        PrevBones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.z, Data.BoneOffset)),
                        PrevBones().GetAligned(ResolvePaletteBone(Header, Palette, S.Slots.w, Data.BoneOffset)),
                        S.Weights);
                    const float4 P = float4(S.Position, 1.0);
                    Prev = float3(dot(PrevRows.R0, P), dot(PrevRows.R1, P), dot(PrevRows.R2, P));
                }

                FPrevSkinnedPosition Out;
                Out.X = Prev.x;
                Out.Y = Prev.y;
                Out.Z = Prev.z;
                Pass().OutPrevPositions.Set(OutIndex, Out);
            }
        }
    }
}
