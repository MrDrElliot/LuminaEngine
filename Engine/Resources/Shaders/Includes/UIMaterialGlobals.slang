#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// Implements the material-codegen surface for the UI domain without SceneGlobals.

// Mirror of FUIMaterialBrushArgs in RmlUiRenderer.cpp; relaxed block layout forbids a vector straddling a 16-byte boundary.
struct FUIMaterialArgs
{
    GPUSpan<FMaterialUniforms> Materials;   // 0
    uint  MaterialIndex;            // 16
    float Time;                     // 20
    uint2 _Pad0;                    // 24
    uint4 ScreenSize;               // 32, xy is the brush resolution
};

FUIMaterialArgs* UIArgs() { return GetArgs<FUIMaterialArgs>(); }

// A brush renders at its own resolution, with nothing upscaling it.
float GetMaterialMipBias() { return 0.0; }
#include "MaterialSampling.slang"

// Narrow field reads through At, as in SceneGlobals, since the element is 592 B.
float GetMaterialScalar(uint MatIndex, uint Index)
{
    if (!UIArgs().Materials.Contains(MatIndex))
    {
        return 0.0;
    }
    return UIArgs().Materials.At(MatIndex).Scalars[Index];
}

float4 GetMaterialVec4(uint MatIndex, uint Index)
{
    if (!UIArgs().Materials.Contains(MatIndex))
    {
        return float4(0.0);
    }
    return loadAligned<16>(&UIArgs().Materials.At(MatIndex).Vectors[Index]);
}

uint GetMaterialTexture(uint MatIndex, uint Index)
{
    if (!UIArgs().Materials.Contains(MatIndex))
    {
        return 0u;
    }
    return UIArgs().Materials.At(MatIndex).Textures[Index];
}

uint2  GetScreenSize() { return UIArgs().ScreenSize.xy; }
float  GetTime()       { return UIArgs().Time; }

// UI has no camera or world, so scene-dependent nodes get neutral values.
float3 GetCameraPosition() { return float3(0.0); }
