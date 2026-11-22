#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Math/Quat/Quat.h"
#include "Memory/SmartPtr.h"
#include "World/Subsystems/TerrainSculptSystem.h"

namespace Lumina
{
    struct SForestComponent;
}

namespace Lumina::Forest
{
    struct FPlant
    {
        FVector3 Position = FVector3(0.0f);
        FQuat    Rotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        float    Scale    = 1.0f;
        uint32   Species  = 0;
    };

    // A copy of the terrain's heights, which build threads sample while the editor may be sculpting the original.
    struct RUNTIME_API FTerrainSnapshot
    {
        // Quantized across the copy's own range, half a float copy and finer than the 16-bit heightmaps terrains come from.
        TVector<uint16> Heights;
        float           HeightMin     = 0.0f;
        float           HeightStep    = 0.0f;
        int32           Resolution    = 0;
        float           TileWorldSize = 0.0f;
        float           MaxHeight     = 0.0f;
        FVector3        Origin        = FVector3(0.0f);
        uint64          Hash          = 0;

        // Null when the view holds no heights.
        static TSharedPtr<FTerrainSnapshot> Capture(const FTerrainHeightView& Source);

        // The same bilinear height and central-difference normal as FTerrainHeightView.
        bool     SampleHeight(float WorldX, float WorldZ, float& OutHeight) const;
        FVector3 SampleNormal(float WorldX, float WorldZ) const;
    };

    struct FArea
    {
        FVector2 Min = FVector2(0.0f);
        FVector2 Max = FVector2(0.0f);

        bool Overlaps(const FArea& Other) const { return Min.x < Other.Max.x && Other.Min.x < Max.x && Min.y < Other.Max.y && Other.Min.y < Max.y; }
    };

    // One painted square of ground, copied out of the component so workers can read it.
    struct FPaintChunk
    {
        // Per species, the chunk's weights, empty where that species is unpainted here.
        TVector<TVector<uint8>> Weights;
        TVector<FPlant>         Placed;
        uint64                  Hash = 0;
    };

    // The forest's painted cover and placed plants, keyed by packed chunk coordinates from the forest center.
    struct FPaint
    {
        THashMap<uint64, FPaintChunk> Chunks;

        static uint64 PackChunk(int32 X, int32 Z) { return ((uint64)(uint32)X << 32) | (uint64)(uint32)Z; }
        static int32  ChunkX(uint64 Packed)       { return (int32)(uint32)(Packed >> 32); }
        static int32  ChunkZ(uint64 Packed)       { return (int32)(uint32)Packed; }
    };

    // The placement half of a species, plain values a worker can read.
    struct FSpeciesRules
    {
        float Density       = 0.0f;
        float ScaleMin      = 1.0f;
        float ScaleMax      = 1.0f;
        float ZOffset       = 0.0f;
        float AlignToNormal = 0.0f;
        bool  bRandomYaw    = true;
        float MinSlopeCos   = 0.0f;
        float MinHeight     = 0.0f;
        float MaxHeight     = 0.0f;
        float ClusterScale  = 1.0f;
        float Coverage      = 1.0f;
        // Plants of this species vanish past it, 0 for never.
        float CullDistance  = 0.0f;
    };

    // Everything a tile reads, captured on the game thread, so scattering never touches the registry.
    struct FRules
    {
        TVector<FSpeciesRules>            Species;
        uint32                            Seed      = 0;
        FVector3                          Center    = FVector3(0.0f);
        float                             Extent    = 0.0f;
        float                             TileSize  = 64.0f;
        // Finest first, so a plant stands on the most detailed terrain under it and a coarse backdrop carries the rest.
        TVector<TSharedPtr<const FTerrainSnapshot>> Terrains;
        // Changes whenever any placement above does, which rebuilds every tile.
        uint64                            Hash      = 0;
        // Left out of Hash, since a stroke only rebuilds the tiles under the chunks DiffPaint finds changed.
        TSharedPtr<const FPaint>          Paint;

        FVector2 GetMinCorner() const { return FVector2(Center.x - Extent * 0.5f, Center.z - Extent * 0.5f); }
    };

    RUNTIME_API void CaptureRules(const SForestComponent& Forest, const FVector3& Center, TVector<TSharedPtr<const FTerrainSnapshot>> Terrains, FRules& Out);

    // Hashes the component's paint cheaply enough to run every frame, which also catches an undo.
    RUNTIME_API uint64 HashPaint(const SForestComponent& Forest);

    RUNTIME_API void CapturePaint(const SForestComponent& Forest, const FVector3& Center, FPaint& Out);

    RUNTIME_API FArea GetChunkArea(const FRules& Rules, uint64 PackedChunk);

    // The ground under every chunk that differs between two captures.
    RUNTIME_API void DiffPaint(const FRules& Rules, const FPaint* Before, const FPaint* After, TVector<FArea>& OutAreas);

    // The finest terrain holding the point and its height there, or null where none does.
    RUNTIME_API const FTerrainSnapshot* FindGround(const FRules& Rules, float X, float Z, float& OutHeight);

    // Every plant whose cell lies in [Min, Max), the same however the region is split, less species that vanish before MinViewDistance.
    RUNTIME_API void ScatterRect(const FRules& Rules, const FVector2& Min, const FVector2& Max, TVector<FPlant>& Out, float MinViewDistance = 0.0f);
}
