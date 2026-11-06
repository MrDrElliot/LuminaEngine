#include "RuntimePCH.h"
#include "ClusterLODBuilder.h"

#include <cfloat>
#include <meshoptimizer.h>
#include "Containers/HashTable.h"
#include "Memory/MemoryTracking.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina::Import::ClusterLOD
{
    namespace
    {
        // Clusters a group merges before simplifying, which is where the hierarchy's branching factor comes from.
        constexpr size_t kTargetGroupClusters = 8;

        // A level keeping more than this share of its triangles is not worth a switch, so its clusters become roots.
        constexpr float  kMaxKeptRatio = 0.8f;

        // Bounds the walk for a mesh whose groups keep halving, which a million triangles reaches in about fifteen.
        constexpr uint32 kMaxLevels = 24;

        // A lost square of surface counts as an error of half its side, which keeps aggregates detailed until their voxels take over.
        constexpr float kAreaLossErrorScale = 0.5f;

        // Islands grow back only once a group has lost this share of the surface it stands for, so solids never swell.
        constexpr float kMaxKeptAreaRatio = 0.9f;

        // One level grows an island at most this much, since a lone survivor cannot stand for a whole group at once.
        constexpr float kMaxAreaScalePerLevel = 1.5f;

        // Marks a simplified index as one of the group's grown vertices until the serial pass gives them real indices.
        constexpr uint32 kGrownVertexBit = 0x80000000u;

        float TriangleArea(const FVector3& A, const FVector3& B, const FVector3& C)
        {
            return 0.5f * Math::Length(Math::Cross(B - A, C - A));
        }

        float TotalArea(const TVector<FVector3>& Positions, const uint32* Indices, size_t IndexCount)
        {
            double Area = 0.0;
            for (size_t i = 0; i + 2 < IndexCount; i += 3)
            {
                Area += (double)TriangleArea(Positions[Indices[i]], Positions[Indices[i + 1]], Positions[Indices[i + 2]]);
            }
            return (float)Area;
        }

        constexpr size_t kMaxVertices  = MESHLET_MAX_VERTICES;
        constexpr size_t kMaxTriangles = MESHLET_MAX_TRIANGLES;

        struct FCluster
        {
            // Into FBuildOutput::Meshlets.
            uint32          MeshletIndex;
            // Its triangles' corner indices into the output vertices, three per triangle.
            TVector<uint32> Indices;
            FVector3        SelfCenter;
            float           SelfRadius;
            float           SelfError;
            // Area of the source surface this cluster stands for, which its own triangles may have lost part of.
            float           SourceArea;
        };

        struct FGroup
        {
            TVector<uint32>   Members;
            TVector<uint32>   Indices;
            TVector<uint32>   Simplified;
            // Vertices area preservation moved, which the serial pass appends to the output before emitting.
            TVector<FVector3> GrownPositions;
            TVector<uint32>   GrownFrom;
            FVector3          Center = FVector3(0.0f);
            float             Radius = 0.0f;
            float             Error  = 0.0f;
            float             SourceArea = 0.0f;
            bool              bSimplified = false;
        };

        // The sphere enclosing every member sphere, so a parent bound never shrinks below what it covers.
        void ComputeGroupSphere(const TVector<FCluster>& Clusters, const FGroup& Group, FVector3& OutCenter, float& OutRadius)
        {
            TInlineVector<FVector3, 16> Centers;
            TInlineVector<float, 16>    Radii;
            for (uint32 Member : Group.Members)
            {
                Centers.Add(Clusters[Member].SelfCenter);
                Radii.Add(Clusters[Member].SelfRadius);
            }

            const meshopt_Bounds B = meshopt_computeSphereBounds(&Centers[0].x, Centers.Num(), sizeof(FVector3),
                                                                 Radii.GetData(), sizeof(float));
            OutCenter = FVector3(B.center[0], B.center[1], B.center[2]);
            OutRadius = B.radius;
        }

        // Appends meshlets for Indices as clusters of the given level, each carrying Self as its own group bound and a share of SourceArea.
        void EmitClusters(const FBuildInput& In, FBuildOutput& Out, const TVector<uint32>& Indices, uint32 Level,
                          const FVector3& SelfCenter, float SelfRadius, float SelfError, float SourceArea, TVector<FCluster>& OutClusters)
        {
            if (Indices.Num() < 3)
            {
                return;
            }

            const TVector<FVector3>& Positions = Out.Positions;
            const float ConeWeight = In.bConeCulling ? 0.25f : 0.0f;
            const size_t MaxMeshlets = meshopt_buildMeshletsBound(Indices.Num(), kMaxVertices, kMaxTriangles);

            // The builder sets up every vertex it is handed, so a group hands it only the vertices its triangles use.
            TVector<uint32> Unique(Indices.GetData(), Indices.Num());
            Algo::Sort(Unique);
            Unique.SetNum(static_cast<int32>(Algo::Unique(Unique) - Unique.GetData()));

            TVector<FVector3> LocalPositions(Unique.Num());
            for (int32 i = 0; i < Unique.Num(); ++i)
            {
                LocalPositions[i] = Positions[Unique[i]];
            }
            TVector<uint32> LocalIndices(Indices.Num());
            for (int32 i = 0; i < Indices.Num(); ++i)
            {
                LocalIndices[i] = (uint32)(Algo::LowerBound(Unique.begin(), Unique.end(), Indices[i]) - Unique.begin());
            }

            TVector<meshopt_Meshlet> Local(IntCastChecked<int32>(MaxMeshlets));
            TVector<uint32>          Vertices(IntCastChecked<int32>(MaxMeshlets * kMaxVertices));
            TVector<uint8>           Triangles(IntCastChecked<int32>(MaxMeshlets * kMaxTriangles * 3u));

            const size_t Count = meshopt_buildMeshlets(Local.GetData(), Vertices.GetData(), Triangles.GetData(),
                                                       LocalIndices.GetData(), LocalIndices.Num(),
                                                       &LocalPositions[0].x, LocalPositions.Num(), sizeof(FVector3),
                                                       kMaxVertices, kMaxTriangles, ConeWeight);
            Local.SetNum(IntCastChecked<int32>(Count));

            // Back to the surface's vertex space, which every level of the output shares.
            for (uint32& Vertex : Vertices)
            {
                Vertex = Unique[Vertex];
            }

            const int32 FirstCluster = OutClusters.Num();
            double      EmittedArea  = 0.0;
            for (const meshopt_Meshlet& M : Local)
            {
                if (In.bOptimizeMeshlets)
                {
                    meshopt_optimizeMeshlet(&Vertices[M.vertex_offset], &Triangles[M.triangle_offset], M.triangle_count, M.vertex_count);
                }

                FMeshlet Meshlet{};
                Meshlet.VertexOffset   = (uint32)Out.Vertices.Num();
                Meshlet.TriangleOffset = (uint32)Out.Triangles.Num();
                Meshlet.VertexCount    = M.vertex_count;
                Meshlet.TriangleCount  = M.triangle_count;
                Meshlet.LODIndex       = Math::Min(Level, (uint32)MESHLET_LOD_MASK);

                Out.Vertices.Append(&Vertices[M.vertex_offset], M.vertex_count);
                Out.Triangles.Append(&Triangles[M.triangle_offset], M.triangle_count * 3u);

                FMeshletSphere Sphere{};
                FMeshletCone   Cone = kDisabledMeshletCone;
                if (In.bConeCulling)
                {
                    const meshopt_Bounds B = meshopt_computeMeshletBounds(&Vertices[M.vertex_offset], &Triangles[M.triangle_offset],
                                                                          M.triangle_count, &Positions[0].x, Positions.Num(), sizeof(FVector3));
                    Sphere.Center = FVector3(B.center[0], B.center[1], B.center[2]);
                    Sphere.Radius = B.radius;
                    Cone.Axis[0]  = (int8)B.cone_axis_s8[0];
                    Cone.Axis[1]  = (int8)B.cone_axis_s8[1];
                    Cone.Axis[2]  = (int8)B.cone_axis_s8[2];
                    Cone.Cutoff   = (int8)B.cone_cutoff_s8;
                }
                else
                {
                    FVector3 MeshletPositions[MESHLET_MAX_VERTICES];
                    for (uint32 i = 0; i < M.vertex_count; ++i)
                    {
                        MeshletPositions[i] = Positions[Vertices[M.vertex_offset + i]];
                    }
                    const meshopt_Bounds B = meshopt_computeSphereBounds(&MeshletPositions[0].x, M.vertex_count, sizeof(FVector3), nullptr, 0);
                    Sphere.Center = FVector3(B.center[0], B.center[1], B.center[2]);
                    Sphere.Radius = B.radius;
                }

                FMeshletLODBounds Bounds{};
                Bounds.SelfCenter   = SelfCenter;
                Bounds.SelfRadius   = SelfRadius;
                Bounds.SelfError    = SelfError;
                Bounds.ParentCenter = SelfCenter;
                Bounds.ParentRadius = SelfRadius;
                Bounds.ParentError  = kMeshletLODErrorInfinite;
                Bounds.Flags        = 0u;
                Bounds.Level        = Level;

                FCluster& Cluster = OutClusters.EmplaceGetRef();
                Cluster.MeshletIndex = (uint32)Out.Meshlets.Num();
                Cluster.Indices.Reserve(M.triangle_count * 3u);
                for (uint32 t = 0; t < M.triangle_count * 3u; ++t)
                {
                    Cluster.Indices.Add(Vertices[M.vertex_offset + Triangles[M.triangle_offset + t]]);
                }
                Cluster.SourceArea = TotalArea(Positions, Cluster.Indices.GetData(), Cluster.Indices.Num());
                EmittedArea       += Cluster.SourceArea;

                // Level 0 clusters are their own group, so their draw test is the sphere the cull already tests.
                Cluster.SelfCenter = Level == 0u ? Sphere.Center : SelfCenter;
                Cluster.SelfRadius = Level == 0u ? Sphere.Radius : SelfRadius;
                Cluster.SelfError  = SelfError;
                if (Level == 0u)
                {
                    Bounds.SelfCenter   = Sphere.Center;
                    Bounds.SelfRadius   = Sphere.Radius;
                    Bounds.ParentCenter = Sphere.Center;
                    Bounds.ParentRadius = Sphere.Radius;
                }

                Out.Meshlets.Add(Meshlet);
                Out.Spheres.Add(Sphere);
                Out.Cones.Add(Cone);
                Out.LODBounds.Add(Bounds);
            }

            // A simplified cluster stands for the source surface in proportion to what it still covers.
            if (SourceArea > 0.0f && EmittedArea > 0.0)
            {
                const float Share = SourceArea / (float)EmittedArea;
                for (int32 c = FirstCluster; c < OutClusters.Num(); ++c)
                {
                    OutClusters[c].SourceArea *= Share;
                }
            }
        }

        // Which clusters go together, with every cluster placed in exactly one group.
        void PartitionLevel(const TVector<FVector3>& Positions, const TVector<FCluster>& Clusters, TVector<FGroup>& OutGroups)
        {
            TVector<uint32> Concat;
            TVector<uint32> Counts;
            Counts.Reserve(Clusters.Num());
            for (const FCluster& C : Clusters)
            {
                Concat.Append(C.Indices);
                Counts.Add((uint32)C.Indices.Num());
            }

            TVector<uint32> Partition(Clusters.Num());
            const size_t NumGroups = meshopt_partitionClusters(Partition.GetData(), Concat.GetData(), Concat.Num(),
                                                               Counts.GetData(), Clusters.Num(),
                                                               &Positions[0].x, Positions.Num(), sizeof(FVector3),
                                                               kTargetGroupClusters);

            OutGroups.SetNum(IntCastChecked<int32>(NumGroups));
            for (int32 c = 0; c < Clusters.Num(); ++c)
            {
                FGroup& Group = OutGroups[Partition[c]];
                Group.Members.Add((uint32)c);
                Group.Indices.Append(Clusters[c].Indices);
                Group.SourceArea += Clusters[c].SourceArea;
            }
        }

        // A vertex two groups share is pinned for every group, which is what keeps the cut watertight across them.
        void LockSharedVertices(const TVector<uint8>& BaseLocks, int32 VertexCount, const TVector<FGroup>& Groups, TVector<uint8>& OutLocks)
        {
            constexpr uint32 kUnowned = ~0u;
            TVector<uint32> Owner(VertexCount, kUnowned);

            OutLocks.SetNum(VertexCount);
            for (int32 v = 0; v < VertexCount; ++v)
            {
                OutLocks[v] = v < BaseLocks.Num() ? BaseLocks[v] : (uint8)0u;
            }

            for (int32 g = 0; g < Groups.Num(); ++g)
            {
                for (uint32 Vertex : Groups[g].Indices)
                {
                    uint32& Current = Owner[Vertex];
                    if (Current == kUnowned)
                    {
                        Current = (uint32)g;
                    }
                    else if (Current != (uint32)g)
                    {
                        OutLocks[Vertex] |= meshopt_SimplifyVertex_Lock;
                    }
                }
            }
        }

        FORCEINLINE uint64 PositionKey(const FVector3& P)
        {
            uint32 Bits[3];
            memcpy(Bits, &P.x, sizeof(Bits));
            uint64 Key = (uint64)Bits[0] | ((uint64)Bits[1] << 32u);
            Key ^= (uint64)Bits[2] * 0x9E3779B97F4A7C15ull;
            return Key;
        }

        struct FIsland
        {
            FVector3 WeightedCenter = FVector3(0.0f);
            float    Area    = 0.0f;
            bool     bLocked = false;
        };

        // Grows the group's free islands about their centers until the group covers the surface it stands for, returning how far a vertex moved.
        float PreserveArea(const TVector<FVector3>& Positions, const TVector<uint8>& Locks, FGroup& Group, float& AreaAfter)
        {
            const int32 NewCount = Group.Simplified.Num();
            if (NewCount < 3 || !(Group.SourceArea > 0.0f) || AreaAfter >= Group.SourceArea * kMaxKeptAreaRatio)
            {
                return 0.0f;
            }

            // Islands join across shared positions, so a seam split for its attributes still grows as one piece.
            TVector<uint32> Unique(Group.Simplified.GetData(), NewCount);
            Algo::Sort(Unique);
            Unique.SetNum(static_cast<int32>(Algo::Unique(Unique) - Unique.GetData()));

            TVector<uint32> Parent(Unique.Num());
            for (int32 i = 0; i < Unique.Num(); ++i)
            {
                Parent[i] = (uint32)i;
            }
            auto Find = [&](uint32 i) -> uint32
            {
                while (Parent[i] != i)
                {
                    Parent[i] = Parent[Parent[i]];
                    i = Parent[i];
                }
                return i;
            };
            auto Union = [&](uint32 a, uint32 b)
            {
                a = Find(a);
                b = Find(b);
                if (a != b)
                {
                    Parent[Math::Max(a, b)] = Math::Min(a, b);
                }
            };
            auto LocalOf = [&](uint32 Vertex) -> uint32
            {
                return (uint32)(Algo::LowerBound(Unique.begin(), Unique.end(), Vertex) - Unique.begin());
            };

            THashMap<uint64, uint32> ByPosition;
            for (int32 i = 0; i < Unique.Num(); ++i)
            {
                uint32& First = ByPosition.FindOrAdd(PositionKey(Positions[Unique[i]]), (uint32)i);
                Union(First, (uint32)i);
            }
            for (int32 t = 0; t + 2 < NewCount; t += 3)
            {
                const uint32 A = LocalOf(Group.Simplified[t]);
                Union(A, LocalOf(Group.Simplified[t + 1]));
                Union(A, LocalOf(Group.Simplified[t + 2]));
            }

            TVector<FIsland> Islands(Unique.Num());
            for (int32 i = 0; i < Unique.Num(); ++i)
            {
                if ((Locks[Unique[i]] & meshopt_SimplifyVertex_Lock) != 0u)
                {
                    Islands[Find((uint32)i)].bLocked = true;
                }
            }
            for (int32 t = 0; t + 2 < NewCount; t += 3)
            {
                const FVector3& A = Positions[Group.Simplified[t]];
                const FVector3& B = Positions[Group.Simplified[t + 1]];
                const FVector3& C = Positions[Group.Simplified[t + 2]];
                const float Area  = TriangleArea(A, B, C);
                FIsland& Island   = Islands[Find(LocalOf(Group.Simplified[t]))];
                Island.Area           += Area;
                Island.WeightedCenter += (A + B + C) * (Area / 3.0f);
            }

            float FreeArea   = 0.0f;
            float LockedArea = 0.0f;
            for (const FIsland& Island : Islands)
            {
                (Island.bLocked ? LockedArea : FreeArea) += Island.Area;
            }
            if (!(FreeArea > 0.0f) || LockedArea >= Group.SourceArea)
            {
                return 0.0f;
            }

            const float Scale = Math::Clamp(Math::Sqrt((Group.SourceArea - LockedArea) / FreeArea), 1.0f, kMaxAreaScalePerLevel);
            if (Scale <= 1.0001f)
            {
                return 0.0f;
            }

            // Every vertex of a free island gets one grown copy, which the island's triangles then all reference.
            TVector<uint32> GrownOf(Unique.Num(), ~0u);
            float MaxDisplacement = 0.0f;
            for (int32 i = 0; i < Unique.Num(); ++i)
            {
                const FIsland& Island = Islands[Find((uint32)i)];
                if (Island.bLocked || !(Island.Area > 0.0f))
                {
                    continue;
                }
                const FVector3 Center   = Island.WeightedCenter / Island.Area;
                const FVector3 Original = Positions[Unique[i]];
                const FVector3 Grown    = Center + (Original - Center) * Scale;

                GrownOf[i]      = (uint32)Group.GrownPositions.Num() | kGrownVertexBit;
                MaxDisplacement = Math::Max(MaxDisplacement, Math::Length(Grown - Original));
                Group.GrownPositions.Add(Grown);
                Group.GrownFrom.Add(Unique[i]);
            }
            for (uint32& Index : Group.Simplified)
            {
                const uint32 Grown = GrownOf[LocalOf(Index)];
                if (Grown != ~0u)
                {
                    Index = Grown;
                }
            }

            AreaAfter = LockedArea + FreeArea * Scale * Scale;
            return MaxDisplacement;
        }

        // Halves the group's triangles against the shared locks. Error is the mesh-local distance, monotonic over the children.
        void SimplifyGroup(const FBuildInput& In, const FBuildOutput& Out, const TVector<FCluster>& Clusters,
                           const TVector<float>& Attributes, const TVector<uint8>& Locks, FGroup& Group)
        {
            const TVector<FVector3>& Positions = Out.Positions;
            const size_t SourceCount = Group.Indices.Num();
            size_t Target = (SourceCount / 2u / 3u) * 3u;
            if (Target < 3u)
            {
                Target = 3u;
            }

            Group.Simplified.SetNum(IntCastChecked<int32>(SourceCount));

            // Sparse, since a group touches a sliver of the surface and the simplifier otherwise sets up every vertex of it for each group.
            float AbsoluteError = 0.0f;
            const size_t NewCount = meshopt_simplifyWithAttributes(
                Group.Simplified.GetData(), Group.Indices.GetData(), SourceCount,
                &Positions[0].x, Positions.Num(), sizeof(FVector3),
                Attributes.IsEmpty() ? nullptr : Attributes.GetData(), In.AttributeCount * sizeof(float),
                In.AttributeWeights.IsEmpty() ? nullptr : In.AttributeWeights.GetData(), In.AttributeCount,
                Locks.GetData(), Target, FLT_MAX, meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute, &AbsoluteError);
            Group.Simplified.SetNum(IntCastChecked<int32>(NewCount));

            float ChildError = 0.0f;
            for (uint32 Member : Group.Members)
            {
                ChildError = Math::Max(ChildError, Clusters[Member].SelfError);
            }

            ComputeGroupSphere(Clusters, Group, Group.Center, Group.Radius);

            // Needles and cards collapse at almost no quadric cost while the canopy they made thins out, so the survivors grow to cover for them.
            float AreaAfter = TotalArea(Positions, Group.Simplified.GetData(), NewCount);
            const float Displacement = PreserveArea(Positions, Locks, Group, AreaAfter);
            const float AreaError    = Math::Sqrt(Math::Max(Group.SourceArea - AreaAfter, 0.0f)) * kAreaLossErrorScale;

            // Stepping up a level must raise the error, or a child and its parent would both pass one threshold.
            const float OwnError = Math::Max(Math::Max(AbsoluteError, Displacement), AreaError);
            Group.Error       = Math::Max(ChildError * 1.0001f, Math::Max(OwnError, ChildError + In.ErrorScale * 1e-6f));
            Group.bSimplified = NewCount >= 3u && (float)NewCount <= (float)SourceCount * kMaxKeptRatio
                             && (In.MaxLevelError <= 0.0f || Group.Error <= In.MaxLevelError);
        }

        // Gives the group's grown vertices their places in the output, after which Simplified indexes the output like any level.
        void AppendGrownVertices(const FBuildInput& In, FBuildOutput& Out, TVector<float>& Attributes, FGroup& Group)
        {
            if (Group.GrownPositions.IsEmpty())
            {
                return;
            }

            const uint32 Base = (uint32)Out.Positions.Num();
            for (int32 i = 0; i < Group.GrownPositions.Num(); ++i)
            {
                const uint32 From = Group.GrownFrom[i];
                Out.Positions.Add(Group.GrownPositions[i]);
                Out.SourceVertex.Add(Out.SourceVertex[From]);
                if (In.AttributeCount > 0u && !Attributes.IsEmpty())
                {
                    Attributes.Append(&Attributes[From * In.AttributeCount], In.AttributeCount);
                }
            }
            for (uint32& Index : Group.Simplified)
            {
                if ((Index & kGrownVertexBit) != 0u)
                {
                    Index = Base + (Index & ~kGrownVertexBit);
                }
            }
        }
    }

    bool Build(const FBuildInput& In, FBuildOutput& Out)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        Out = FBuildOutput{};
        if (In.Indices.Num() < 3 || In.Positions.IsEmpty())
        {
            return false;
        }

        Out.Positions.Append(In.Positions.GetData(), In.Positions.Num());
        Out.SourceVertex.SetNum(In.Positions.Num());
        for (int32 v = 0; v < In.Positions.Num(); ++v)
        {
            Out.SourceVertex[v] = (uint32)v;
        }
        TVector<float> Attributes(In.Attributes.GetData(), In.Attributes.Num());

        TVector<FCluster> Level;
        {
            TVector<uint32> Source(In.Indices.GetData(), In.Indices.Num());
            EmitClusters(In, Out, Source, 0u, FVector3(0.0f), 0.0f, 0.0f, 0.0f, Level);
        }
        if (Level.IsEmpty())
        {
            return false;
        }

        Out.LevelMeshletBegin.Add(0u);
        Out.LevelMeshletBegin.Add((uint32)Out.Meshlets.Num());
        Out.NumLevels = 1u;

        TVector<FGroup> Groups;
        TVector<uint8>  Locks;
        // A stuck group's clusters never coarsen again, so the neighbors they border must keep those vertices at every later level.
        TVector<uint8>  RootLocks(In.Locks.GetData(), In.Locks.Num());

        while (Level.Num() > 1 && Out.NumLevels < kMaxLevels)
        {
            Groups.Reset();
            PartitionLevel(Out.Positions, Level, Groups);
            LockSharedVertices(RootLocks, Out.Positions.Num(), Groups, Locks);

            Jobs::ParallelForOrSerial((uint32)Groups.Num(), 4u, [&](uint32 GroupIndex)
            {
                SimplifyGroup(In, Out, Level, Attributes, Locks, Groups[GroupIndex]);
            });

            // Emitted serially, since every group appends to the shared output streams.
            TVector<FCluster> Next;
            RootLocks.SetNum(Out.Positions.Num(), (uint8)0u);
            for (FGroup& Group : Groups)
            {
                if (!Group.bSimplified)
                {
                    for (uint32 Vertex : Group.Indices)
                    {
                        RootLocks[Vertex] |= meshopt_SimplifyVertex_Lock;
                    }
                    continue;
                }

                for (uint32 Member : Group.Members)
                {
                    FMeshletLODBounds& Bounds = Out.LODBounds[Level[Member].MeshletIndex];
                    Bounds.ParentCenter = Group.Center;
                    Bounds.ParentRadius = Group.Radius;
                    Bounds.ParentError  = Group.Error;
                }

                AppendGrownVertices(In, Out, Attributes, Group);
                EmitClusters(In, Out, Group.Simplified, Out.NumLevels, Group.Center, Group.Radius, Group.Error, Group.SourceArea, Next);
            }

            if (Next.IsEmpty())
            {
                break;
            }

            Out.LevelMeshletBegin.Add((uint32)Out.Meshlets.Num());
            ++Out.NumLevels;
            Level = Move(Next);
        }

        // Whatever never found a parent is a root, drawn whenever its own error is accepted.
        Out.MaxRootError = 0.0f;
        TVector<FVector3> RootCenters;
        TVector<float>    RootRadii;
        for (const FMeshletLODBounds& Bounds : Out.LODBounds)
        {
            if (Bounds.ParentError >= kMeshletLODErrorInfinite)
            {
                Out.MaxRootError = Math::Max(Out.MaxRootError, Bounds.SelfError);
                RootCenters.Add(Bounds.SelfCenter);
                RootRadii.Add(Bounds.SelfRadius);
            }
        }

        // The roots cover the whole surface between them, so their enclosing sphere is the surface's.
        if (!RootCenters.IsEmpty())
        {
            const meshopt_Bounds B = meshopt_computeSphereBounds(&RootCenters[0].x, RootCenters.Num(), sizeof(FVector3),
                                                                 RootRadii.GetData(), sizeof(float));
            Out.BoundsCenter = FVector3(B.center[0], B.center[1], B.center[2]);
            Out.BoundsRadius = B.radius;
        }

        return !Out.Meshlets.IsEmpty();
    }

    void SetRootParents(FBuildOutput& Out, const FVector3& Center, float Radius, float Error, bool bVoxelParent)
    {
        for (FMeshletLODBounds& Bounds : Out.LODBounds)
        {
            if (Bounds.ParentError >= kMeshletLODErrorInfinite)
            {
                Bounds.ParentCenter = Center;
                Bounds.ParentRadius = Radius;
                Bounds.ParentError  = Error;
                Bounds.Flags       |= bVoxelParent ? MESHLET_LOD_FLAG_VOXEL_PARENT : 0u;
            }
        }
    }
}
