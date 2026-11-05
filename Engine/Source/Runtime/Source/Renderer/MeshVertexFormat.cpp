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
        void ForEachLegacySlot(const TVector<FLegacyMeshlet>& Meshlets, int32 SlotCount, TVisit&& Visit)
        {
            for (const FLegacyMeshlet& M : Meshlets)
            {
                for (uint32 Local = 0; Local < M.VertexCount && static_cast<int64>(M.VertexOffset) + Local < SlotCount; ++Local)
                {
                    Visit(M, static_cast<int32>(M.VertexOffset + Local));
                }
            }
        }

        void ConvertStaticVertices(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets, const TVector<FLegacyMeshletVertex>& Legacy)
        {
            TVector<FLegacyVertexKey> Keys(Legacy.Num());
            memset(Keys.GetData(), 0, Keys.Num() * sizeof(FLegacyVertexKey));

            ForEachLegacySlot(Meshlets, Legacy.Num(), [&](const FLegacyMeshlet& M, int32 Slot)
            {
                const FLegacyMeshletVertex& V = Legacy[Slot];
                Keys[Slot] = { DecodeLegacyPosition(M, V), PackLegacyNormal(V), V.Tangent, V.UV, V.UV1, V.Color };
            });

            TVector<uint32> Remap(Keys.Num());
            const size_t UniqueCount = meshopt_generateVertexRemap(Remap.GetData(), nullptr, Keys.Num(), Keys.GetData(), Keys.Num(), sizeof(FLegacyVertexKey));

            TVector<FLegacyVertexKey> Unique(IntCastChecked<int32>(UniqueCount));
            for (int32 i = 0; i < Keys.Num(); ++i)
            {
                Unique[Remap[i]] = Keys[i];
            }

            TVector<FVector3> Positions(IntCastChecked<int32>(UniqueCount));
            bool bHasUV1   = false;
            bool bHasColor = false;
            for (int32 i = 0, Limit = IntCastChecked<int32>(UniqueCount); i < Limit; ++i)
            {
                Positions[i] = Unique[i].Position;
                bHasUV1      = bHasUV1   || Unique[i].UV1 != Unique[i].UV;
                bHasColor    = bHasColor || Unique[i].Color != 0xFFFFFFFFu;
            }

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.GetData(), Positions.Num());
            Data.VertexPositions.SetNum(IntCastChecked<int32>(UniqueCount));
            Data.VertexAttributes.SetNum(IntCastChecked<int32>(UniqueCount));
            Data.VertexUV1s.SetNum(IntCastChecked<int32>(bHasUV1 ? UniqueCount : 0u));
            Data.VertexColors.SetNum(IntCastChecked<int32>(bHasColor ? UniqueCount : 0u));
            for (int32 i = 0, Limit = IntCastChecked<int32>(UniqueCount); i < Limit; ++i)
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

            for (int32 m = 0; m < Meshlets.Num(); ++m)
            {
                uint32 Vertices[MESHLET_MAX_VERTICES];
                const uint32 Count = Math::Min(Meshlets[m].VertexCount, (uint32)MESHLET_MAX_VERTICES);
                for (uint32 Local = 0; Local < Count; ++Local)
                {
                    const int32 Slot = (size_t)Meshlets[m].VertexOffset + Local;
                    Vertices[Local] = Slot < Remap.Num() ? Remap[Slot] : 0u;
                }
                AppendMeshletVertexRefs(Data, Data.Meshlets[m], Vertices, Count);
            }
        }

        void ConvertSkinnedVertices(FMeshletData& Data, const TVector<FLegacyMeshlet>& Meshlets, const TVector<FLegacyMeshletSkinnedVertex>& Legacy)
        {
            TVector<FVector3> Positions(Legacy.Num(), FVector3(0.0f));
            ForEachLegacySlot(Meshlets, Legacy.Num(), [&](const FLegacyMeshlet& M, int32 Slot)
            {
                Positions[Slot] = DecodeLegacyPosition(M, Legacy[Slot].Base);
            });

            Data.PositionGrid = ComputeMeshPositionGrid(Positions.GetData(), Positions.Num());
            Data.MeshletSkinnedVertices.SetNum(Legacy.Num());
            for (int32 i = 0; i < Legacy.Num(); ++i)
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

        Data.Meshlets.SetNum(Meshlets.Num());
        for (int32 m = 0; m < Meshlets.Num(); ++m)
        {
            const FLegacyMeshlet& In  = Meshlets[m];
            FMeshlet&             Out = Data.Meshlets[m];
            Out = {};
            Out.VertexOffset   = In.VertexOffset;
            Out.TriangleOffset = In.TriangleOffset;
            Out.VertexCount    = Math::Min(In.VertexCount, MESHLET_COUNT_MASK);
            Out.TriangleCount  = Math::Min(In.TriangleCount, MESHLET_COUNT_MASK);
            Out.LODIndex       = Math::Min(In.LODIndex, MESHLET_LOD_MASK);
        }

        Data.MeshletVertexRefs.Reset();
        if (!StaticVertices.IsEmpty())
        {
            ConvertStaticVertices(Data, Meshlets, StaticVertices);
        }
        if (!SkinnedVertices.IsEmpty())
        {
            ConvertSkinnedVertices(Data, Meshlets, SkinnedVertices);
        }
    }
}
