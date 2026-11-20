#include "RuntimePCH.h"
#include "GrassPatch.h"

#include "Core/Math/Quat/Quat.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"

namespace Lumina::GrassPatch
{
    namespace
    {
        // Enough blades that a patch reads as a field rather than a clump, few enough that its finest level stays small.
        constexpr float kTargetBlades = 128.0f;
        constexpr float kMinPatchSize = 1.0f;
        // Sparse plants still tile, so a fern patch stops growing before it covers a clearing.
        constexpr float kMaxPatchSize = 8.0f;
        // Each blade stays this far inside its stratum, so neighbors never stack yet the grid never shows.
        constexpr float kJitter       = 0.8f;

        float Random01(uint32& State)
        {
            State = State * 747796405u + 2891336453u;
            uint32 Word = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
            Word = (Word >> 22u) ^ Word;
            return (float)(Word >> 8u) * (1.0f / 16777216.0f);
        }
    }

    float GetPatchSize(float Density)
    {
        return Math::Clamp(Math::Sqrt(kTargetBlades / Math::Max(Density, 1e-4f)), kMinPatchSize, kMaxPatchSize);
    }

    bool Build(const FMeshResource& Blade, const FParams& Params, FMeshResource& Out)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        const int32 BladeVertices = Blade.GetNumVertices();
        if (BladeVertices == 0 || Blade.Indices.IsEmpty() || Blade.GeometrySurfaces.IsEmpty())
        {
            return false;
        }

        const float Size  = GetPatchSize(Params.Density);
        const int32 Count = Math::Max(1, (int32)Math::Round(Params.Density * Size * Size));
        const int32 Side  = (int32)Math::Ceil(Math::Sqrt((float)Count));

        struct FPlacement
        {
            FVector3 Position;
            FQuat    Rotation;
            float    Scale;
        };
        TVector<FPlacement> Placements;
        Placements.Reserve(Count);
        uint32 State = Params.Seed * 2654435761u + 0x68E31DA4u;
        for (int32 i = 0; i < Count; ++i)
        {
            const float CellX = (float)(i % Side) + 0.5f + (Random01(State) - 0.5f) * kJitter;
            const float CellZ = (float)(i / Side) + 0.5f + (Random01(State) - 0.5f) * kJitter;
            FPlacement& Placement = Placements.EmplaceGetRef();
            Placement.Position = FVector3((CellX / (float)Side - 0.5f) * Size, 0.0f, (CellZ / (float)Side - 0.5f) * Size);
            const float Yaw    = Params.bRandomYaw ? Random01(State) * Math::TwoPi<float>() : 0.0f;
            Placement.Rotation = Math::AngleAxis(Yaw, FVector3(0.0f, 1.0f, 0.0f));
            Placement.Scale    = Math::Mix(Params.ScaleMin, Params.ScaleMax, Random01(State));
        }

        Out.Name             = Blade.Name;
        Out.GeometrySurfaces = Blade.GeometrySurfaces;
        Out.ResizeVertices(BladeVertices * Count);
        for (int32 i = 0; i < Count; ++i)
        {
            const FPlacement& Placement = Placements[i];
            const int32 Base = i * BladeVertices;
            for (int32 v = 0; v < BladeVertices; ++v)
            {
                Out.Positions[Base + v] = Placement.Position + Placement.Rotation * (Blade.Positions[v] * Placement.Scale);
                Out.Normals[Base + v]   = PackNormal(Placement.Rotation * UnpackNormal(Blade.Normals[v]));
                const FVector4 Tangent  = UnpackTangent(Blade.Tangents[v]);
                Out.Tangents[Base + v]  = PackTangent(Placement.Rotation * FVector3(Tangent), Tangent.w);
                Out.UVs[Base + v]       = Blade.UVs[v];
                Out.UVs1[Base + v]      = Blade.UVs1[v];
                Out.Colors[Base + v]    = Blade.Colors[v];
            }
        }

        // Surface by surface, so each material's triangles stay one contiguous range across every blade.
        Out.Indices.Reserve(Blade.Indices.Num() * Count);
        for (int32 s = 0; s < Blade.GeometrySurfaces.Num(); ++s)
        {
            const FGeometrySurface& Source = Blade.GeometrySurfaces[s];
            FGeometrySurface& Surface = Out.GeometrySurfaces[s];
            Surface.StartIndex = (uint32)Out.Indices.Num();
            for (int32 i = 0; i < Count; ++i)
            {
                const uint32 Base = (uint32)(i * BladeVertices);
                for (uint32 n = 0; n < Source.IndexCount; ++n)
                {
                    Out.Indices.Add(Base + Blade.Indices[Source.StartIndex + n]);
                }
            }
            Surface.IndexCount = (uint32)Out.Indices.Num() - Surface.StartIndex;
        }

        Out.bGenerateTangents = false;
        Import::Mesh::GenerateMeshlets(Out);
        return !Out.MeshletData.IsEmpty();
    }
}
