#pragma once
#include "Includes/SceneGlobals.slang"

// Driven off retained slots, since a skinned instance's place in the visible buffer is decided on the GPU.

struct FPushConstants
{
    // One running base per entry of SlotList, subtracted from the work index to find the entry's meshlet.
    GPUSpan<uint>              WorkBase;
    // The SlotList entry each work index belongs to, written by SkinWorkEntries.slang.
    GPUSpan<uint>              WorkEntry;
    GPUSpan<uint>              SlotList;
    GPUSpan<FSkinnedFrameData> SkinnedData;    // indexed by the skinned index SlotList holds
    GPUSpan<FInstanceStatic>   RetainedStatic; // indexed by the entry's RetainedSlot; carries the header pointer

    GPUSpan<FPreSkinnedVertex> OutVertices;
    // Last frame's pose for the same vertices, empty when no view wants motion vectors.
    GPUSpan<FPrevSkinnedPosition> OutPrevPositions;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Staged once per meshlet, so a vertex's four influences are LDS reads, not scattered gathers.
groupshared FBoneTransform GroupBones[SKIN_GROUP_PALETTE_BONES];
groupshared FBoneTransform GroupPrevBones[SKIN_GROUP_PALETTE_BONES];

// Set when any staged bone moved since last frame; a pose held by update-rate skipping leaves it clear.
groupshared uint gsPoseMoved;

bool SameBone(FBoneTransform A, FBoneTransform B)
{
    return A.Rot[0] == B.Rot[0] && A.Rot[1] == B.Rot[1] && A.Rot[2] == B.Rot[2] && A.Rot[3] == B.Rot[3] && A.Rot[4] == B.Rot[4]
        && asuint(A.Tx) == asuint(B.Tx) && asuint(A.Ty) == asuint(B.Ty) && asuint(A.Tz) == asuint(B.Tz);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    // Undoes the Y fold BuildSkinWork applies past MAX_DISPATCH_AXIS groups.
    const uint WorkIndex = GroupID.x + GroupID.y * (uint)MAX_DISPATCH_AXIS;

    // Also rejects folded tail groups, which sit past the last work index.
    if (!Pass().WorkEntry.Contains(WorkIndex))
    {
        return;
    }

    const uint Entry = Pass().WorkEntry.Get(WorkIndex);
    if (!Pass().WorkBase.Contains(Entry) || !Pass().SlotList.Contains(Entry))
    {
        return;
    }
    const uint SkinnedIndex = Pass().SlotList.Get(Entry);

    const FSkinnedFrameData Data = Pass().SkinnedData.Get(SkinnedIndex);
    const uint Slot = Data.RetainedSlot;
    if (!Pass().RetainedStatic.Contains(Slot))
    {
        return;
    }

    // Safe by value because FInstanceStatic carries a header slot, not a device address.
    const FInstanceStatic Static = Pass().RetainedStatic.GetAligned(Slot);

    // Slot 0 is the null header, so a dead or never-uploaded mesh reads zero meshlets.
    FMeshletHeader* Header = Static.MeshletHeader;
    if (Header.MeshletCount == 0u)
    {
        return;
    }

    // Bounds the meshlet against the entry's own range.
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
    if (!HasPreSkinSlice(SkinnedVertexBase) || Data.BoneOffset == kNoBoneSlice)
    {
        return;   // no pre-skin budget or no bone slice; the draw path skins it inline instead
    }

    // Every reject above is workgroup-uniform, which lets the barrier below sit after them.
    const FMeshletBonePalette Palette = GetBonePalette(Header, MeshletIndex);
    const bool bCanSkin = CanSkinMeshlet(Palette, Data.BoneOffset);
    const bool bStaged  = bCanSkin && (Palette.Count <= (uint)SKIN_GROUP_PALETTE_BONES);

    // A shorter snapshot cannot be indexed safely, because ResolvePaletteBone clamps against the live count.
    const bool bWantsPrev      = Pass().OutPrevPositions.Count != 0u;
    const bool bPrevPoseUsable = bWantsPrev && bCanSkin && PrevBones().Count != 0u && PrevBones().Count >= Scene().CullData.BoneNum;

    if (Lane == 0u)
    {
        gsPoseMoved = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (bStaged)
    {
        for (uint i = Lane; i < Palette.Count; i += 64u)
        {
            const uint Bone = ResolvePaletteBone(Header, Palette, i, Data.BoneOffset);
            const FBoneTransform Current = Bones().GetAligned(Bone);
            GroupBones[i] = Current;
            if (bPrevPoseUsable)
            {
                const FBoneTransform Prev = PrevBones().GetAligned(Bone);
                GroupPrevBones[i] = Prev;
                if (!SameBone(Prev, Current))
                {
                    gsPoseMoved = 1u;
                }
            }
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
                float3 Prev = float3(Current.Px, Current.Py, Current.Pz);
                if (bStaged && bPrevPoseUsable)
                {
                    if (gsPoseMoved != 0u)
                    {
                        const uint4 PaletteSlot = min(S.Slots, uint4(Palette.Count - 1u));
                        const FSkinRows PrevRows = BlendSkinRows(GroupPrevBones[PaletteSlot.x], GroupPrevBones[PaletteSlot.y],
                                                                 GroupPrevBones[PaletteSlot.z], GroupPrevBones[PaletteSlot.w], S.Weights);
                        const float4 P = float4(S.Position, 1.0);
                        Prev = float3(dot(PrevRows.R0, P), dot(PrevRows.R1, P), dot(PrevRows.R2, P));
                    }
                }
                else if (bPrevPoseUsable)
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
