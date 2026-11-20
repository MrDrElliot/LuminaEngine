#include "RuntimePCH.h"
#include "ForestComponent.h"

#include "Containers/Algorithm.h"

namespace Lumina
{
    FIntVector2 SForestComponent::GetChunkCoord(const FVector3& Center, float X, float Z)
    {
        return FIntVector2((int32)Math::Floor((X - Center.x) / SForestPaintChunk::kSize), (int32)Math::Floor((Z - Center.z) / SForestPaintChunk::kSize));
    }

    SForestPaintChunk* SForestComponent::FindPaint(int32 X, int32 Z, int32 SpeciesIndex)
    {
        auto It = Algo::FindIf(Paint, [&](const SForestPaintChunk& Chunk) { return Chunk.X == X && Chunk.Z == Z && Chunk.Species == SpeciesIndex; });
        return It != Paint.end() ? &*It : nullptr;
    }

    SForestPaintChunk& SForestComponent::FindOrAddPaint(int32 X, int32 Z, int32 SpeciesIndex)
    {
        if (SForestPaintChunk* Existing = FindPaint(X, Z, SpeciesIndex))
        {
            return *Existing;
        }
        SForestPaintChunk& Chunk = Paint.EmplaceGetRef();
        Chunk.X       = X;
        Chunk.Z       = Z;
        Chunk.Species = SpeciesIndex;
        Chunk.Weights.SetNum(SForestPaintChunk::kCells * SForestPaintChunk::kCells, SForestPaintChunk::kNeutral);
        return Chunk;
    }

    void SForestComponent::RemoveSpecies(int32 Index)
    {
        if (Index < 0 || Index >= Species.Num())
        {
            return;
        }
        Species.RemoveAt(Index);
        Paint.RemoveAll([Index](const SForestPaintChunk& Chunk) { return Chunk.Species == Index; });
        Placed.RemoveAll([Index](const SFoliageInstance& Plant) { return Plant.TypeIndex == Index; });
        for (SForestPaintChunk& Chunk : Paint)
        {
            Chunk.Species -= Chunk.Species > Index ? 1 : 0;
        }
        for (SFoliageInstance& Plant : Placed)
        {
            Plant.TypeIndex -= Plant.TypeIndex > Index ? 1 : 0;
        }
    }
}
