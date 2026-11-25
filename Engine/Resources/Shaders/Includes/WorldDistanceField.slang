#pragma once
#include "Includes/GlobalRHI.slang"

// Every static distance field caster merged into a camera-centered clipmap, toroidally addressed so a level scrolls without moving its texels.

static const uint  kWorldDFLevels     = 7u;
static const uint  kWorldDFResolution = 128u;
static const uint  kWorldDFBrickSize  = 8u;
static const float kWorldDFBandVoxels = 4.0;

// Mirror of Lumina::FWorldDistanceFieldArgs.
struct FWorldDistanceField
{
    // Each level's minimum corner in xyz and its voxel size in w.
    float4     LevelBoxes[kWorldDFLevels];
    FTexture3D Levels[kWorldDFLevels];
    // Zero while the field is off, which every sample then reads as open space.
    uint       NumLevels;

    // The finest level from FirstLevel up holding P a voxel inside its edge, so the trilinear read never wraps onto the far side.
    uint LevelAt(float3 P, uint FirstLevel = 0u)
    {
        [loop]
        for (uint Level = FirstLevel; Level < NumLevels; ++Level)
        {
            // Inside [1, R - 1] on every axis is a voxel's distance from the box center, one compare instead of six.
            const float3 FromCenter = abs((P - LevelBoxes[Level].xyz) / LevelBoxes[Level].w - 0.5 * float(kWorldDFResolution));
            if (max(FromCenter.x, max(FromCenter.y, FromCenter.z)) <= 0.5 * float(kWorldDFResolution) - 1.0)
            {
                return Level;
            }
        }
        return kWorldDFLevels;
    }

    float VoxelSize(uint Level)
    {
        return LevelBoxes[min(Level, kWorldDFLevels - 1u)].w;
    }

    float Band(uint Level)
    {
        return VoxelSize(Level) * kWorldDFBandVoxels;
    }

    float SampleLevel(uint Level, float3 P)
    {
        // Voxel i of the world sits at texel i mod the resolution, so the wrap sampler reads neighbors across the seam correctly.
        const float3 UVW = P / (LevelBoxes[Level].w * float(kWorldDFResolution));
        return Levels[Level].SampleLevel(Samplers::LinearWrap, UVW, 0.0).r;
    }

    // Distance in meters to the nearest merged caster, searching from Level, which returns the level used and kWorldDFLevels past the field.
    // A march resumes from its last level, since its samples move away from the receiver into coarser levels.
    float Sample(float3 P, inout uint Level)
    {
        Level = LevelAt(P, Level);
        return Level < kWorldDFLevels ? SampleLevel(Level, P) : 1.0e6;
    }
};
