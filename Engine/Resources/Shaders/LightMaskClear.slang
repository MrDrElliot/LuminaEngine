#pragma once
#include "Includes/SceneGlobals.slang"

// Sized by the bin scan to the live tiles times this frame's mask words, so a quiet frame clears only what it uses.
[shader("compute")]
[numthreads(LIGHT_MASK_CLEAR_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    const uint Index = DispatchID.x;
    const uint Live  = Scene().GridSize.x * Scene().GridSize.y * LightGridWord(LIGHT_GRID_MASK_WORDS);
    if (Index < Live && LightTileMasks().Contains(Index))
    {
        LightTileMasks().At(Index)[0] = 0u;
    }
}
