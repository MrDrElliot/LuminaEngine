#include "RuntimePCH.h"
#include "ForestScatter.h"

#include "Containers/HashPrimitives.h"
#include "World/Entity/Components/ForestComponent.h"

namespace Lumina::Forest
{
    namespace
    {
        FORCEINLINE uint64 CellHash(uint64 Seed, uint32 Species, int32 X, int32 Z, uint32 Stream)
        {
            uint64 H = Containers::CombineHash(Seed, Species);
            H = Containers::CombineHash(H, (uint64)(uint32)X | ((uint64)(uint32)Z << 32));
            return Containers::CombineHash(H, Stream);
        }

        FORCEINLINE float UnitFloat(uint64 Hash)
        {
            return (float)(Hash >> 40) * (1.0f / 16777216.0f);
        }

        // Value noise, so the stands have soft edges that do not depend on how the region was split.
        float StandNoise(uint64 Seed, uint32 Species, float X, float Z)
        {
            const float FX = Math::Floor(X);
            const float FZ = Math::Floor(Z);
            const int32 IX = (int32)FX;
            const int32 IZ = (int32)FZ;
            const float TX = X - FX;
            const float TZ = Z - FZ;
            const float SX = TX * TX * (3.0f - 2.0f * TX);
            const float SZ = TZ * TZ * (3.0f - 2.0f * TZ);
            constexpr uint32 kNoiseStream = 7u;
            const float N00 = UnitFloat(CellHash(Seed, Species, IX,     IZ,     kNoiseStream));
            const float N10 = UnitFloat(CellHash(Seed, Species, IX + 1, IZ,     kNoiseStream));
            const float N01 = UnitFloat(CellHash(Seed, Species, IX,     IZ + 1, kNoiseStream));
            const float N11 = UnitFloat(CellHash(Seed, Species, IX + 1, IZ + 1, kNoiseStream));
            return Math::Mix(Math::Mix(N00, N10, SX), Math::Mix(N01, N11, SX), SZ);
        }

        // Two octaves read as natural stands, where one reads as blobs.
        float StandDensity(uint64 Seed, uint32 Species, const FSpeciesRules& Rules, float X, float Z)
        {
            const float Inv = 1.0f / Rules.ClusterScale;
            const float N = StandNoise(Seed, Species, X * Inv, Z * Inv) * 0.7f
                          + StandNoise(Seed ^ 0x9E3779B97F4A7C15ull, Species, X * Inv * 3.1f, Z * Inv * 3.1f) * 0.3f;
            return N;
        }

        // Remembers the last chunk, since neighboring candidates nearly always share one.
        struct FPaintLookup
        {
            const FRules&      Rules;
            uint64             Key   = ~0ull;
            const FPaintChunk* Chunk = nullptr;

            // From -1 where the species is cleared to 1 where it is filled in, 0 where the rules alone decide.
            float Weight(uint32 Species, float X, float Z)
            {
                const float LocalX = (X - Rules.Center.x) / SForestPaintChunk::kSize;
                const float LocalZ = (Z - Rules.Center.z) / SForestPaintChunk::kSize;
                const int32 CX = (int32)Math::Floor(LocalX);
                const int32 CZ = (int32)Math::Floor(LocalZ);
                const uint64 Packed = FPaint::PackChunk(CX, CZ);
                if (Packed != Key)
                {
                    Key   = Packed;
                    Chunk = Rules.Paint->Chunks.Find(Packed);
                }
                if (Chunk == nullptr || Species >= (uint32)Chunk->Weights.Num() || Chunk->Weights[Species].IsEmpty())
                {
                    return 0.0f;
                }
                constexpr int32 Cells = SForestPaintChunk::kCells;
                const int32 IX = Math::Clamp((int32)((LocalX - (float)CX) * Cells), 0, Cells - 1);
                const int32 IZ = Math::Clamp((int32)((LocalZ - (float)CZ) * Cells), 0, Cells - 1);
                const float Raw = (float)Chunk->Weights[Species][IZ * Cells + IX] - (float)SForestPaintChunk::kNeutral;
                return Math::Clamp(Raw / 127.0f, -1.0f, 1.0f);
            }
        };

        bool IsPainted(const FPaintChunk& Chunk, uint32 Species)
        {
            return Species < (uint32)Chunk.Weights.Num() && !Chunk.Weights[Species].IsEmpty();
        }
    }

    uint64 HashPaint(const SForestComponent& Forest)
    {
        uint64 Hash = Containers::CombineHash((uint64)Forest.Paint.Num(), (uint64)Forest.Placed.Num());
        for (const SForestPaintChunk& Chunk : Forest.Paint)
        {
            const int32 Header[] = { Chunk.X, Chunk.Z, Chunk.Species };
            Hash = Containers::CombineHash(Hash, Containers::HashBytes(Header, sizeof(Header)));
            Hash = Containers::CombineHash(Hash, Containers::HashBytes(Chunk.Weights.GetData(), Chunk.Weights.Num()));
        }
        return Containers::CombineHash(Hash, Containers::HashBytes(Forest.Placed.GetData(), Forest.Placed.Num() * sizeof(SFoliageInstance)));
    }

    void CapturePaint(const SForestComponent& Forest, const FVector3& Center, FPaint& Out)
    {
        constexpr int32 kCellCount = SForestPaintChunk::kCells * SForestPaintChunk::kCells;
        const int32 SpeciesCount = Forest.Species.Num();
        Out.Chunks.Reset();
        for (const SForestPaintChunk& Source : Forest.Paint)
        {
            if (Source.Species < 0 || Source.Species >= SpeciesCount || Source.Weights.Num() != kCellCount)
            {
                continue;
            }
            FPaintChunk& Chunk = Out.Chunks.FindOrAdd(FPaint::PackChunk(Source.X, Source.Z));
            Chunk.Weights.SetNum(SpeciesCount);
            Chunk.Weights[Source.Species] = Source.Weights;
        }
        for (const SFoliageInstance& Source : Forest.Placed)
        {
            if (Source.TypeIndex < 0 || Source.TypeIndex >= SpeciesCount)
            {
                continue;
            }
            const FIntVector2 Coord = SForestComponent::GetChunkCoord(Center, Source.Position.x, Source.Position.z);
            FPaintChunk& Chunk = Out.Chunks.FindOrAdd(FPaint::PackChunk(Coord.x, Coord.y));
            FPlant& Plant = Chunk.Placed.EmplaceGetRef();
            Plant.Position = Source.Position;
            Plant.Rotation = Source.GetRotationQuat();
            Plant.Scale    = Source.Scale.x;
            Plant.Species  = (uint32)Source.TypeIndex;
        }
        for (auto& [Packed, Chunk] : Out.Chunks)
        {
            uint64 Hash = Packed;
            for (int32 Species = 0; Species < Chunk.Weights.Num(); ++Species)
            {
                Hash = Containers::CombineHash(Hash, Containers::HashBytes(Chunk.Weights[Species].GetData(), Chunk.Weights[Species].Num()));
            }
            Chunk.Hash = Containers::CombineHash(Hash, Containers::HashBytes(Chunk.Placed.GetData(), Chunk.Placed.Num() * sizeof(FPlant)));
        }
    }

    FArea GetChunkArea(const FRules& Rules, uint64 PackedChunk)
    {
        FArea Area;
        Area.Min = FVector2(Rules.Center.x + (float)FPaint::ChunkX(PackedChunk) * SForestPaintChunk::kSize,
                            Rules.Center.z + (float)FPaint::ChunkZ(PackedChunk) * SForestPaintChunk::kSize);
        Area.Max = Area.Min + FVector2(SForestPaintChunk::kSize);
        return Area;
    }

    void DiffPaint(const FRules& Rules, const FPaint* Before, const FPaint* After, TVector<FArea>& OutAreas)
    {
        if (After != nullptr)
        {
            for (const auto& [Packed, Chunk] : After->Chunks)
            {
                const FPaintChunk* Old = Before != nullptr ? Before->Chunks.Find(Packed) : nullptr;
                if (Old == nullptr || Old->Hash != Chunk.Hash)
                {
                    OutAreas.Add(GetChunkArea(Rules, Packed));
                }
            }
        }
        if (Before != nullptr)
        {
            for (const auto& [Packed, Chunk] : Before->Chunks)
            {
                if (After == nullptr || !After->Chunks.Contains(Packed))
                {
                    OutAreas.Add(GetChunkArea(Rules, Packed));
                }
            }
        }
    }

    void CaptureRules(const SForestComponent& Forest, const FVector3& Center, TVector<TSharedPtr<const FTerrainSnapshot>> Terrains, FRules& Out)
    {
        Out.Species.Reset();
        Out.Seed     = (uint32)Forest.Seed;
        Out.Center   = Center;
        Out.Extent   = Forest.Extent;
        Out.TileSize = Forest.TileSize;
        Out.Terrains.Reset();
        if (Forest.bFollowTerrain)
        {
            Out.Terrains = Move(Terrains);
        }

        uint64 Hash = Containers::CombineHash(Out.Seed, Containers::HashBytes(&Out.Extent, sizeof(float)));
        Hash = Containers::CombineHash(Hash, Containers::HashBytes(&Out.Center, sizeof(FVector3)));
        Hash = Containers::CombineHash(Hash, Containers::HashBytes(&Out.TileSize, sizeof(float)));
        for (const TSharedPtr<const FTerrainSnapshot>& Terrain : Out.Terrains)
        {
            Hash = Containers::CombineHash(Hash, Terrain->Hash);
        }

        for (const SForestSpecies& Species : Forest.Species)
        {
            FSpeciesRules& R = Out.Species.EmplaceGetRef();
            const SFoliageType& Type = Species.Type;
            R.Density       = Type.Mesh ? Math::Max(Type.Density, 0.0f) : 0.0f;
            R.ScaleMin      = Math::Min(Type.ScaleMin, Type.ScaleMax);
            R.ScaleMax      = Math::Max(Type.ScaleMin, Type.ScaleMax);
            R.ZOffset       = Type.ZOffset;
            R.AlignToNormal = Math::Clamp(Type.AlignToNormal, 0.0f, 1.0f);
            R.bRandomYaw    = Type.bRandomYaw;
            R.MinSlopeCos   = Math::Cos(Math::Radians(Math::Clamp(Species.MaxSlope, 0.0f, 90.0f)));
            R.MinHeight     = Species.MinHeight;
            R.MaxHeight     = Species.MaxHeight;
            R.ClusterScale  = Math::Max(Species.ClusterScale, 1.0f);
            R.Coverage      = Math::Clamp(Species.Coverage, 0.0f, 1.0f);
            R.CullDistance  = Math::Max(Type.CullDistance, 0.0f);

            // Hashed field by field, since the struct carries padding a byte hash would read.
            const float Values[] = { R.Density, R.ScaleMin, R.ScaleMax, R.ZOffset, R.AlignToNormal, R.MinSlopeCos, R.MinHeight, R.MaxHeight, R.ClusterScale, R.Coverage, R.CullDistance };
            Hash = Containers::CombineHash(Hash, Containers::HashBytes(Values, sizeof(Values)));
            Hash = Containers::CombineHash(Hash, R.bRandomYaw ? 1u : 0u);
        }
        Out.Hash = Hash;
    }

    TSharedPtr<FTerrainSnapshot> FTerrainSnapshot::Capture(const FTerrainHeightView& Source)
    {
        const int32 Res = Source.Resolution;
        if (Source.Heights == nullptr || Res < 2)
        {
            return nullptr;
        }

        const int32  Count  = Res * Res;
        const float* Begin  = Source.Heights;
        float Low  = Begin[0];
        float High = Begin[0];
        for (int32 i = 1; i < Count; ++i)
        {
            Low  = Math::Min(Low, Begin[i]);
            High = Math::Max(High, Begin[i]);
        }

        TSharedPtr<FTerrainSnapshot> Out = MakeShared<FTerrainSnapshot>();
        Out->Resolution    = Res;
        Out->TileWorldSize = Source.TileWorldSize;
        Out->MaxHeight     = Source.MaxHeight;
        Out->Origin        = Source.Origin;
        Out->HeightMin     = Low;
        Out->HeightStep    = (High - Low) / 65535.0f;
        Out->Heights.SetNum(Count);
        const float ToStep = Out->HeightStep > 0.0f ? 1.0f / Out->HeightStep : 0.0f;
        for (int32 i = 0; i < Count; ++i)
        {
            Out->Heights[i] = (uint16)Math::Clamp((Begin[i] - Low) * ToStep + 0.5f, 0.0f, 65535.0f);
        }

        const float Shape[] = { Out->TileWorldSize, Out->MaxHeight, Out->HeightMin, Out->HeightStep, Out->Origin.x, Out->Origin.y, Out->Origin.z };
        Out->Hash = Containers::CombineHash(Containers::HashBytes(Out->Heights.GetData(), Count * sizeof(uint16)), Containers::HashBytes(Shape, sizeof(Shape)));
        return Out;
    }

    bool FTerrainSnapshot::SampleHeight(float WorldX, float WorldZ, float& OutHeight) const
    {
        const int32 Res = Resolution;
        if (Heights.IsEmpty() || Res < 2)
        {
            return false;
        }

        const float Stride = TileWorldSize / float(Res - 1);
        const float Lx     = (WorldX - (Origin.x - TileWorldSize * 0.5f)) / Stride;
        const float Lz     = (WorldZ - (Origin.z - TileWorldSize * 0.5f)) / Stride;
        if (Lx < 0.0f || Lz < 0.0f || Lx > float(Res - 1) || Lz > float(Res - 1))
        {
            return false;
        }

        const int32 X0 = int32(Lx), Z0 = int32(Lz);
        const int32 X1 = Math::Min(X0 + 1, Res - 1), Z1 = Math::Min(Z0 + 1, Res - 1);
        const float Tx = Lx - float(X0), Tz = Lz - float(Z0);
        const float H  = Math::Mix(Math::Mix((float)Heights[Z0 * Res + X0], (float)Heights[Z0 * Res + X1], Tx),
                                   Math::Mix((float)Heights[Z1 * Res + X0], (float)Heights[Z1 * Res + X1], Tx), Tz);
        OutHeight = Origin.y + (HeightMin + H * HeightStep) * MaxHeight;
        return true;
    }

    FVector3 FTerrainSnapshot::SampleNormal(float WorldX, float WorldZ) const
    {
        const float Stride = TileWorldSize / float(Math::Max(Resolution - 1, 1));
        const float Eps    = Math::Max(Stride, 0.5f);

        float HxL, HxR, HzL, HzR;
        if (!SampleHeight(WorldX - Eps, WorldZ, HxL) || !SampleHeight(WorldX + Eps, WorldZ, HxR)
            || !SampleHeight(WorldX, WorldZ - Eps, HzL) || !SampleHeight(WorldX, WorldZ + Eps, HzR))
        {
            return FVector3(0.0f, 1.0f, 0.0f);
        }
        return Math::Normalize(FVector3(-(HxR - HxL) / (2.0f * Eps), 1.0f, -(HzR - HzL) / (2.0f * Eps)));
    }

    const FTerrainSnapshot* FindGround(const FRules& Rules, float X, float Z, float& OutHeight)
    {
        for (const TSharedPtr<const FTerrainSnapshot>& Terrain : Rules.Terrains)
        {
            if (Terrain->SampleHeight(X, Z, OutHeight))
            {
                return Terrain.get();
            }
        }
        return nullptr;
    }

    void ScatterRect(const FRules& Rules, const FVector2& Min, const FVector2& Max, TVector<FPlant>& Out, float MinViewDistance)
    {
        const FVector2 ForestMin = Rules.GetMinCorner();
        const FVector2 ForestMax = ForestMin + FVector2(Rules.Extent);
        const FVector2 ClipMin   = Math::Max(Min, ForestMin);
        const FVector2 ClipMax   = Math::Min(Max, ForestMax);
        if (ClipMin.x >= ClipMax.x || ClipMin.y >= ClipMax.y)
        {
            return;
        }

        const bool bFollowTerrain = !Rules.Terrains.IsEmpty();
        const bool bPainted = Rules.Paint && !Rules.Paint->Chunks.IsEmpty();
        const FVector3 Up(0.0f, 1.0f, 0.0f);
        FPaintLookup Lookup{ Rules };

        // Painted chunks clipped to the region, which a species the rules leave out entirely grows in alone.
        TVector<TPair<uint64, FArea>> PaintAreas;
        if (bPainted)
        {
            const FArea Clip{ ClipMin, ClipMax };
            for (const auto& [Packed, Chunk] : Rules.Paint->Chunks)
            {
                const FArea Area = GetChunkArea(Rules, Packed);
                if (Area.Overlaps(Clip))
                {
                    PaintAreas.Emplace(Packed, FArea{ Math::Max(Area.Min, ClipMin), Math::Min(Area.Max, ClipMax) });
                }
            }
        }

        for (uint32 s = 0; s < (uint32)Rules.Species.Num(); ++s)
        {
            const FSpeciesRules& R = Rules.Species[s];
            if (!(R.Density > 0.0f) || (R.CullDistance > 0.0f && R.CullDistance < MinViewDistance))
            {
                continue;
            }

            // One candidate per cell keeps a species evenly spaced, where uniform random points clump.
            const float Cell = 1.0f / Math::Sqrt(R.Density);
            auto ScatterCells = [&](const FVector2& AreaMin, const FVector2& AreaMax)
            {
                const int32 X0 = (int32)Math::Ceil((AreaMin.x - ForestMin.x) / Cell - 0.5f);
                const int32 X1 = (int32)Math::Ceil((AreaMax.x - ForestMin.x) / Cell - 0.5f);
                const int32 Z0 = (int32)Math::Ceil((AreaMin.y - ForestMin.y) / Cell - 0.5f);
                const int32 Z1 = (int32)Math::Ceil((AreaMax.y - ForestMin.y) / Cell - 0.5f);

                for (int32 Z = Z0; Z < Z1; ++Z)
                {
                    for (int32 X = X0; X < X1; ++X)
                    {
                        const uint64 H = CellHash(Rules.Seed, s, X, Z, 0u);
                        const float JX = UnitFloat(H) - 0.5f;
                        const float JZ = UnitFloat(Containers::MixHash64(H)) - 0.5f;
                        const float PX = ForestMin.x + ((float)X + 0.5f + JX * 0.9f) * Cell;
                        const float PZ = ForestMin.y + ((float)Z + 0.5f + JZ * 0.9f) * Cell;

                        // The keep test reads one more hash, so thinning a stand never moves the plants that stay.
                        const float Keep = UnitFloat(CellHash(Rules.Seed, s, X, Z, 1u));
                        float Edge = 0.0f;
                        if (R.Coverage > 0.0f)
                        {
                            const float Stand = StandDensity(Rules.Seed, s, R, PX, PZ);
                            Edge = Math::Clamp((R.Coverage - Stand) * 6.0f + 0.5f, 0.0f, 1.0f);
                        }
                        if (bPainted)
                        {
                            // Read at the cell center, which the region owns, so paint never reaches a plant across a tile border.
                            const float Painted = Lookup.Weight(s, ForestMin.x + ((float)X + 0.5f) * Cell, ForestMin.y + ((float)Z + 0.5f) * Cell);
                            Edge = Painted < 0.0f ? Edge * (1.0f + Painted) : Math::Mix(Edge, 1.0f, Painted);
                        }
                        if (Keep >= Edge)
                        {
                            continue;
                        }

                        FVector3 Position(PX, Rules.Center.y, PZ);
                        FVector3 Normal = Up;
                        if (bFollowTerrain)
                        {
                            const FTerrainSnapshot* Ground = FindGround(Rules, PX, PZ, Position.y);
                            if (Ground == nullptr)
                            {
                                continue;
                            }
                            Normal = Ground->SampleNormal(PX, PZ);
                            if (Normal.y < R.MinSlopeCos)
                            {
                                continue;
                            }
                        }
                        if (Position.y < R.MinHeight || Position.y > R.MaxHeight)
                        {
                            continue;
                        }

                        const uint64 Shape = CellHash(Rules.Seed, s, X, Z, 2u);
                        const FVector3 PlantUp = Math::Normalize(Math::Mix(Up, Normal, R.AlignToNormal));
                        const float Yaw = R.bRandomYaw ? UnitFloat(Shape) * Math::TwoPi<float>() : 0.0f;

                        FPlant& Plant = Out.EmplaceGetRef();
                        Plant.Rotation = Math::RotationBetween(Up, PlantUp) * Math::AngleAxis(Yaw, Up);
                        Plant.Position = Position + PlantUp * R.ZOffset;
                        Plant.Scale    = Math::Mix(R.ScaleMin, R.ScaleMax, UnitFloat(Containers::MixHash64(Shape)));
                        Plant.Species  = s;
                    }
                }
            };

            if (R.Coverage > 0.0f)
            {
                ScatterCells(ClipMin, ClipMax);
                continue;
            }
            for (const TPair<uint64, FArea>& Painted : PaintAreas)
            {
                if (IsPainted(*Rules.Paint->Chunks.Find(Painted.Key), s))
                {
                    ScatterCells(Painted.Value.Min, Painted.Value.Max);
                }
            }
        }

        for (const TPair<uint64, FArea>& Painted : PaintAreas)
        {
            for (const FPlant& Source : Rules.Paint->Chunks.Find(Painted.Key)->Placed)
            {
                const FSpeciesRules& R = Rules.Species[Source.Species];
                const bool bInside = Source.Position.x >= ClipMin.x && Source.Position.x < ClipMax.x && Source.Position.z >= ClipMin.y && Source.Position.z < ClipMax.y;
                if (!bInside || (R.CullDistance > 0.0f && R.CullDistance < MinViewDistance))
                {
                    continue;
                }
                FPlant& Plant = Out.EmplaceGetRef();
                Plant = Source;
                // Placed plants ride the terrain like scattered ones, so sculpting under them keeps them grounded.
                float Ground = 0.0f;
                if (bFollowTerrain && FindGround(Rules, Source.Position.x, Source.Position.z, Ground))
                {
                    Plant.Position.y = Ground + R.ZOffset;
                }
            }
        }
    }
}
