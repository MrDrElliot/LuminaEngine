#include "RuntimePCH.h"
#include "SkeletalMeshMerge.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Containers/HashTable.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Threading/Thread.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"
#include "Renderer/SkeletonResource.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina::SkeletalMeshMerge
{
    namespace
    {
        // The smallest run that must stay contiguous, because a surface's per-LOD range is one span.
        struct FCell
        {
            int32  MeshIndex       = 0;
            int32  SurfaceIndex    = 0;
            uint32 LODSlot         = 0;
            uint32 SrcMeshletBegin = 0;
            uint32 MeshletCount    = 0;

            uint32 VertexCount   = 0;
            uint32 TriangleCount = 0;
            uint32 PaletteBones  = 0;

            uint32 DstMeshletBase  = 0;
            uint32 DstVertexBase   = 0;
            uint32 DstTriangleBase = 0;
            uint32 DstPaletteBase  = 0;
        };

        CSkeleton* SkeletonOf(const CSkeletalMesh* Mesh)
        {
            return (Mesh != nullptr && Mesh->Skeleton.IsValid()) ? Mesh->Skeleton.Get() : nullptr;
        }

        bool HasMergeableGeometry(const CSkeletalMesh* Mesh)
        {
            if (Mesh == nullptr || SkeletonOf(Mesh) == nullptr)
            {
                return false;
            }

            const FMeshResource& Resource = Mesh->GetMeshResource();
            return Resource.bSkinnedMesh && !Resource.MeshletData.IsEmpty();
        }
    }

    bool BuildUnifiedSkeleton(TSpan<CSkeleton* const> Skeletons, CSkeleton* Base, FSkeletonResource& Out,
                              TVector<TVector<int32>>& OutBoneRemap, FString& OutError)
    {
        LUMINA_PROFILE_SCOPE();

        Out.Bones.Reset();
        Out.BoneNameToIndex.Reset();
        OutBoneRemap.Reset();
        OutBoneRemap.SetNum(Skeletons.Num());

        // The base goes in whole and first, so its bone ORDER and bind pose survive the merge.
        TVector<CSkeleton*> Ordered;
        Ordered.Reserve(Skeletons.Num() + 1);
        if (Base != nullptr && Base->GetSkeletonResource() != nullptr)
        {
            Ordered.Add(Base);
        }
        for (CSkeleton* Skeleton : Skeletons)
        {
            if (Skeleton != nullptr && Skeleton->GetSkeletonResource() != nullptr && Skeleton != Base)
            {
                Ordered.Add(Skeleton);
            }
        }

        if (Ordered.IsEmpty())
        {
            OutError = "No skeleton to merge: every input was null or carried no skeleton resource.";
            return false;
        }

        for (const CSkeleton* Skeleton : Ordered)
        {
            const FSkeletonResource& Source = *Skeleton->GetSkeletonResource();

            for (int32 i = 0; i < Source.GetNumBones(); ++i)
            {
                const FSkeletonResource::FBoneInfo& Bone = Source.GetBone(i);
                if (Out.BoneNameToIndex.Contains(Bone.Name))
                {
                    continue;   // first definition wins, so the base's bind pose is authoritative
                }

                // Sources are parents-first, so the parent is already in Out unless it roots elsewhere.
                int32 ParentIndex = Constants::kIndexNone;
                if (Bone.ParentIndex != Constants::kIndexNone)
                {
                    const auto It = Out.BoneNameToIndex.Find(Source.GetBone(Bone.ParentIndex).Name);
                    if (It != nullptr)
                    {
                        ParentIndex = (*It);
                    }
                }

                const int32 NewIndex = Out.Bones.Num();
                FSkeletonResource::FBoneInfo& Added = Out.Bones.EmplaceGetRef(Bone);
                Added.ParentIndex = ParentIndex;
                Out.BoneNameToIndex.TryAdd(Bone.Name, NewIndex);
            }
        }

        if (Out.Bones.IsEmpty())
        {
            OutError = "No skeleton to merge: every input skeleton was empty.";
            return false;
        }

        Out.Name = Ordered[0]->GetSkeletonResource()->Name;
        Out.BuildBindPoseCache();

        for (int32 s = 0; s < Skeletons.Num(); ++s)
        {
            const CSkeleton* Skeleton = Skeletons[s];
            if (Skeleton == nullptr || Skeleton->GetSkeletonResource() == nullptr)
            {
                continue;
            }

            const FSkeletonResource& Source = *Skeleton->GetSkeletonResource();
            TVector<int32>& Remap = OutBoneRemap[s];
            Remap.SetNum(Source.GetNumBones(), 0);

            for (int32 i = 0; i < Source.GetNumBones(); ++i)
            {
                const auto It = Out.BoneNameToIndex.Find(Source.GetBone(i).Name);
                Remap[i] = (It != nullptr) ? (*It) : 0;
            }
        }

        return true;
    }

    static FResult MergeUncached(TSpan<CSkeletalMesh* const> Meshes, const FSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();

        FResult Result;

        TVector<CSkeletalMesh*> Sources;
        Sources.Reserve(Meshes.Num());
        for (CSkeletalMesh* Mesh : Meshes)
        {
            if (HasMergeableGeometry(Mesh))
            {
                Sources.Add(Mesh);
            }
        }

        if (Sources.IsEmpty())
        {
            Result.Error = "Nothing to merge: no input was a skinned mesh with baked meshlets and a skeleton.";
            return Result;
        }

        //~ Unified skeleton, and the per-source bone remap the palettes are rewritten through.

        TVector<CSkeleton*> SourceSkeletons;
        SourceSkeletons.Reserve(Sources.Num());
        for (const CSkeletalMesh* Mesh : Sources)
        {
            SourceSkeletons.Add(SkeletonOf(Mesh));
        }

        TUniquePtr<FSkeletonResource> MergedSkeleton = MakeUnique<FSkeletonResource>();
        TVector<TVector<int32>>       BoneRemap;
        if (!BuildUnifiedSkeleton(TSpan<CSkeleton* const>(SourceSkeletons.GetData(), SourceSkeletons.Num()),
                                  Settings.BaseSkeleton, *MergedSkeleton, BoneRemap, Result.Error))
        {
            return Result;
        }

        //~ Material slots, deduplicated so two pieces sharing a material share one slot.

        TVector<TStrongObjectPtr<CMaterialInterface>> MergedMaterials;
        TVector<TVector<int16>>                 MaterialRemap(Sources.Num());

        for (int32 m = 0; m < Sources.Num(); ++m)
        {
            const TVector<TStrongObjectPtr<CMaterialInterface>>& Slots = Sources[m]->Materials;
            MaterialRemap[m].SetNum(Slots.Num(), -1);

            for (int32 s = 0; s < Slots.Num(); ++s)
            {
                int32 Found = Constants::kIndexNone;
                for (int32 e = 0; e < MergedMaterials.Num(); ++e)
                {
                    if (MergedMaterials[e].Get() == Slots[s].Get())
                    {
                        Found = (int32)e;
                        break;
                    }
                }

                if (Found == Constants::kIndexNone)
                {
                    Found = MergedMaterials.Num();
                    MergedMaterials.Add(Slots[s]);
                }
                MaterialRemap[m][s] = (int16)Found;
            }
        }

        //~ LOD-MAJOR as the importer bakes it, so a pre-skin slice spans one LOD's meshlet range.

        TVector<FCell> Cells;
        for (uint32 LOD = 0; LOD < MAX_MESH_LODS; ++LOD)
        {
            for (int32 m = 0; m < Sources.Num(); ++m)
            {
                const FMeshResource& Resource = Sources[m]->GetMeshResource();
                const FMeshletData&  Data     = Resource.MeshletData;

                for (int32 s = 0; s < Resource.GeometrySurfaces.Num(); ++s)
                {
                    const FGeometrySurface& Surface = Resource.GeometrySurfaces[s];
                    if (LOD >= Surface.NumLODs || Surface.LODMeshletCount[LOD] == 0)
                    {
                        continue;
                    }

                    const uint32 Begin = Surface.LODMeshletOffset[LOD];
                    const uint32 End   = Math::Min(Begin + Surface.LODMeshletCount[LOD], (uint32)Data.Meshlets.Num());
                    if (Begin >= End)
                    {
                        continue;
                    }

                    FCell& Cell = Cells.EmplaceGetRef();
                    Cell.MeshIndex       = m;
                    Cell.SurfaceIndex    = s;
                    Cell.LODSlot         = LOD;
                    Cell.SrcMeshletBegin = Begin;
                    Cell.MeshletCount    = End - Begin;

                    for (uint32 i = Begin; i < End; ++i)
                    {
                        Cell.VertexCount   += Data.Meshlets[i].VertexCount;
                        Cell.TriangleCount += Data.Meshlets[i].TriangleCount;
                        Cell.PaletteBones  += (i < static_cast<uint32>(Data.MeshletBonePalettes.Num()))
                            ? Data.MeshletBonePalettes[i].Count : 0u;
                    }
                }
            }
        }

        if (Cells.IsEmpty())
        {
            Result.Error = "Nothing to merge: no input surface had meshlets at any LOD.";
            return Result;
        }

        uint32 TotalMeshlets  = 0;
        uint32 TotalVertices  = 0;
        uint32 TotalTriangles = 0;
        uint32 TotalPalette   = 0;
        for (FCell& Cell : Cells)
        {
            Cell.DstMeshletBase  = TotalMeshlets;
            Cell.DstVertexBase   = TotalVertices;
            Cell.DstTriangleBase = TotalTriangles;
            Cell.DstPaletteBase  = TotalPalette;

            TotalMeshlets  += Cell.MeshletCount;
            TotalVertices  += Cell.VertexCount;
            TotalTriangles += Cell.TriangleCount;
            TotalPalette   += Cell.PaletteBones;
        }

        TUniquePtr<FMeshResource> Merged = MakeUnique<FMeshResource>();
        Merged->Name         = Settings.Name.IsNone() ? Sources[0]->GetMeshResource().Name : Settings.Name;
        Merged->bSkinnedMesh = true;

        FMeshletData& Dst = Merged->MeshletData;
        Dst.Meshlets.SetNum(TotalMeshlets);
        Dst.MeshletSpheres.SetNum(TotalMeshlets);
        Dst.MeshletCones.SetNum(TotalMeshlets);
        Dst.MeshletBonePalettes.SetNum(TotalMeshlets);
        Dst.MeshletSkinnedVertices.SetNum(TotalVertices);
        Dst.MeshletTriangles.SetNum(TotalTriangles);
        Dst.MeshletBoneIndices.SetNum(TotalPalette);

        // Spans every source grid, so each source's vertices re-encode onto it without clamping.
        {
            const FMeshVertexPosition GridLow  = { 0u, 0u };
            const FMeshVertexPosition GridHigh = { ~0u, ~0u };
            TVector<FVector3> GridCorners;
            for (const CSkeletalMesh* Source : Sources)
            {
                const FMeshPositionGrid& Grid = Source->GetMeshResource().MeshletData.PositionGrid;
                GridCorners.Add(DecodeMeshPosition(Grid, GridLow));
                GridCorners.Add(DecodeMeshPosition(Grid, GridHigh));
            }
            Dst.PositionGrid = ComputeMeshPositionGrid(GridCorners.GetData(), GridCorners.Num());
        }

        //~ Every cell owns a disjoint destination range, so the copy needs no synchronization at all.

        Jobs::ParallelFor((uint32)Cells.Num(), [&](uint32 CellIndex)
        {
            const FCell&          Cell     = Cells[CellIndex];
            const FMeshResource&  Resource = Sources[Cell.MeshIndex]->GetMeshResource();
            const FMeshletData&   Src      = Resource.MeshletData;
            const TVector<int32>& Remap    = BoneRemap[Cell.MeshIndex];

            uint32 VertexCursor   = Cell.DstVertexBase;
            uint32 TriangleCursor = Cell.DstTriangleBase;
            uint32 PaletteCursor  = Cell.DstPaletteBase;

            for (uint32 i = 0; i < Cell.MeshletCount; ++i)
            {
                const uint32    SrcIndex = Cell.SrcMeshletBegin + i;
                const FMeshlet& SrcM     = Src.Meshlets[SrcIndex];
                const uint32    DstIndex = Cell.DstMeshletBase + i;

                FMeshlet& DstM      = Dst.Meshlets[DstIndex];
                DstM                = SrcM;
                DstM.VertexOffset   = VertexCursor;
                DstM.TriangleOffset = TriangleCursor;

                Dst.MeshletSpheres[DstIndex] = Src.MeshletSpheres[SrcIndex];
                Dst.MeshletCones[DstIndex]   = Src.MeshletCones[SrcIndex];

                // JointIndices address the meshlet's own palette, so only the position changes, onto the merged grid.
                const uint32 VertexEnd = Math::Min(SrcM.VertexOffset + SrcM.VertexCount,
                                                   (uint32)Src.MeshletSkinnedVertices.Num());
                for (uint32 v = SrcM.VertexOffset; v < VertexEnd; ++v)
                {
                    FMeshletSkinnedVertex Vertex = Src.MeshletSkinnedVertices[v];
                    Vertex.Position = EncodeMeshPosition(Dst.PositionGrid, DecodeMeshPosition(Src.PositionGrid, Vertex.Position));
                    Dst.MeshletSkinnedVertices[VertexCursor + (v - SrcM.VertexOffset)] = Vertex;
                }

                const uint32 TriangleEnd = Math::Min(SrcM.TriangleOffset + SrcM.TriangleCount,
                                                     (uint32)Src.MeshletTriangles.Num());
                for (uint32 t = SrcM.TriangleOffset; t < TriangleEnd; ++t)
                {
                    Dst.MeshletTriangles[TriangleCursor + (t - SrcM.TriangleOffset)] = Src.MeshletTriangles[t];
                }

                FMeshletBonePalette& DstPalette = Dst.MeshletBonePalettes[DstIndex];
                DstPalette.Offset = PaletteCursor;
                DstPalette.Count  = (SrcIndex < static_cast<uint32>(Src.MeshletBonePalettes.Num()))
                    ? Src.MeshletBonePalettes[SrcIndex].Count : 0u;

                if (DstPalette.Count != 0u)
                {
                    const uint32 SrcPaletteBase = Src.MeshletBonePalettes[SrcIndex].Offset;
                    for (uint32 b = 0; b < DstPalette.Count; ++b)
                    {
                        const uint32 SrcSlot = SrcPaletteBase + b;
                        const uint32 SrcBone = (SrcSlot < static_cast<uint32>(Src.MeshletBoneIndices.Num()))
                            ? Src.MeshletBoneIndices[SrcSlot] : 0u;

                        Dst.MeshletBoneIndices[PaletteCursor + b] = (SrcBone < static_cast<uint32>(Remap.Num()))
                            ? (uint32)Remap[SrcBone] : 0u;
                    }
                }

                VertexCursor   += SrcM.VertexCount;
                TriangleCursor += SrcM.TriangleCount;
                PaletteCursor  += DstPalette.Count;
            }
        });

        //~ Surfaces. One per (mesh, surface), carrying the LOD table the cells just laid out.

        struct FSurfaceKey { int32 MeshIndex; int32 SurfaceIndex; };
        TVector<FSurfaceKey> SurfaceKeys;

        for (int32 m = 0; m < Sources.Num(); ++m)
        {
            const FMeshResource& Resource = Sources[m]->GetMeshResource();
            for (int32 s = 0; s < Resource.GeometrySurfaces.Num(); ++s)
            {
                const FGeometrySurface& Source = Resource.GeometrySurfaces[s];

                FGeometrySurface& Out = Merged->GeometrySurfaces.EmplaceGetRef();
                Out.ID            = Source.ID;
                Out.IndexCount    = Source.IndexCount;
                Out.StartIndex    = Source.StartIndex;
                Out.TexelFactor   = Source.TexelFactor;
                Out.NumLODs       = 0;
                Out.MaterialIndex = (Source.MaterialIndex >= 0 && Source.MaterialIndex < MaterialRemap[m].Num())
                    ? MaterialRemap[m][Source.MaterialIndex] : (int16)-1;

                for (uint32 LOD = 0; LOD < MAX_MESH_LODS; ++LOD)
                {
                    Out.LODMeshletOffset[LOD] = 0;
                    Out.LODMeshletCount[LOD]  = 0;
                    Out.LODError[LOD]         = Source.LODError[LOD];
                }

                SurfaceKeys.Emplace(m, s);
            }
        }

        for (const FCell& Cell : Cells)
        {
            for (int32 k = 0; k < SurfaceKeys.Num(); ++k)
            {
                if (SurfaceKeys[k].MeshIndex != Cell.MeshIndex || SurfaceKeys[k].SurfaceIndex != Cell.SurfaceIndex)
                {
                    continue;
                }

                FGeometrySurface& Out = Merged->GeometrySurfaces[k];
                Out.LODMeshletOffset[Cell.LODSlot] = Cell.DstMeshletBase;
                Out.LODMeshletCount[Cell.LODSlot]  = Cell.MeshletCount;
                Out.NumLODs = Math::Max(Out.NumLODs, Cell.LODSlot + 1u);
                break;
            }
        }

        // LOD selection clamps to NumLODs, so a surface that contributed nothing must still read as one LOD.
        for (FGeometrySurface& Surface : Merged->GeometrySurfaces)
        {
            Surface.NumLODs = Math::Max(Surface.NumLODs, 1u);
        }

        uint32 RequiredBones = 0;
        for (uint32 Bone : Dst.MeshletBoneIndices)
        {
            RequiredBones = Math::Max(RequiredBones, Bone + 1u);
        }
        Merged->RequiredBoneCount = RequiredBones;

        //~ Publish.

        const FName MeshName = Settings.Name.IsNone() ? FName("MergedSkeletalMesh") : Settings.Name;

        CSkeleton* OutSkeleton = NewObject<CSkeleton>(nullptr, FName(MeshName.ToString() + "_Skeleton"),
                                                     FGuid::New(), OF_Transient);
        OutSkeleton->SetSkeletonResource(Move(MergedSkeleton));

        if (Settings.bMergeSockets)
        {
            for (const CSkeleton* Skeleton : SourceSkeletons)
            {
                if (Skeleton == nullptr)
                {
                    continue;
                }
                for (const SMeshSocket& Socket : Skeleton->Sockets)
                {
                    if (FindSocketByName(OutSkeleton->Sockets, Socket.SocketName) == nullptr)
                    {
                        OutSkeleton->Sockets.Add(Socket);
                    }
                }
            }
        }

        CSkeletalMesh* OutMesh = NewObject<CSkeletalMesh>(nullptr, MeshName, FGuid::New(), OF_Transient);
        OutMesh->Skeleton  = OutSkeleton;

        // Mesh sockets stay on the mesh, so they keep overriding the skeleton's after the merge.
        if (Settings.bMergeSockets)
        {
            for (const CSkeletalMesh* Source : Sources)
            {
                for (const SMeshSocket& Socket : Source->Sockets)
                {
                    if (FindSocketByName(OutMesh->Sockets, Socket.SocketName) == nullptr)
                    {
                        OutMesh->Sockets.Add(Socket);
                    }
                }
            }
        }
        OutMesh->Materials = MergedMaterials;
        OutMesh->MergeSources.Reserve(Sources.Num());
        for (CSkeletalMesh* Source : Sources)
        {
            OutMesh->MergeSources.Emplace(Source);
        }

        // Last, because it uploads the geometry and derives the bounding box from what it was given.
        OutMesh->SetMeshResource(Move(Merged));

        Result.Mesh     = OutMesh;
        Result.Skeleton = OutSkeleton;
        return Result;
    }

    namespace
    {
        struct FMergeCacheEntry
        {
            TVector<uint64>               Key;
            TWeakObjectPtr<CSkeletalMesh> Mesh;
            TWeakObjectPtr<CSkeleton>     Skeleton;
        };

        // Weak, so a merged mesh still dies with the last character wearing it.
        struct FMergeCache
        {
            FMutex                                Mutex;
            THashMap<uint64, FMergeCacheEntry>    Entries;
            int32                                 PruneAt = 64;
        };

        FMergeCache& GetMergeCache()
        {
            static FMergeCache Cache;
            return Cache;
        }

        uint64 PackHandle(const CObjectBase* Object)
        {
            const FObjectHandle Handle(Object);
            return ((uint64)(uint32)Handle.Index << 32) | (uint32)Handle.Generation;
        }

        // Resource addresses catch a reimport, which swaps the geometry under the same object.
        void BuildMergeKey(TSpan<CSkeletalMesh* const> Meshes, const FSettings& Settings, TVector<uint64>& OutKey)
        {
            OutKey.Add(Settings.BaseSkeleton != nullptr ? PackHandle(Settings.BaseSkeleton) : 0ull);
            OutKey.Add(GetTypeHash(Settings.Name));
            OutKey.Add(Settings.bMergeSockets ? 1ull : 0ull);

            for (CSkeletalMesh* Mesh : Meshes)
            {
                if (!HasMergeableGeometry(Mesh))
                {
                    continue;
                }
                const CSkeleton* Skeleton = SkeletonOf(Mesh);
                OutKey.Add(PackHandle(Mesh));
                OutKey.Add((uint64)(uintptr_t)&Mesh->GetMeshResource());
                OutKey.Add(PackHandle(Skeleton));
                OutKey.Add((uint64)(uintptr_t)Skeleton->GetSkeletonResource());
                for (const TStrongObjectPtr<CMaterialInterface>& Material : Mesh->Materials)
                {
                    OutKey.Add((uint64)(uintptr_t)Material.Get());
                }
            }
        }

        uint64 HashMergeKey(const TVector<uint64>& Key)
        {
            uint64 Hash = 0xcbf29ce484222325ull;
            for (uint64 Word : Key)
            {
                Hash = (Hash ^ Word) * 0x100000001b3ull;
            }
            return Hash;
        }
    }

    FResult Merge(TSpan<CSkeletalMesh* const> Meshes, const FSettings& Settings)
    {
        TVector<uint64> Key;
        BuildMergeKey(Meshes, Settings, Key);
        const uint64 Hash = HashMergeKey(Key);

        FMergeCache& Cache = GetMergeCache();
        {
            FScopeLock Lock(Cache.Mutex);
            auto It = Cache.Entries.Find(Hash);
            if (It != nullptr && It->Key == Key)
            {
                FResult Shared;
                Shared.Mesh     = It->Mesh.Lock();
                Shared.Skeleton = It->Skeleton.Lock();
                if (Shared.IsValid() && Shared.Skeleton.Get() != nullptr)
                {
                    return Shared;
                }
            }
        }

        FResult Result = MergeUncached(Meshes, Settings);
        if (!Result.IsValid())
        {
            return Result;
        }

        FScopeLock Lock(Cache.Mutex);
        FMergeCacheEntry& Entry = Cache.Entries[Hash];
        Entry.Key      = Move(Key);
        Entry.Mesh     = Result.Mesh;
        Entry.Skeleton = Result.Skeleton;

        if (Cache.Entries.Num() >= Cache.PruneAt)
        {
            Cache.Entries.RemoveAll([](const auto& Pair) { return !Pair.Value.Mesh.IsValid(); });
            Cache.PruneAt = Math::Max(64, Cache.Entries.Num() * 2);
        }
        return Result;
    }
}
