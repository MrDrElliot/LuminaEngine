#include "RuntimePCH.h"
#include "DynamicMeshComponent.h"

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Tools/Import/ImportHelpers.h"
#include "TaskSystem/TaskSystem.h"

#include <atomic>

namespace Lumina
{
    // One material-tagged slice of the index buffer.
    struct FDynamicMeshSection
    {
        int32 MaterialSlot = 0;
        int32 StartIndex   = 0;
        int32 IndexCount   = 0;
    };

    // CPU-side staging filled by the setters; consumed and cleared by Commit().
    struct FDynamicMeshBuildData
    {
        TVector<FVector3>          Positions;
        TVector<FVector3>          Normals;   // unpacked; octahedral-packed at Commit
        TVector<FVector2>          UVs;
        TVector<uint32>             Colors;    // RGBA8 packed
        TVector<uint32>             Indices;
        TVector<FDynamicMeshSection> Sections;
    };

    namespace
    {
        constexpr uint32 kCommitGrain = 4096;
        
        void ComputeSmoothNormals(const TVector<FVector3>& Positions, const TVector<uint32>& Indices, TVector<FVector3>& OutNormals)
        {
            LUMINA_PROFILE_SCOPE();

            const uint32 VertexCount   = (uint32)Positions.size();
            const uint32 TriangleCount = (uint32)(Indices.size() / 3);

            OutNormals.assign(VertexCount, FVector3(0.0f));
            if (VertexCount == 0)
            {
                return;
            }

            const FVector3* Pos = Positions.data();
            const uint32*   Idx = Indices.data();

            // A triangle with an out-of-range index contributes nothing, and must not reach the counting
            // sort either (it would index the count array out of bounds), so one test gates every phase.
            auto IsTriangleValid = [Idx, VertexCount](uint32 Tri)
            {
                return Idx[Tri * 3 + 0] < VertexCount
                    && Idx[Tri * 3 + 1] < VertexCount
                    && Idx[Tri * 3 + 2] < VertexCount;
            };
            
            TVector<FVector3> FaceNormals;
            FaceNormals.resize(TriangleCount);
            Task::ParallelFor(TriangleCount, [&](const Task::FParallelRange& Range)
            {
                for (uint32 Tri = Range.Start; Tri < Range.End; ++Tri)
                {
                    if (!IsTriangleValid(Tri))
                    {
                        FaceNormals[Tri] = FVector3(0.0f);
                        continue;
                    }

                    const FVector3& P0 = Pos[Idx[Tri * 3 + 0]];
                    const FVector3& P1 = Pos[Idx[Tri * 3 + 1]];
                    const FVector3& P2 = Pos[Idx[Tri * 3 + 2]];
                    FaceNormals[Tri] = Math::Cross(P1 - P0, P2 - P0);
                }
            }, kCommitGrain);

            TVector<uint32> Offsets;
            Offsets.assign(VertexCount + 1u, 0u);
            Task::ParallelFor(TriangleCount, [&](const Task::FParallelRange& Range)
            {
                for (uint32 Tri = Range.Start; Tri < Range.End; ++Tri)
                {
                    if (!IsTriangleValid(Tri))
                    {
                        continue;
                    }
                    for (uint32 Corner = 0; Corner < 3u; ++Corner)
                    {
                        (void)std::atomic_ref<uint32>(Offsets[Idx[Tri * 3 + Corner]]).fetch_add(1u, std::memory_order_relaxed);
                    }
                }
            }, kCommitGrain);
            
            uint32 Running = 0;
            for (uint32 Vertex = 0; Vertex <= VertexCount; ++Vertex)
            {
                const uint32 Count = Offsets[Vertex];
                Offsets[Vertex] = Running;
                Running += Count;
            }

            TVector<uint32> Cursor(Offsets.begin(), Offsets.begin() + VertexCount);
            TVector<uint32> Adjacency;
            Adjacency.resize(Running);
            Task::ParallelFor(TriangleCount, [&](const Task::FParallelRange& Range)
            {
                for (uint32 Tri = Range.Start; Tri < Range.End; ++Tri)
                {
                    if (!IsTriangleValid(Tri))
                    {
                        continue;
                    }
                    for (uint32 Corner = 0; Corner < 3u; ++Corner)
                    {
                        const uint32 Slot = std::atomic_ref<uint32>(Cursor[Idx[Tri * 3 + Corner]]).fetch_add(1u, std::memory_order_relaxed);
                        Adjacency[Slot] = Tri;
                    }
                }
            }, kCommitGrain);

            // Phase 3: gather. Each vertex owns its own output slot, so there is no sharing at all.
            Task::ParallelFor(VertexCount, [&](const Task::FParallelRange& Range)
            {
                for (uint32 Vertex = Range.Start; Vertex < Range.End; ++Vertex)
                {
                    FVector3 Sum(0.0f);
                    const uint32 Begin = Offsets[Vertex];
                    const uint32 End   = Offsets[Vertex + 1u];
                    for (uint32 Entry = Begin; Entry < End; ++Entry)
                    {
                        Sum += FaceNormals[Adjacency[Entry]];
                    }

                    const float Len = Math::Length(Sum);
                    OutNormals[Vertex] = (Len > 1e-8f) ? (Sum / Len) : FVector3(0.0f, 1.0f, 0.0f);
                }
            }, kCommitGrain);
        }
    }

    FDynamicMeshBuildData& SDynamicMeshComponent::EnsureBuildData()
    {
        if (!BuildData)
        {
            BuildData = MakeShared<FDynamicMeshBuildData>();
        }
        return *BuildData;
    }

    CMaterialInterface* SDynamicMeshComponent::GetMaterialForSlot(size_t Slot) const
    {
        // MaterialOverrides is the only source now. The runtime CStaticMesh this used to fall back to only
        // ever had its Materials array resized to nulls -- it never carried a material -- so nothing is lost.
        if (Slot < MaterialOverrides.size())
        {
            return MaterialOverrides[Slot];
        }
        return nullptr;
    }

    FAABB SDynamicMeshComponent::GetAABB() const
    {
        if (!RenderData)
        {
            return FAABB();
        }

        return FAABB(RenderData->LocalMin, RenderData->LocalMax);
    }

    void SDynamicMeshComponent::RefreshResolvedMaterials()
    {
        if (!RenderData)
        {
            return;
        }

        bool bAllReady = true;
        const TVector<FGeometrySurface>& Geometry = RenderData->Resource.GeometrySurfaces;

        for (size_t i = 0; i < RenderData->Surfaces.size() && i < Geometry.size(); ++i)
        {
            // -1 means unassigned; widening keeps it out of range so it falls through to the default.
            const size_t Slot = (size_t)Geometry[i].MaterialIndex;
            if (!MeshResolve::ResolveSurfaceMaterial(RenderData->Surfaces[i], GetMaterialForSlot(Slot)))
            {
                bAllReady = false;
            }
        }

        RenderData->bAllMaterialsReady = bAllReady;
    }

    void SDynamicMeshComponent::AddSection(int32 MaterialSlot, int32 StartIndex, int32 IndexCount)
    {
        FDynamicMeshSection& Section = EnsureBuildData().Sections.emplace_back();
        Section.MaterialSlot = Math::Max(0, MaterialSlot);
        Section.StartIndex   = Math::Max(0, StartIndex);
        Section.IndexCount   = Math::Max(0, IndexCount);
    }

    void SDynamicMeshComponent::ClearMesh()
    {
        BuildData.reset();
        RenderData.reset();     // GPU buffers retire through ~FMeshBuffers' deferred free
        ++RenderDataVersion;    // the primitive sync polls this; see the field
        CommittedVertexCount   = 0;
        CommittedTriangleCount = 0;
    }

    bool SDynamicMeshComponent::IsBuilt() const
    {
        return RenderData && RenderData->MeshletHeaderAddress != 0;
    }

    int32 SDynamicMeshComponent::GetVertexCount() const
    {
        return BuildData ? (int32)BuildData->Positions.size() : CommittedVertexCount;
    }

    int32 SDynamicMeshComponent::GetTriangleCount() const
    {
        return BuildData ? (int32)(BuildData->Indices.size() / 3) : CommittedTriangleCount;
    }

    void SDynamicMeshComponent::SetPositionsData(const float* Data, int32 FloatCount)
    {
        FDynamicMeshBuildData& BD = EnsureBuildData();
        const int32 Count = FloatCount / 3;
        BD.Positions.resize((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            BD.Positions[i] = FVector3(Data[i * 3 + 0], Data[i * 3 + 1], Data[i * 3 + 2]);
        }
    }

    void SDynamicMeshComponent::SetNormalsData(const float* Data, int32 FloatCount)
    {
        FDynamicMeshBuildData& BD = EnsureBuildData();
        const int32 Count = FloatCount / 3;
        BD.Normals.resize((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            BD.Normals[i] = FVector3(Data[i * 3 + 0], Data[i * 3 + 1], Data[i * 3 + 2]);
        }
    }

    void SDynamicMeshComponent::SetUVsData(const float* Data, int32 FloatCount)
    {
        FDynamicMeshBuildData& BD = EnsureBuildData();
        const int32 Count = FloatCount / 2;
        BD.UVs.resize((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            BD.UVs[i] = FVector2(Data[i * 2 + 0], Data[i * 2 + 1]);
        }
    }

    void SDynamicMeshComponent::SetColorsFloatData(const float* Data, int32 FloatCount)
    {
        FDynamicMeshBuildData& BD = EnsureBuildData();
        const int32 Count = FloatCount / 4;
        BD.Colors.resize((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            BD.Colors[i] = PackColor(FVector4(Data[i * 4 + 0], Data[i * 4 + 1], Data[i * 4 + 2], Data[i * 4 + 3]));
        }
    }

    void SDynamicMeshComponent::SetColorsPackedData(const uint32* Data, int32 Count)
    {
        FDynamicMeshBuildData& BD = EnsureBuildData();
        BD.Colors.assign(Data, Data + Math::Max(0, Count));
    }

    void SDynamicMeshComponent::SetIndicesData(const uint32* Data, int32 Count)
    {
        FDynamicMeshBuildData& BD = EnsureBuildData();
        BD.Indices.assign(Data, Data + Math::Max(0, Count));
    }

    bool SDynamicMeshComponent::Commit()
    {
        if (!BuildData || BuildData->Positions.empty() || BuildData->Indices.empty())
        {
            return false;
        }

        LUMINA_PROFILE_SCOPE();

        FDynamicMeshBuildData& BD = *BuildData;
        const size_t VertexCount = BD.Positions.size();

        TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
        Resource->bSkinnedMesh = false;
        Resource->MaxLODs      = (uint32)Math::Clamp(MaxLODs, 1, (int32)MAX_MESH_LODS);
        Resource->bGenerateTangents = bGenerateTangents;

        // Normals are derived first because deriving them is the only stage that reads Positions and
        // Indices; once it is done both streams can be moved into the resource rather than copied.
        const TVector<FVector3>* SourceNormals = &BD.Normals;
        TVector<FVector3> DerivedNormals;
        if (BD.Normals.size() != VertexCount)
        {
            ComputeSmoothNormals(BD.Positions, BD.Indices, DerivedNormals);
            SourceNormals = &DerivedNormals;
        }

        Resource->Normals.resize(VertexCount);
        Resource->UVs.resize(VertexCount);
        Resource->Colors.resize(VertexCount);

        // Tangents are generated by MikkTSpace inside GenerateMeshlets; start them zeroed.
        Resource->Tangents.assign(VertexCount, 0u);
        
        {
            LUMINA_PROFILE_SECTION("Pack Normals");
            const FVector3* InNormals = SourceNormals->data();
            const FVector2* InUVs     = (BD.UVs.size()    == VertexCount) ? BD.UVs.data()    : nullptr;
            const uint32*   InColors  = (BD.Colors.size() == VertexCount) ? BD.Colors.data() : nullptr;

            uint32* OutNormals = Resource->Normals.data();
            uint32* OutUVs     = Resource->UVs.data();
            uint32* OutColors  = Resource->Colors.data();

            Task::ParallelFor((uint32)VertexCount, [=](const Task::FParallelRange& Range)
            {
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    OutNormals[i] = PackNormal(InNormals[i]);
                    OutUVs[i]     = Math::PackHalf2x16(InUVs ? InUVs[i] : FVector2(0.0f));
                    OutColors[i]  = InColors ? InColors[i] : 0xFFFFFFFFu;
                }
            }, kCommitGrain);
        }
        
        Resource->Positions = eastl::move(BD.Positions);
        Resource->Indices   = eastl::move(BD.Indices);

        int32 MaterialSlotCount = 1;
        if (BD.Sections.empty())
        {
            FGeometrySurface& Surface = Resource->GeometrySurfaces.emplace_back();
            Surface.ID            = "Section0";
            Surface.StartIndex    = 0;
            Surface.IndexCount    = (uint32)Resource->Indices.size();
            Surface.MaterialIndex = 0;
        }
        else
        {
            uint32 SurfaceIndex = 0;
            for (const FDynamicMeshSection& Section : BD.Sections)
            {
                FGeometrySurface& Surface = Resource->GeometrySurfaces.emplace_back();
                Surface.ID            = FName(eastl::string("Section") + eastl::to_string(SurfaceIndex++));
                Surface.StartIndex    = (uint32)Section.StartIndex;
                Surface.IndexCount    = (uint32)Section.IndexCount;
                Surface.MaterialIndex = (int16)Section.MaterialSlot;
                MaterialSlotCount     = Math::Max(MaterialSlotCount, Section.MaterialSlot + 1);
            }
        }

        (void)MaterialSlotCount;   // slots come from MaterialOverrides; nothing to size here anymore

        CommittedVertexCount   = (int32)VertexCount;
        CommittedTriangleCount = (int32)(Resource->Indices.size() / 3);

        // Local bounds from the staged positions, before GenerateMeshlets drops the scratch streams.
        FVector3 Min(FLT_MAX), Max(-FLT_MAX);
        for (const FVector3& P : Resource->Positions)
        {
            Min = Math::Min(Min, P);
            Max = Math::Max(Max, P);
        }

        TSharedPtr<FDynamicMeshRenderData> NewData = MakeShared<FDynamicMeshRenderData>();
        NewData->LocalMin    = Min;
        NewData->LocalMax    = Max;
        NewData->LocalCenter = (Min + Max) * 0.5f;
        NewData->LocalRadius = Math::Length(Max - NewData->LocalCenter);

        Import::Mesh::GenerateMeshlets(*Resource);
        NewData->Resource = eastl::move(*Resource);
        MeshBuffers::CreateForResource(NewData->Resource);
        NewData->MeshletHeaderAddress = NewData->Resource.MeshBuffers.MeshletHeaderBuffer;

        // Geometry half of each surface, straight off the built resource; the material half follows.
        const TVector<FGeometrySurface>& Geometry = NewData->Resource.GeometrySurfaces;
        NewData->Surfaces.resize(Geometry.size());
        for (size_t i = 0; i < Geometry.size(); ++i)
        {
            FResolvedSurface& R = NewData->Surfaces[i];
            R.NumLODs = Geometry[i].NumLODs;
            for (uint32 LOD = 0; LOD < MAX_MESH_LODS; ++LOD)
            {
                R.LODMeshletOffset[LOD] = Geometry[i].LODMeshletOffset[LOD];
                R.LODMeshletCount[LOD]  = Geometry[i].LODMeshletCount[LOD];
                const float Threshold   = Geometry[i].LODScreenThreshold[LOD];
                R.LODScreenThresholdSq[LOD] = Threshold * Threshold;
            }
        }

        // Published before the materials resolve: the old data (and its GPU buffers) drop here, and the
        // extract later this tick already reads the new addresses -- no resolve-cache tick of lag.
        RenderData = eastl::move(NewData);
        ++RenderDataVersion;    // the primitive sync polls this; see the field
        RefreshResolvedMaterials();

        // Scratch streams are dead once the meshlets and GPU buffers exist.
        RenderData->Resource.ClearVertices();
        RenderData->Resource.Indices.clear();
        RenderData->Resource.Indices.shrink_to_fit();

        // Staging is consumed; the next edit re-stages from scratch.
        BuildData.reset();
        return true;
    }
}
