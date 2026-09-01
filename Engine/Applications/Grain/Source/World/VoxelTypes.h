#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

#include <bit>

namespace Grain
{
    using namespace Lumina;

    inline constexpr int32 kNodeSide   = 8;
    inline constexpr int32 kNodeSlots  = kNodeSide * kNodeSide * kNodeSide;
    inline constexpr int32 kMaskWords  = kNodeSlots / 32;

    inline constexpr float kVoxelSize = 1.0f / 16.0f;

    //~ Three levels of 8, so a root cell is 32 m, a mid cell 4 m and a leaf 0.5 m.

    inline constexpr int32 kRootCountX = 5;
    inline constexpr int32 kRootCountY = 4;
    inline constexpr int32 kRootCountZ = 5;
    inline constexpr int32 kRootCount  = kRootCountX * kRootCountY * kRootCountZ;

    inline constexpr int32 kVoxelsPerRoot = kNodeSide * kNodeSide * kNodeSide;

    inline constexpr int32 kWorldVoxelsX = kRootCountX * kVoxelsPerRoot;
    inline constexpr int32 kWorldVoxelsY = kRootCountY * kVoxelsPerRoot;
    inline constexpr int32 kWorldVoxelsZ = kRootCountZ * kVoxelsPerRoot;

    inline constexpr float kWorldSizeX = float(kWorldVoxelsX) * kVoxelSize;
    inline constexpr float kWorldSizeY = float(kWorldVoxelsY) * kVoxelSize;
    inline constexpr float kWorldSizeZ = float(kWorldVoxelsZ) * kVoxelSize;

    // Voxels covered by one cell, indexed by level, where 0 is a leaf holding single voxels.
    inline constexpr int32 kLevelVoxels[4] = { 1, kNodeSide, kNodeSide * kNodeSide, kVoxelsPerRoot };

    inline constexpr float kSeaLevel = 26.0f;

    enum class EMaterial : uint8
    {
        Air = 0,
        Grass,
        Dirt,
        Stone,
        Rock,
        Sand,
        Snow,
        Water,
        Wood,
        Leaves,
        Gravel,
        Clay,
        Moss,
        Ore,
        Crystal,
        Lava,
        Count,
    };

    inline constexpr uint32 kFlagSolid   = 1u << 0;
    inline constexpr uint32 kFlagUniform = 1u << 1;
    inline constexpr uint32 kFlagPresent = 1u << 2;
    inline constexpr uint32 kFlagLeaf    = 1u << 3;

    // Split from the occupancy masks so both buffers stay tightly packed under the shader's scalar layout.
    struct FVoxNode
    {
        uint32 ChildBase   = 0;
        uint32 Palette     = 0;
        uint32 Flags       = 0;
        uint32 PayloadBase = 0;
    };

    static_assert(sizeof(FVoxNode) == 16, "The Slang mirror of FVoxNode expects a packed 16 byte record.");

    inline uint32 SlotIndex(int32 X, int32 Y, int32 Z)
    {
        return uint32((Y * kNodeSide + Z) * kNodeSide + X);
    }

    inline bool TestBit(const uint32* Mask, uint32 Slot)
    {
        return (Mask[Slot >> 5] & (1u << (Slot & 31u))) != 0u;
    }

    inline void SetBit(uint32* Mask, uint32 Slot)
    {
        Mask[Slot >> 5] |= 1u << (Slot & 31u);
    }

    inline uint32 PopcountBefore(const uint32* Mask, uint32 Slot)
    {
        uint32 Count = 0;
        const uint32 Word = Slot >> 5;
        for (uint32 i = 0; i < Word; ++i)
        {
            Count += uint32(std::popcount(Mask[i]));
        }
        return Count + uint32(std::popcount(Mask[Word] & ((1u << (Slot & 31u)) - 1u)));
    }
}
