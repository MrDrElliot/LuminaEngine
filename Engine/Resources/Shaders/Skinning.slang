#pragma once
#include "Includes/SceneGlobals.slang"

// GPU vertex pre-skinning: one workgroup per meshlet skins its vertices once into
// PreSkinnedVertices(), which all draw passes then read instead of re-skinning per pass.

struct FPushConstants
{
    uint DescriptorCount;
    uint VertexCapacity;    // elements in PreSkinnedVertices(); a store past it would page-fault
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint Lane : SV_GroupThreadID)
{
    // One workgroup per meshlet. Index folded across X/Y (matches the CPU dispatch's 65535 cap).
    uint Descriptor = GroupID.y * 65535u + GroupID.x;
    if (Descriptor >= Pass().DescriptorCount)
    {
        return;
    }

    FSkinDescriptor Desc   = SkinDescriptors()[Descriptor];
    FMeshletHeader* Header = Desc.MeshletHeader;

    // SkinnedVertexBase folds in the span start so the write lands in the compacted slice
    // (same index the draw VS reads). 64 lanes per meshlet.
    FMeshlet M = Header.Meshlets[Desc.MeshletIndex];
    if (Lane < M.VertexCount)
    {
        uint OutIndex = Desc.SkinnedVertexBase + M.VertexOffset + Lane;

        // The CPU budget should keep every index in range; this is the backstop that turns a
        // sizing bug into a missing mesh instead of a device-lost page fault.
        if (OutIndex < Pass().VertexCapacity)
        {
            PreSkinnedVertices()[OutIndex] = SkinVertex(Header, M, Lane, Desc.BoneOffset);
        }
    }
}
