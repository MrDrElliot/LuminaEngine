#pragma once
#include "Common.slang"
#include "GlobalRHI.slang"

// Globals for the UI material domain (fullscreen pass into an offscreen brush, no scene).
// Implements the material-codegen surface (GetScreenSize/GetTime/GetMaterial*) without SceneGlobals.

// Must match FUIMaterialBrushArgs in RmlUiRenderer.cpp (scalar layout).
// Field ORDER is load-bearing, not just layout-matching. This block is read through a
// PhysicalStorageBuffer pointer, so it must satisfy relaxed block layout, and that forbids a vector
// straddling a 16-byte boundary. With ScreenSize second it landed at offset 8 and spanned 8..24 --
// illegal, and spirv-val rejected every UI material module because of it. The two scalars fill the
// pointer's tail so the uint4 starts at 16. Keep them there; mirror is FUIMaterialBrushArgs in
// RmlUiRenderer.cpp.
struct FUIMaterialArgs
{
    FMaterialUniforms* Materials;   // 0
    uint  MaterialIndex;            // 8
    float Time;                     // 12
    uint4 ScreenSize;               // 16 -- .xy = brush resolution
};

FUIMaterialArgs* UIArgs() { return GetArgs<FUIMaterialArgs>(); }

float  GetMaterialScalar (uint MatIndex, uint Index) { return UIArgs().Materials[MatIndex].Scalars[Index];  }
float4 GetMaterialVec4   (uint MatIndex, uint Index) { return UIArgs().Materials[MatIndex].Vectors[Index];  }
uint   GetMaterialTexture(uint MatIndex, uint Index) { return UIArgs().Materials[MatIndex].Textures[Index]; }

uint2  GetScreenSize() { return UIArgs().ScreenSize.xy; }
float  GetTime()       { return UIArgs().Time; }

// UI has no camera/world; neutral values for scene-dependent nodes the graph may emit.
float3 GetCameraPosition() { return float3(0.0); }
