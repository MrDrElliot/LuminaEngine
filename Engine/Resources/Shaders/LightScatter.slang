#pragma once
#include "Includes/SceneGlobals.slang"

// One thread per visible light writes it to its depth-sorted slot, which the grid build then walks in order.
[shader("compute")]
[numthreads(LIGHT_CULL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    const uint Visible = DispatchID.x;
    if (Visible >= LightGridWord(LIGHT_GRID_NUM_VISIBLE))
    {
        return;
    }

    const uint Slot    = LightGrid().At(LIGHT_GRID_VISIBLE + 2u * Visible)[0];
    const uint BinRank = LightGrid().At(LIGHT_GRID_VISIBLE + 2u * Visible + 1u)[0];
    const uint Bin     = BinRank >> LIGHT_GRID_RANK_BITS;
    const uint Sorted  = LightGrid().At(LIGHT_GRID_BIN_STARTS + Bin)[0] + (BinRank & ((1u << LIGHT_GRID_RANK_BITS) - 1u));
    if (RetainedLights().Contains(Slot) && ViewLights().Contains(Sorted))
    {
        ViewLights().At(Sorted)[0] = RetainedLights().GetAligned(Slot);
    }
}
