#pragma once
#include "SceneGlobals.slang"

bool IsEntitySelected(uint PickerValue, GPUSpan<uint> SelectionBits, uint EntityIndexMask)
{
    // 0 is the Picker's cleared/background value, and the null handle is all-ones; neither is a slot.
    if (PickerValue == 0u || PickerValue == 0xFFFFFFFFu)
    {
        return false;
    }

    const uint Index = PickerValue & EntityIndexMask;

    // Out-of-range reads return zero, meaning not selected.
    return (SelectionBits.Get(Index >> 5u) & (1u << (Index & 31u))) != 0u;
}

// The outline runs over display pixels while the picker holds one id per render pixel.
int2 DisplayToPickerTexel(int2 DisplayTexel)
{
    const float2 Scale = float2(GetScreenSize()) / float2(max(GetDisplaySize(), uint2(1u, 1u)));
    return clamp(int2((float2(DisplayTexel) + 0.5) * Scale), int2(0, 0), int2(GetScreenSize()) - int2(1, 1));
}
