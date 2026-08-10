#include "Includes/SceneGlobals.slang"

// Lays out the pre-skinning dispatch: one workgroup per (skinned slot, block, meshlet). Replaces a CPU array
// that held one descriptor PER MESHLET and was rebuilt and re-uploaded every frame -- the enumeration is
// derivable from what the slot already publishes, so nothing has to be shipped.
//
// Keyed on RETAINED slots, not on visible-buffer indices: skinned instances compact through CullInstances
// like everything else now, so there is no CPU-fed head to walk. The slot list is exactly the set the CPU
// gathered this frame, so a slot in it always has this frame's data and needs no staleness test.
//
// Serial one-workgroup scan, same shape as BuildDrawPrefix: the gathered skeletal set is tens to hundreds of
// slots, so a hierarchical scan would cost more in launch overhead than it saves.
//
// TWO blocks per slot, because a skeletal mesh can be pre-skinned at two different LODs in one frame: the
// surface LOD it rasterizes at, and a coarser shadow LOD. Each block owns its own compacted slice, so each
// needs its own base. The pair is (SlotListIndex * 2 + bShadow).

struct FDispatchIndirectArguments
{
    uint GroupX;
    uint GroupY;
    uint GroupZ;
};

struct FBuildSkinWorkArgs
{
    // Entries in SlotList: the skinned slots gathered this frame.
    uint  NumSlots;
    uint  _Pad0;

    uint*              SlotList;
    FSkinnedFrameData* SkinnedData;   // indexed by RETAINED slot, not by SlotList position

    // One running base per (slot, block) pair, which is what turns a flat workgroup id back into a meshlet.
    // Written for EVERY pair, including empty ones, so the search in Skinning.slang never reads a stale slot.
    uint* OutWorkBase;
    FDispatchIndirectArguments* OutDispatchArgs;
};
FBuildSkinWorkArgs* Pass() { return GetPassArgs<FBuildSkinWorkArgs>(); }

// Meshlets this pair pre-skins. The three rejects mirror what the CPU slice assignment decided:
//   - no slice granted (over the pre-skin budget, or the block's meshlet range was malformed)
//   - an empty range
//   - a shadow block that RESOLVED TO THE SURFACE BLOCK, which the surface pass already covers. Compared on
//     the meshlet OFFSET, not on the base: two distinct blocks can land on the same base when the shadow
//     LOD's vertices directly follow the surface LOD's, and skipping one of those would drop its vertices.
uint SkinBlockMeshletCount(FSkinnedFrameData Data, bool bShadow)
{
    if (bShadow)
    {
        if (Data.ShadowSkinnedVertexBase == kNoPreSkinBase
            || Data.ShadowMeshletOffset == Data.SurfaceMeshletOffset)
        {
            return 0u;
        }
        return Data.ShadowMeshletCount;
    }

    if (Data.SkinnedVertexBase == kNoPreSkinBase)
    {
        return 0u;
    }
    return Data.SurfaceMeshletCount;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain()
{
    const uint NumSlots = Pass().NumSlots;

    uint Running = 0u;

    if (Pass().SlotList != nullptr && Pass().SkinnedData != nullptr)
    {
        for (uint j = 0u; j < NumSlots; ++j)
        {
            const FSkinnedFrameData Data = Pass().SkinnedData[Pass().SlotList[j]];

            for (uint k = 0u; k < 2u; ++k)
            {
                Pass().OutWorkBase[j * 2u + k] = Running;
                Running += SkinBlockMeshletCount(Data, k != 0u);
            }
        }
    }

    // Folded across Y exactly as every other oversized grid in the renderer; Skinning.slang undoes it.
    FDispatchIndirectArguments Args;
    Args.GroupX = min(Running, (uint)MAX_DISPATCH_AXIS);
    Args.GroupY = (Running + (uint)MAX_DISPATCH_AXIS - 1u) / (uint)MAX_DISPATCH_AXIS;
    Args.GroupZ = 1u;
    Pass().OutDispatchArgs[0] = Args;
}
