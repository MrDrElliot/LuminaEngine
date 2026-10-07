#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina::SIMD
{
    // Out = A * (1 - Alpha) + B * Alpha over Count floats. Out may alias A or B element-wise.
    RUNTIME_API void LerpArray(float* Out, const float* A, const float* B, int32 Count, float Alpha);

    // Per-element alpha, where Alphas holds Count entries. Out may alias A or B element-wise.
    RUNTIME_API void LerpArrayVarAlpha(float* Out, const float* A, const float* B, const float* Alphas, int32 Count);

    // Out = A + B * S over Count floats.
    RUNTIME_API void AddScaledArray(float* Out, const float* A, const float* B, float S, int32 Count);

    // Out = Base + Dir * Scale over Count floats, with Scale per element.
    RUNTIME_API void MulAddArray(float* Out, const float* Base, const float* Dir, const float* Scale, int32 Count);

    // Out = A * Mix(1, B, S), which is A * (1 + S * (B - 1)).
    RUNTIME_API void MulLerpOneArray(float* Out, const float* A, const float* B, float S, int32 Count);
}
