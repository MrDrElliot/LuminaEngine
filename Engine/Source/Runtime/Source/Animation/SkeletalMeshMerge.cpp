#include "RuntimePCH.h"
#include "SkeletalMeshMerge.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina::SkeletalMeshMerge
{
    namespace
    {
        // The smallest run that must stay contiguous, because a surface's per-LOD range is one span.
        struct FCell
        {
            uint32 MeshIndex       = 0;
            uint32 SurfaceIndex    = 0;
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

        Out.Bones.clear();
        Out.BoneNameToIndex.clear();
        OutBoneRemap.clear();
        OutBoneRemap.resize(Skeletons.size());

        // The base goes in whole and first, so its bone ORDER and bind pose survive the merge.
        TVector<CSkeleton*> Ordered;
        Ordered.reserve(Skeletons.size() + 1);
        if (Base != nullptr && Base->GetSkeletonResource() != nullptr)
        {
            Ordered.push_back(Base);
        }
        for (CSkeleton* Skeleton : Skeletons)
        {
            if (Skeleton != nullptr && Skeleton->GetSkeletonResource() != nullptr && Skeleton != Base)
            {
                Ordered.push_back(Skeleton);
            }
        }

        if (Ordered.empty())
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
                if (Out.BoneNameToIndex.find(Bone.Name) != Out.BoneNameToIndex.end())
                {
                    continue;   // first definition wins, so the base's bind pose is authoritative
                }

                // Sources are parents-first, so the parent is already in Out unless it roots elsewhere.
                int32 ParentIndex = INDEX_NONE;
                if (Bone.ParentIndex != INDEX_NONE)
                {
                    const auto It = Out.BoneNameToIndex.find(Source.GetBone(Bone.ParentIndex).Name);
                    if (It != Out.BoneNameToIndex.end())
                    {
                        ParentIndex = It->second;
                    }
                }

                const int32 NewIndex = (int32)Out.Bones.size();
                FSkeletonResource::FBoneInfo& Added = Out.Bones.emplace_back(Bone);
                Added.ParentIndex = ParentIndex;
                Out.BoneNameToIndex.emplace(Bone.Name, NewIndex);
            }
        }

        if (Out.Bones.empty())
        {
            OutError = "No skeleton to merge: every input skeleton was empty.";
            return false;
        }

        Out.Name = Ordered[0]->GetSkeletonResource()->Name;
        Out.BuildBindPoseCache();

        for (SIZE_T s = 0; s < Skeletons.size(); ++s)
        {
            const CSkeleton* Skeleton = Skeletons[s];
            if (Skeleton == nullptr || Skeleton->GetSkeletonResource() == nullptr)
            {
                continue;
            }

            const FSkeletonResource& Source = *Skeleton->GetSkeletonResource();
            TVector<int32>& Remap = OutBoneRemap[s];
            Remap.resize(Source.GetNumBones(), 0);

            for (int32 i = 0; i < Source.GetNumBones(); ++i)
            {
                const auto It = Out.BoneNameToIndex.find(Source.GetBone(i).Name);
                Remap[i] = (It != Out.BoneNameToIndex.end()) ? It->second : 0;
            }
        }

        return true;
    }

    FResult Merge(TSpan<CSkeletalMesh* const> Meshes, const FSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();

        FResult Result;

        TVector<CSkeletalMesh*> Sources;
        Sources.reserve(Meshes.size());
        for (CSkeletalMesh* Mesh : Meshes)
        {
            if (HasMergeableGeometry(Mesh))
            {
                Sources.push_back(Mesh);
            }
        }

        if (Sources.empty())
        {
            Result.Error = "Nothing to merge: no input was a skinned mesh with baked meshlets and a skeleton.";
            return Result;
        }

        //~ Unified skeleton, and the per-source bone remap the palettes are rewritten through.

        TVector<CSkeleton*> SourceSkeletons;
        SourceSkeletons.reserve(Sources.size());
        for (const CSkeletalMesh* Mesh : Sources)
        {
            SourceSkeletons.push_back(SkeletonOf(Mesh));
        }

        TUniquePtr<FSkeletonResource> MergedSkeleton = MakeUnique<FSkeletonResource>();
        TVector<TVector<int32>>       BoneRemap;
        if (!BuildUnifiedSkeleton(TSpan<CSkeleton* const>(SourceSkeletons.data(), SourceSkeletons.size()),
                                  Settings.BaseSkeleton, *MergedSkeleton, BoneRemap, Result.Error))
        {
            return Result;
        }

        //~ Material slots, deduplicated so two pieces sharing a material share one slot.

        TVector<TObjectPtr<CMaterialInterface>> MergedMaterials;
        TVector<TVector<int16>>                 MaterialRemap(Sources.size());

        for (SIZE_T m = 0; m < Sources.size(); ++m)
        {
            const TVector<TObjectPtr<CMaterialInterface>>& Slots = Sources[m]->Materials;
            MaterialRemap[m].resize(Slots.size(), -1);

            for (SIZE_T s = 0; s < Slots.size(); ++s)
            {
                int32 Found = INDEX_NONE;
                for (SIZE_T e = 0; e < MergedMaterials.size(); ++e)
                {
                    if (MergedMaterials[e].Get() == Slots[s].Get())
                    {
                        Found = (int32)e;
                        break;
                    }
                }

                if (Found == INDEX_NONE)
                {
                    Found = (int32)MergedMaterials.size();
                    MergedMaterials.push_back(Slots[s]);
                }
                MaterialRemap[m][s] = (int16)Found;
            }
        }

        //~ Layout, LOD-MAJOR as the importer bakes it: a pre-skin slice spans one LOD's meshlet range.

        TVector<FCell> Cells;
        for (uint32 LOD = 0; LOD < MAX_MESH_LODS; ++LOD)
        {
            for (uint32 m = 0; m < (uint32)Sources.size(); ++m)
            {
                const FMeshResource& Resource = Sources[m]->GetMeshResource();
                const FMeshletData&  Data     = Resource.MeshletData;

                for (uint32 s = 0; s < (uint32)Resource.GeometrySurfaces.size(); ++s)
                {
                    const FGeometrySurface& Surface = Resource.GeometrySurfaces[s];
                    if (LOD >= Surface.NumLODs || Surface.LODMeshletCount[LOD] == 0)
                    {
                        continue;
                    }

                    const uint32 Begin = Surface.LODMeshletOffset[LOD];
                    const uint32 End   = Math::Min(Begin + Surface.LODMeshletCount[LOD], (uint32)Data.Meshlets.size());
                    if (Begin >= End)
                    {
                        continue;
                    }

                    FCell& Cell = Cells.emplace_back();
                    Cell.MeshIndex       = m;
                    Cell.SurfaceIndex    = s;
                    Cell.LODSlot         = LOD;
                    Cell.SrcMeshletBegin = Begin;
                    Cell.MeshletCount    = End - Begin;

                    for (uint32 i = Begin; i < End; ++i)
                    {
                        Cell.VertexCount   += Data.Meshlets[i].VertexCount;
                        Cell.TriangleCount += Data.Meshlets[i].TriangleCount;
                        Cell.PaletteBones  += (i < Data.MeshletBonePalettes.size())
                            ? Data.MeshletBonePalettes[i].Count : 0u;
                    }
                }
            }
        }

        if (Cells.empty())
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
        Dst.Meshlets.resize(TotalMeshlets);
        Dst.MeshletSpheres.resize(TotalMeshlets);
        Dst.MeshletCones.resize(TotalMeshlets);
        Dst.MeshletBonePalettes.resize(TotalMeshlets);
        Dst.MeshletSkinnedVertices.resize(TotalVertices);
        Dst.MeshletTriangles.resize(TotalTriangles);
        Dst.MeshletBoneIndices.resize(TotalPalette);

        //~ Every cell owns a disjoint destination range, so the copy needs no synchronization at all.

        Task::ParallelFor((uint32)Cells.size(), [&](uint32 CellIndex)
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

                // Vertices copy VERBATIM: JointIndices address the meshlet's own palette, not the skeleton.
                const uint32 VertexEnd = Math::Min(SrcM.VertexOffset + SrcM.VertexCount,
                                                   (uint32)Src.MeshletSkinnedVertices.size());
                for (uint32 v = SrcM.VertexOffset; v < VertexEnd; ++v)
                {
                    Dst.MeshletSkinnedVertices[VertexCursor + (v - SrcM.VertexOffset)] = Src.MeshletSkinnedVertices[v];
                }

                const uint32 TriangleEnd = Math::Min(SrcM.TriangleOffset + SrcM.TriangleCount,
                                                     (uint32)Src.MeshletTriangles.size());
                for (uint32 t = SrcM.TriangleOffset; t < TriangleEnd; ++t)
                {
                    Dst.MeshletTriangles[TriangleCursor + (t - SrcM.TriangleOffset)] = Src.MeshletTriangles[t];
                }

                FMeshletBonePalette& DstPalette = Dst.MeshletBonePalettes[DstIndex];
                DstPalette.Offset = PaletteCursor;
                DstPalette.Count  = (SrcIndex < Src.MeshletBonePalettes.size())
                    ? Src.MeshletBonePalettes[SrcIndex].Count : 0u;

                if (DstPalette.Count != 0u)
                {
                    const uint32 SrcPaletteBase = Src.MeshletBonePalettes[SrcIndex].Offset;
                    for (uint32 b = 0; b < DstPalette.Count; ++b)
                    {
                        const uint32 SrcSlot = SrcPaletteBase + b;
                        const uint32 SrcBone = (SrcSlot < Src.MeshletBoneIndices.size())
                            ? Src.MeshletBoneIndices[SrcSlot] : 0u;

                        Dst.MeshletBoneIndices[PaletteCursor + b] = (SrcBone < Remap.size())
                            ? (uint32)Remap[SrcBone] : 0u;
                    }
                }

                VertexCursor   += SrcM.VertexCount;
                TriangleCursor += SrcM.TriangleCount;
                PaletteCursor  += DstPalette.Count;
            }
        });

        //~ Surfaces. One per (mesh, surface), carrying the LOD table the cells just laid out.

        struct FSurfaceKey { uint32 MeshIndex; uint32 SurfaceIndex; };
        TVector<FSurfaceKey> SurfaceKeys;

        for (uint32 m = 0; m < (uint32)Sources.size(); ++m)
        {
            const FMeshResource& Resource = Sources[m]->GetMeshResource();
            for (uint32 s = 0; s < (uint32)Resource.GeometrySurfaces.size(); ++s)
            {
                const FGeometrySurface& Source = Resource.GeometrySurfaces[s];

                FGeometrySurface& Out = Merged->GeometrySurfaces.emplace_back();
                Out.ID            = Source.ID;
                Out.IndexCount    = Source.IndexCount;
                Out.StartIndex    = Source.StartIndex;
                Out.TexelFactor   = Source.TexelFactor;
                Out.NumLODs       = 0;
                Out.MaterialIndex = (Source.MaterialIndex >= 0 && (SIZE_T)Source.MaterialIndex < MaterialRemap[m].size())
                    ? MaterialRemap[m][Source.MaterialIndex] : (int16)-1;

                for (uint32 LOD = 0; LOD < MAX_MESH_LODS; ++LOD)
                {
                    Out.LODMeshletOffset[LOD]   = 0;
                    Out.LODMeshletCount[LOD]    = 0;
                    Out.LODScreenThreshold[LOD] = Source.LODScreenThreshold[LOD];
                }

                SurfaceKeys.push_back(FSurfaceKey{ m, s });
            }
        }

        for (const FCell& Cell : Cells)
        {
            for (SIZE_T k = 0; k < SurfaceKeys.size(); ++k)
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
                for (const FMeshSocket& Socket : Skeleton->Sockets)
                {
                    if (FindSocketByName(OutSkeleton->Sockets, Socket.SocketName) == nullptr)
                    {
                        OutSkeleton->Sockets.push_back(Socket);
                    }
                }
            }
        }

        CSkeletalMesh* OutMesh = NewObject<CSkeletalMesh>(nullptr, MeshName, FGuid::New(), OF_Transient);
        OutMesh->Skeleton  = OutSkeleton;
        OutMesh->Materials = MergedMaterials;

        // Last, because it uploads the geometry and derives the bounding box from what it was given.
        OutMesh->SetMeshResource(Move(Merged));

        if (Settings.bAddToRoot)
        {
            OutSkeleton->AddToRoot();
            OutMesh->AddToRoot();
        }

        Result.Mesh     = OutMesh;
        Result.Skeleton = OutSkeleton;
        return Result;
    }
}
