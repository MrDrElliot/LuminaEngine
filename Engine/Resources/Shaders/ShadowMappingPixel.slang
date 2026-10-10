#pragma once
#include "Includes/SceneGlobals.slang"

// Writes no depth so early-Z holds, since SV_Depth would force the PS per covered fragment.

struct FPushConstants
{
    int ShadowDataIndex;
    int ViewIndex;
};

[earlydepthstencil]
[shader("fragment")]
void FragmentMain()
{
}
