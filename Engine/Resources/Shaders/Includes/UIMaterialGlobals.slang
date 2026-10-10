#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// Implements the material-codegen surface for the UI domain without SceneGlobals.

// Mirror of FUIMaterialBrushArgs in RmlUiRenderer.cpp; relaxed block layout forbids a vector straddling a 16-byte boundary.
struct FUIMaterialArgs
{
    FMaterialTable Materials;       // 0
    uint  MaterialIndex;            // 32
    float Time;                     // 36
    uint2 _Pad0;                    // 40
    uint4 ScreenSize;               // 48, xy is the brush resolution
};

FUIMaterialArgs* UIArgs() { return GetArgs<FUIMaterialArgs>(); }

// A brush renders at its own resolution, with nothing upscaling it.
float GetMaterialMipBias() { return 0.0; }
#include "MaterialSampling.slang"

float  GetMaterialScalar(uint MatIndex, uint Index)  { return UIArgs().Materials.Scalar(MatIndex, Index); }
float4 GetMaterialVec4(uint MatIndex, uint Index)    { return UIArgs().Materials.Vec4(MatIndex, Index); }
uint   GetMaterialTexture(uint MatIndex, uint Index) { return UIArgs().Materials.Texture(MatIndex, Index); }

uint2  GetScreenSize() { return UIArgs().ScreenSize.xy; }
float  GetTime()       { return UIArgs().Time; }

// UI has no camera or world, so scene-dependent nodes get neutral values.
float3 GetCameraPosition() { return float3(0.0); }
