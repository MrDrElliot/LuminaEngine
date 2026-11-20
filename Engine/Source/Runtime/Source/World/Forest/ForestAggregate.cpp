#include "RuntimePCH.h"
#include "ForestAggregate.h"

#include <cfloat>
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Containers/Algorithm.h"
#include "Containers/HashPrimitives.h"
#include "Containers/HashTable.h"
#include "Core/ArtifactCache/ArtifactCache.h"
#include "Core/Engine/Engine.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"
#include "zstd.h"

namespace Lumina::Forest
{
    namespace
    {
        // Bumped whenever the payload or the build changes, so stale artifacts are never read back.
        constexpr uint64 kAggregateFormat = 0xF0E57A66'00000009ull;
        constexpr int    kZstdLevel       = 3;
        constexpr uint32 kPayloadMagic    = 0x46524147u;

        // Appends the triangles of the meshlets Keep accepts, with each vertex once, from the mesh's CPU streams.
        template<typename TKeep>
        void DecodeMeshlets(const FMeshletData& MD, uint32 Begin, uint32 Count, TKeep&& Keep, FSurfaceLevel& Out)
        {
            const int32 NumVertices = MD.VertexPositions.Num();
            const bool  bHasColor   = MD.VertexColors.Num() == NumVertices;
            THashMap<uint32, uint32> Remap;

            auto Emit = [&](uint32 Vertex)
            {
                uint32* Existing = Remap.Find(Vertex);
                if (Existing == nullptr)
                {
                    const uint32 Local = (uint32)Out.Positions.Num();
                    Remap.Add(Vertex, Local);
                    Out.Positions.Add(DecodeMeshPosition(MD.PositionGrid, MD.VertexPositions[Vertex]));
                    Out.Normals.Add(MD.VertexAttributes[Vertex].Normal);
                    Out.Tangents.Add(MD.VertexAttributes[Vertex].Tangent);
                    Out.UVs.Add(MD.VertexAttributes[Vertex].UV);
                    Out.Colors.Add(bHasColor ? MD.VertexColors[Vertex] : 0xFFFFFFFFu);
                    Out.Indices.Add(Local);
                    return;
                }
                Out.Indices.Add(*Existing);
            };

            const uint32 End = Math::Min(Begin + Count, (uint32)MD.Meshlets.Num());
            for (uint32 m = Begin; m < End; ++m)
            {
                if (!Keep(m))
                {
                    continue;
                }
                const FMeshlet& M = MD.Meshlets[m];
                for (uint32 t = 0; t < M.TriangleCount; ++t)
                {
                    const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                    Emit(GetStaticVertexIndex(MD, M, Packed & 0xFFu));
                    Emit(GetStaticVertexIndex(MD, M, (Packed >> 8) & 0xFFu));
                    Emit(GetStaticVertexIndex(MD, M, (Packed >> 16) & 0xFFu));
                }
            }
        }

        struct FSection
        {
            CMaterialInterface* Material = nullptr;
            uint32              Source   = 0;
            TVector<FVector3>   Positions;
            TVector<uint32>     Normals;
            TVector<uint32>     Tangents;
            TVector<uint32>     UVs;
            TVector<uint32>     Colors;
            TVector<uint32>     Indices;
        };

        void AppendTransformed(const FSurfaceLevel& Level, const FPlant& Plant, const FVector3& Origin, FSection& Out)
        {
            const uint32 Base = (uint32)Out.Positions.Num();
            for (int32 v = 0; v < Level.Positions.Num(); ++v)
            {
                Out.Positions.Add(Plant.Position - Origin + Plant.Rotation * (Level.Positions[v] * Plant.Scale));
                Out.Normals.Add(PackNormal(Plant.Rotation * UnpackNormal(Level.Normals[v])));
                const FVector4 Tangent = UnpackTangent(Level.Tangents[v]);
                Out.Tangents.Add(PackTangent(Plant.Rotation * FVector3(Tangent), Tangent.w));
            }
            Out.UVs.Append(Level.UVs.GetData(), Level.UVs.Num());
            Out.Colors.Append(Level.Colors.GetData(), Level.Colors.Num());
            for (uint32 Index : Level.Indices)
            {
                Out.Indices.Add(Base + Index);
            }
        }

        // The coarsest level that still stays under half a voxel of the aggregate, since anything finer only costs build time.
        const FSurfaceLevel* PickLevel(const FSpeciesSurface& Surface, float Scale, float VoxelSize)
        {
            const FSurfaceLevel* Picked = Surface.Levels.IsEmpty() ? nullptr : &Surface.Levels[0];
            for (const FSurfaceLevel& Level : Surface.Levels)
            {
                if (Level.Error * Scale <= VoxelSize * 0.5f)
                {
                    Picked = &Level;
                }
            }
            return Picked;
        }

        void WritePayload(const FAggregateResult& Result, TVector<uint8>& Bytes)
        {
            FMemoryWriter Writer(Bytes);
            uint32 Magic = kPayloadMagic;
            uint32 NumSections = (uint32)Result.SectionSources.Num();
            Writer << Magic;
            Writer << NumSections;
            for (uint32 Source : Result.SectionSources)
            {
                Writer << Source;
            }
            float Bounds[9] = { Result.LocalMin.x, Result.LocalMin.y, Result.LocalMin.z,
                                Result.LocalMax.x, Result.LocalMax.y, Result.LocalMax.z,
                                Result.Origin.x,   Result.Origin.y,   Result.Origin.z };
            for (float& Value : Bounds)
            {
                Writer << Value;
            }
            if (NumSections > 0u)
            {
                Writer << const_cast<FMeshResource&>(Result.Resource);
            }
        }

        // Quantized splats compress well, and the cache would otherwise hold gigabytes for one forest.
        void Compress(const TVector<uint8>& Raw, TVector<uint8>& Out)
        {
            const uint64 RawSize = (uint64)Raw.Num();
            Out.SetNum(IntCastChecked<int32>(sizeof(uint64) + ZSTD_compressBound(Raw.Num())));
            Memory::Memcpy(Out.GetData(), &RawSize, sizeof(uint64));
            const size_t Written = ZSTD_compress(Out.GetData() + sizeof(uint64), Out.Num() - sizeof(uint64), Raw.GetData(), Raw.Num(), kZstdLevel);
            Out.SetNum(ZSTD_isError(Written) ? 0 : IntCastChecked<int32>(sizeof(uint64) + Written));
        }

        bool Decompress(const TVector<uint8>& Packed, TVector<uint8>& Out)
        {
            uint64 RawSize = 0;
            if (Packed.Num() < (int32)sizeof(uint64))
            {
                return false;
            }
            Memory::Memcpy(&RawSize, Packed.GetData(), sizeof(uint64));
            if (RawSize > (1ull << 31))
            {
                return false;
            }
            Out.SetNum((int32)RawSize);
            const size_t Read = ZSTD_decompress(Out.GetData(), Out.Num(), Packed.GetData() + sizeof(uint64), Packed.Num() - sizeof(uint64));
            return !ZSTD_isError(Read) && Read == RawSize;
        }

        bool ReadPayload(const TVector<uint8>& Bytes, FAggregateResult& Out)
        {
            FMemoryReader Reader(Bytes);
            uint32 Magic = 0;
            uint32 NumSections = 0;
            Reader << Magic;
            Reader << NumSections;
            if (Magic != kPayloadMagic || NumSections > 4096u)
            {
                return false;
            }
            Out.SectionSources.SetNum(NumSections);
            for (uint32& Source : Out.SectionSources)
            {
                Reader << Source;
            }
            float Bounds[9] = {};
            for (float& Value : Bounds)
            {
                Reader << Value;
            }
            Out.LocalMin = FVector3(Bounds[0], Bounds[1], Bounds[2]);
            Out.LocalMax = FVector3(Bounds[3], Bounds[4], Bounds[5]);
            Out.Origin   = FVector3(Bounds[6], Bounds[7], Bounds[8]);
            if (NumSections > 0u)
            {
                Reader << Out.Resource;
            }
            Out.bEmpty = NumSections == 0u || Out.Resource.MeshletData.IsEmpty();
            return true;
        }
    }

    TSharedPtr<const FSpeciesGeometry> DecodeSpecies(const CStaticMesh& Mesh)
    {
        const FMeshResource* Resource = Mesh.TryGetMeshResource();
        if (Resource == nullptr || Resource->MeshletData.IsEmpty() || Resource->MeshletData.VertexPositions.IsEmpty())
        {
            return nullptr;
        }

        const FMeshletData& MD = Resource->MeshletData;
        TSharedPtr<FSpeciesGeometry> Geometry = MakeShared<FSpeciesGeometry>();
        uint64 Hash = 0;

        for (const FGeometrySurface& Surface : Resource->GeometrySurfaces)
        {
            FSpeciesSurface& Out = Geometry->Surfaces.EmplaceGetRef();
            Out.Material = Mesh.GetMaterialAtSlot(Surface.MaterialIndex);

            const bool bCluster = Surface.HasClusterLOD();
            const bool bHasBounds = MD.MeshletLODBounds.Num() == MD.Meshlets.Num();
            const uint32 NumSlots = Math::Min(Surface.NumLODs, MAX_MESH_LODS);
            bool bHasVoxels = false;
            for (uint32 Slot = 0; Slot < NumSlots; ++Slot)
            {
                bHasVoxels = bHasVoxels || Surface.IsVoxelLOD(Slot);
            }
            for (uint32 Slot = 0; Slot < NumSlots; ++Slot)
            {
                // A hierarchy slot mixes overlapping levels, so only its source clusters and the voxel slots are whole meshes.
                const bool bVoxel = Surface.IsVoxelLOD(Slot);
                if (bCluster && Slot > 0u && !bVoxel)
                {
                    continue;
                }
                // Even the finest aggregate voxel is coarser than a mesh's own voxel levels, so a scanned tree's millions of source triangles never need decoding.
                if (bHasVoxels && !bVoxel)
                {
                    continue;
                }

                FSurfaceLevel Level;
                Level.Error = Surface.LODError[Slot];
                DecodeMeshlets(MD, Surface.LODMeshletOffset[Slot], Surface.LODMeshletCount[Slot], [&](uint32 Meshlet)
                {
                    return !bCluster || bVoxel || !bHasBounds || MD.MeshletLODBounds[Meshlet].Level == 0u;
                }, Level);
                if (Level.Indices.IsEmpty())
                {
                    continue;
                }

                Hash = Containers::CombineHash(Hash, Containers::HashBytes(Level.Positions.GetData(), Level.Positions.Num() * sizeof(FVector3)));
                Hash = Containers::CombineHash(Hash, Containers::HashBytes(Level.UVs.GetData(), Level.UVs.Num() * sizeof(uint32)));
                Out.Levels.Add(Move(Level));
            }
        }

        Geometry->Hash = Hash;
        return Geometry;
    }

    float GetNodeSize(const FRules& Rules, int32 Level)
    {
        return Rules.TileSize * (float)(1u << (uint32)Math::Clamp(Level, 0, 20));
    }

    FVector2 GetNodeMin(const FRules& Rules, const FNodeKey& Key)
    {
        const float Size = GetNodeSize(Rules, Key.Level);
        return Rules.GetMinCorner() + FVector2((float)Key.X, (float)Key.Z) * Size;
    }

    void BuildAggregate(const FAggregateRequest& Request, FAggregateResult& Out)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        const FRules&  Rules    = *Request.Rules;
        const float    Size     = GetNodeSize(Rules, Request.Key.Level);
        const FVector2 Min      = GetNodeMin(Rules, Request.Key);
        const uint32   Res      = Request.Resolution;

        uint64 CacheKey = Containers::CombineHash(kAggregateFormat, Rules.Hash);
        for (const TSharedPtr<const FSpeciesGeometry>& Geometry : Request.Geometry)
        {
            CacheKey = Containers::CombineHash(CacheKey, Geometry ? Geometry->Hash : 0ull);
        }
        CacheKey = Containers::CombineHash(CacheKey, Request.Key.Pack());
        CacheKey = Containers::CombineHash(CacheKey, Res);
        // Unpainted ground keeps the key it always had, so existing caches stay valid.
        if (const uint64 PaintHash = GetRegionHash(Rules, Min, Min + FVector2(Size)))
        {
            CacheKey = Containers::CombineHash(CacheKey, PaintHash);
        }

        FArtifactCache& Cache = Global::GEngine->GetArtifactCache();
        if (Request.bUseCache)
        {
            TVector<uint8> Packed;
            TVector<uint8> Bytes;
            if (Cache.Load(CacheKey, Packed) && Decompress(Packed, Bytes) && ReadPayload(Bytes, Out))
            {
                return;
            }
        }

        const float VoxelSize = Size / (float)Res;
        TVector<FPlant> Plants;
        ScatterRect(Rules, Min, Min + FVector2(Size), Plants, VoxelSize * Rules.ViewDistancePerVoxel);

        float SumY = 0.0f;
        for (const FPlant& Plant : Plants)
        {
            SumY += Plant.Position.y;
        }
        Out.Origin = FVector3(Min.x + Size * 0.5f, Plants.IsEmpty() ? Rules.Center.y : SumY / (float)Plants.Num(), Min.y + Size * 0.5f);

        TVector<FSection> Sections;
        for (const FPlant& Plant : Plants)
        {
            const FSpeciesGeometry* Geometry = Plant.Species < (uint32)Request.Geometry.Num() ? Request.Geometry[Plant.Species].get() : nullptr;
            if (Geometry == nullptr)
            {
                continue;
            }
            for (uint32 s = 0; s < (uint32)Geometry->Surfaces.Num(); ++s)
            {
                const FSpeciesSurface& Surface = Geometry->Surfaces[s];
                const FSurfaceLevel* Level = PickLevel(Surface, Plant.Scale, VoxelSize);
                if (Level == nullptr)
                {
                    continue;
                }

                auto Found = Algo::FindIf(Sections, [&](const FSection& S) { return S.Material == Surface.Material; });
                FSection* Section = Found != Sections.end() ? &*Found : nullptr;
                if (Section == nullptr)
                {
                    Section = &Sections.EmplaceGetRef();
                    Section->Material = Surface.Material;
                    Section->Source   = (Plant.Species << 16) | s;
                }
                AppendTransformed(*Level, Plant, Out.Origin, *Section);
            }
        }

        FMeshResource& Resource = Out.Resource;
        Resource.bGenerateTangents   = false;
        Resource.bMeshletConeCulling = false;
        Resource.bClusterLODs        = false;
        Resource.MaxLODs             = 1;
        Resource.VoxelFarResolution  = Res;
        Resource.bVoxelLevelsOnly    = true;

        for (FSection& Section : Sections)
        {
            const uint32 Base = (uint32)Resource.Positions.Num();
            FGeometrySurface& Surface = Resource.GeometrySurfaces.EmplaceGetRef();
            Surface.MaterialIndex = (int16)(Resource.GeometrySurfaces.Num() - 1);
            Surface.StartIndex    = (uint32)Resource.Indices.Num();
            Surface.IndexCount    = (uint32)Section.Indices.Num();
            for (uint32 Index : Section.Indices)
            {
                Resource.Indices.Add(Base + Index);
            }
            Resource.Positions.Append(Section.Positions.GetData(), Section.Positions.Num());
            Resource.Normals.Append(Section.Normals.GetData(), Section.Normals.Num());
            Resource.Tangents.Append(Section.Tangents.GetData(), Section.Tangents.Num());
            Resource.UVs.Append(Section.UVs.GetData(), Section.UVs.Num());
            Resource.UVs1.Append(Section.UVs.GetData(), Section.UVs.Num());
            Resource.Colors.Append(Section.Colors.GetData(), Section.Colors.Num());
            Out.SectionSources.Add(Section.Source);
            Section = FSection{};
        }

        if (!Resource.Indices.IsEmpty())
        {
            FVector3 BoundsMin(FLT_MAX);
            FVector3 BoundsMax(-FLT_MAX);
            for (const FVector3& P : Resource.Positions)
            {
                BoundsMin = Math::Min(BoundsMin, P);
                BoundsMax = Math::Max(BoundsMax, P);
            }
            Out.LocalMin = BoundsMin;
            Out.LocalMax = BoundsMax;

            Import::Mesh::GenerateMeshlets(Resource);
            // An aggregate is nothing but splats, so it keeps one vertex per splat instead of three.
            Import::Mesh::CompactSplatMeshlets(Resource.MeshletData);
            Resource.ClearVertices();
            Resource.Indices.Reset();
            Resource.Indices.Shrink();
        }

        Out.bEmpty = Resource.MeshletData.IsEmpty();
        if (Out.bEmpty)
        {
            Out.SectionSources.Reset();
        }

        if (Request.bUseCache)
        {
            TVector<uint8> Bytes;
            TVector<uint8> Packed;
            WritePayload(Out, Bytes);
            Compress(Bytes, Packed);
            if (!Packed.IsEmpty())
            {
                Cache.Store(CacheKey, TSpan<const uint8>(Packed.GetData(), Packed.Num()));
            }
        }
    }
}
