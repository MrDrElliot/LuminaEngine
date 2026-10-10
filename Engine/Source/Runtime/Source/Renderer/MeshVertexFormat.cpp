#include "RuntimePCH.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"
#include <meshoptimizer.h>

namespace Lumina
{
    namespace
    {
        FVector3 DecodeLegacyPosition(const FLegacyMeshlet& M, const FLegacyMeshletVertex& V)
        {
            constexpr uint32 kAnchorMask    = 0x00FFFFFFu;
            constexpr uint32 kAnchorSign    = 0x00800000u;
            constexpr uint32 kExponentShift = 24u;

            auto Anchor = [](uint32 Packed) { return (int32)((Packed & kAnchorMask) ^ kAnchorSign) - (int32)kAnchorSign; };
            const float Scale = PositionGridScale((int32)(int8)(uint8)(M.PackedAnchorX >> kExponentShift));

            return FVector3(
                (float)(Anchor(M.PackedAnchorX) + (int32)V.PositionX) * Scale,
                (float)(Anchor(M.PackedAnchorY) + (int32)V.PositionY) * Scale,
                (float)(Anchor(M.PackedAnchorZ) + (int32)V.PositionZ) * Scale);
        }

        uint32 PackLegacyNormal(const FLegacyMeshletVertex& V)
        {
            return PackNormal(FVector3(Math::SNorm16ToFloat(V.NormalX), Math::SNorm16ToFloat(V.NormalY), Math::SNorm16ToFloat(V.NormalZ)));
        }

        // Every field the shared format keeps, decoded, so binary equality means the same vertex.
        struct FLegacyVertexKey
        {
            FVector3 Position;
            uint32   Normal;
            uint32   Tangent;
            uint32   UV;
            uint32   UV1;
            uint32   Color;
        };

        template<typename TVisit>
        void ForEachLegacySlot(const TVector<FLegacyMeshlet>& Meshlets, size_t SlotCount, TVisit&& Visit)
        {
            for (const FLegacyMeshlet& M : Meshlets)
            {
                for (uint32 Local = 0; Local < M.VertexCount && (size_t)M.VertexOffset + Local < SlotCount; ++Local)
                {
                    Visit(M, (size_t)M.VertexOffset + Local);
                }
            }
        }

        void ConvertStaticVertices(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets, const TVector<FLegacyMeshletVertex>& Legacy)
        {
            TVector<FLegacyVertexKey> Keys(Legacy.size());
            memset(Keys.data(), 0, Keys.size() * sizeof(FLegacyVertexKey));

            ForEachLegacySlot(Meshlets, Legacy.size(), [&](const FLegacyMeshlet& M, size_t Slot)
            {
                const FLegacyMeshletVertex& V = Legacy[Slot];
                Keys[Slot] = { DecodeLegacyPosition(M, V), PackLegacyNormal(V), V.Tangent, V.UV, V.UV1, V.Color };
            });

            TVector<uint32> Remap(Keys.size());
            const size_t UniqueCount = meshopt_generateVertexRemap(Remap.data(), nullptr, Keys.size(), Keys.data(), Keys.size(), sizeof(FLegacyVertexKey));

            TVector<FLegacyVertexKey> Unique(UniqueCount);
            for (size_t i = 0; i < Keys.size(); ++i)
            {
                Unique[Remap[i]] = Keys[i];
            }

            TVector<FVector3> Positions(UniqueCount);
            bool bHasUV1   = false;
            bool bHasColor = false;
            for (size_t i = 0; i < UniqueCount; ++i)
            {
                Positions[i] = Unique[i].Position;
                bHasUV1      = bHasUV1   || Unique[i].UV1 != Unique[i].UV;
                bHasColor    = bHasColor || Unique[i].Color != 0xFFFFFFFFu;
            }

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.data(), Positions.size());
            Data.VertexPositions.resize(UniqueCount);
            Data.VertexAttributes.resize(UniqueCount);
            Data.VertexUV1s.resize(bHasUV1 ? UniqueCount : 0u);
            Data.VertexColors.resize(bHasColor ? UniqueCount : 0u);
            for (size_t i = 0; i < UniqueCount; ++i)
            {
                Data.VertexPositions[i]  = EncodeMeshPosition(Data.PositionGrid, Positions[i]);
                Data.VertexAttributes[i] = { Unique[i].Normal, Unique[i].Tangent, Unique[i].UV };
                if (bHasUV1)
                {
                    Data.VertexUV1s[i] = Unique[i].UV1;
                }
                if (bHasColor)
                {
                    Data.VertexColors[i] = Unique[i].Color;
                }
            }

            for (size_t m = 0; m < Meshlets.size(); ++m)
            {
                uint32 Vertices[MESHLET_MAX_VERTICES];
                const uint32 Count = Math::Min(Meshlets[m].VertexCount, (uint32)MESHLET_MAX_VERTICES);
                for (uint32 Local = 0; Local < Count; ++Local)
                {
                    const size_t Slot = (size_t)Meshlets[m].VertexOffset + Local;
                    Vertices[Local] = Slot < Remap.size() ? Remap[Slot] : 0u;
                }
                AppendMeshletVertexRefs(Data, Data.Meshlets[m], Vertices, Count);
            }
        }

        void ConvertSkinnedVertices(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets, const TVector<FLegacyMeshletSkinnedVertex>& Legacy)
        {
            TVector<FVector3> Positions(Legacy.size(), FVector3(0.0f));
            ForEachLegacySlot(Meshlets, Legacy.size(), [&](const FLegacyMeshlet& M, size_t Slot)
            {
                Positions[Slot] = DecodeLegacyPosition(M, Legacy[Slot].Base);
            });

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.data(), Positions.size());
            Data.MeshletSkinnedVertices.resize(Legacy.size());
            for (size_t i = 0; i < Legacy.size(); ++i)
            {
                const FLegacyMeshletVertex& V   = Legacy[i].Base;
                FMeshletSkinnedVertex&      Out = Data.MeshletSkinnedVertices[i];
                Out.Position     = EncodeMeshPosition(Data.PositionGrid, Positions[i]);
                Out.Normal       = PackLegacyNormal(V);
                Out.Tangent      = V.Tangent;
                Out.UV           = V.UV;
                Out.UV1          = V.UV1;
                Out.Color        = V.Color;
                Out.JointIndices = Legacy[i].JointIndices;
                Out.JointWeights = Legacy[i].JointWeights;
            }
        }
    }

    void ConvertLegacyMeshletData(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets,
                                  const TVector<FLegacyMeshletVertex>& StaticVertices,
                                  const TVector<FLegacyMeshletSkinnedVertex>& SkinnedVertices)
    {
        LUMINA_PROFILE_SCOPE();

        Data.Meshlets.resize(Meshlets.size());
        for (size_t m = 0; m < Meshlets.size(); ++m)
        {
            const FLegacyMeshlet& In  = Meshlets[m];
            FMeshlet&             Out = Data.Meshlets[m];
            Out = {};
            Out.VertexOffset   = In.VertexOffset;
            Out.TriangleOffset = In.TriangleOffset;
            Out.VertexCount    = Math::Min(In.VertexCount, (uint32)MESHLET_COUNT_MASK);
            Out.TriangleCount  = Math::Min(In.TriangleCount, (uint32)MESHLET_COUNT_MASK);
            Out.LODIndex       = Math::Min(In.LODIndex, (uint32)MESHLET_LOD_MASK);
        }

        Data.MeshletVertexRefs.clear();
        if (!StaticVertices.empty())
        {
            ConvertStaticVertices(Data, Meshlets, StaticVertices);
        }
        if (!SkinnedVertices.empty())
        {
            ConvertSkinnedVertices(Data, Meshlets, SkinnedVertices);
        }
    }
}
