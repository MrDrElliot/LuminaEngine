#include "EditorPCH.h"
#include "MeshImporter.h"

#include "Tools/Import/ImportPaths.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/Factories/Factory.h"
#include "GUID/GUID.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/PostProcessComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Core/Object/Package/Package.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Threading/Thread.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Renderer/RendererUtils.h"
#include "Renderer/SkeletonResource.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Assets/AssetEvents.h"
#include "Tools/Import/MaterialImport.h"
#include "Tools/Import/TextureImporter.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Log/Log.h"
#include "Containers/StringFormat.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"

namespace Lumina
{
    using namespace Import::Mesh;

    namespace
    {
        // Directory and extension are stripped, leaving the prefix and a clean stem.
        FFixedString TextureAssetName(FStringView Raw)
        {
            const int32 Slash = Raw.FindLastOf("/\\");
            FStringView Stem = (Slash == Constants::kIndexNone) ? Raw : Raw.Mid(Slash + 1);
            const int32 Dot = Stem.FindLastOf('.');
            if (Dot != Constants::kIndexNone && Dot > 0)
            {
                Stem = Stem.Mid(0, Dot);
            }
            return FFixedString(*Import::MakeAssetName("T_", Stem));
        }

        // Blender custom properties arrive as node extras naming collision, render, nav, tag, or a component.
        void ApplyNodeExtras(ECS::FRegistry& Registry, ECS::FEntity Entity, const FSourceSceneNode& Node,
                             CStaticMesh* Mesh, ESceneCollision DefaultCollision)
        {
            FJsonValue Extras = FJsonValue::MakeObject();
            if (!Node.Extras.IsEmpty())
            {
                if (!FJsonValue::Parse(Node.Extras.View(), Extras) || !Extras.IsObject())
                {
                    LOG_WARN("[Import] node '{}' has extras that are not a JSON object", Node.Name);
                    Extras = FJsonValue::MakeObject();
                }
            }

            const FStringView Name(*Node.Name);
            ESceneCollision Collision = Mesh != nullptr ? DefaultCollision : ESceneCollision::None;
            bool bRender = true;
            bool bTrigger = false;
            bool bNav = true;
            bool bBodyFromExtras = false;
            if (Name.StartsWith("UCX_"))
            {
                Collision = ESceneCollision::ConvexHull;
                bRender = false;
            }
            else if (Name.StartsWith("COL_"))
            {
                Collision = ESceneCollision::TriangleMesh;
                bRender = false;
            }

            auto IsTrue = [](const FJsonValue& Value)
            {
                return Value.IsBool() ? Value.AsBool() : Value.IsNumber() && Value.AsDouble() != 0.0;
            };

            for (const auto& [Key, Value] : Extras.Members())
            {
                if (Key == "collision" && Value.IsString())
                {
                    const FStringView Kind = Value.AsString();
                    bTrigger = Kind == "trigger";
                    Collision = Kind == "mesh" ? ESceneCollision::TriangleMesh
                              : (Kind == "convex" || bTrigger) ? ESceneCollision::ConvexHull
                              : ESceneCollision::None;
                }
                else if (Key == "render")
                {
                    bRender = IsTrue(Value);
                }
                else if (Key == "nav")
                {
                    bNav = IsTrue(Value);
                }
                else if (Key == "tag" && Value.IsString())
                {
                    ECS::Utils::SetEntityTag(Registry, Entity, FName(Value.AsCString()));
                }
                else if (CStruct* Component = FindComponentStruct(Key.View()))
                {
                    // A Blender custom property holds a nested object only when typed in Python, so a JSON string works too.
                    FJsonValue Fields = Value;
                    if (Value.IsString() && !FJsonValue::Parse(Value.AsString(), Fields))
                    {
                        Fields.Reset();
                    }
                    if (!Fields.IsObject())
                    {
                        LOG_WARN("[Import] node '{}' extra '{}' is not a JSON object", Node.Name, Key.View());
                        continue;
                    }
                    if (void* Data = Component->GetComponentOps()->Emplace(Registry, Entity))
                    {
                        FJsonStructuredArchive::LoadStruct(Fields, Component, Data);
                    }
                    bBodyFromExtras |= Component == SRigidBodyComponent::StaticStruct();
                }
                else
                {
                    LOG_WARN("[Import] node '{}' extra '{}' names no component or import key", Node.Name, Key.View());
                }
            }

            if (Mesh == nullptr)
            {
                return;
            }
            if (!bRender)
            {
                Registry.Remove<SStaticMeshComponent>(Entity);
            }
            if (Collision == ESceneCollision::None)
            {
                return;
            }

            SRigidBodyComponent& Body = Registry.GetOrEmplace<SRigidBodyComponent>(Entity);
            if (!bBodyFromExtras)
            {
                Body.BodyType = EBodyType::Static;
            }

            SMeshColliderComponent& Collider = Registry.GetOrEmplace<SMeshColliderComponent>(Entity);
            Collider.Mesh = Mesh;
            // Triangle meshes cannot simulate, so a body the extras made dynamic gets a hull instead.
            Collider.bConvex = Collision == ESceneCollision::ConvexHull || Body.BodyType == EBodyType::Dynamic;
            Collider.bIsTrigger = bTrigger;
            Collider.bAffectsNavigation = bNav && !bTrigger;
        }

        // Importing over an existing asset replaces it in place, so its GUID and every reference to it survive.
        CAnimation* FindAnimationToOverwrite(const FFixedString& Path)
        {
            const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path);
            return Data != nullptr ? Cast<CAnimation>(LoadObject<CObject>(Data->AssetGUID)) : nullptr;
        }

    }

    // On the main thread, since an animation system may be sampling the old clip mid-frame.
    void CMeshImporter::ReplaceAnimationInPlace(CAnimation* Existing, TUniquePtr<FAnimationResource>&& NewClip,
                                                CSkeleton* NewSkeleton)
    {
        MainThread::Enqueue([Existing = TStrongObjectPtr<CAnimation>(Existing), NewClip = Move(NewClip),
                             NewSkeleton = TStrongObjectPtr<CSkeleton>(NewSkeleton)]() mutable
        {
            if (!Existing.IsValid())
            {
                return;
            }

            // Notifies, curves and the sync lane are authored in the editor, and no source file carries them.
            if (const FAnimationResource* OldClip = Existing->GetAnimationResource())
            {
                NewClip->Notifies      = OldClip->Notifies;
                NewClip->NotifyStates  = OldClip->NotifyStates;
                NewClip->NotifyTracks  = OldClip->NotifyTracks;
                NewClip->SyncTrackName = OldClip->SyncTrackName;
                if (NewClip->Curves.IsEmpty())
                {
                    NewClip->Curves = OldClip->Curves;
                }
            }

            Existing->AnimationResource = Move(NewClip);
            Existing->AnimationResource->RebuildSyncTrack();
            if (NewSkeleton != nullptr)
            {
                Existing->Skeleton = NewSkeleton;
            }

            CPackage* Package = Existing->GetPackage();
            if (Package == nullptr || !CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                LOG_ERROR("[Import] Overwrote '{}' in memory but could not save it.", Existing->GetName());
            }
            else
            {
                FAssetRegistry::Get().AssetSaved(Existing.Get());
            }

            // An open animation editor holds pointers into the clip that was just replaced.
            AssetEvents::BroadcastAssetDataChanged(Existing.Get());
        });
    }

    namespace
    {
        // FindObject only sees loaded objects, so a multi-asset run needs its own claim set.
        class FUniquePathAllocator
        {
        public:

            // The import already reserved its destination, so that one name is this run's to use.
            explicit FUniquePathAllocator(FStringView OwnedDestination)
                : Owned(OwnedDestination.GetData(), OwnedDestination.Len())
            {
            }

            FFixedString Claim(const FFixedString& Desired)
            {
                if (IsFree(Desired))
                {
                    Claimed.Add(Desired);
                    return Desired;
                }

                for (uint32 N = 1; N < 10000; ++N)
                {
                    FFixedString Candidate = Desired;
                    Candidate.Append("_").Append(Format("{}", N));
                    if (IsFree(Candidate))
                    {
                        Claimed.Add(Candidate);
                        return Candidate;
                    }
                }
                return Desired;
            }

        private:

            // A file nothing has loaded and another import's reservation hold a name too, or this run would write over them.
            bool IsFree(const FFixedString& Path) const
            {
                if (Claimed.Contains(Path))
                {
                    return false;
                }
                return Path == Owned ? FindObject<CPackage>(Path) == nullptr : Import::PathReservations::IsFree(Path);
            }

            FFixedString           Owned;
            THashSet<FFixedString> Claimed;
        };

        void AppendCreateFailure(FImportResult& OutResult, FStringView Path)
        {
            OutResult.Error += (OutResult.Error.IsEmpty() ? "Could not create " : ", ") + FString(Path.GetData(), Path.Len());
        }

        size_t CountMaterialSlots(const FMeshResource& Resource)
        {
            size_t SlotCount = 0;
            bool bAnyExplicit = false;
            for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                if (Surface.MaterialIndex >= 0)
                {
                    bAnyExplicit = true;
                    SlotCount = Math::Max(SlotCount, (size_t)Surface.MaterialIndex + 1);
                }
            }
            return bAnyExplicit ? SlotCount : Resource.GeometrySurfaces.Num();
        }

        // A parser indexes by position in the WHOLE file, which leaves dangling slots behind.
        TVector<int16> DensifyMaterialSlots(FMeshResource& Resource)
        {
            TVector<int16> SlotToSource;
            THashMap<int16, int16> SourceToSlot;

            for (FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                if (Surface.MaterialIndex < 0)
                {
                    continue;
                }

                const int16 Source = Surface.MaterialIndex;
                const auto It = SourceToSlot.Find(Source);
                if (It != nullptr)
                {
                    Surface.MaterialIndex = (*It);
                    continue;
                }

                const int16 NewSlot = (int16)SlotToSource.Num();
                SourceToSlot.TryAdd(Source, NewSlot);
                SlotToSource.Add(Source);
                Surface.MaterialIndex = NewSlot;
            }

            // A source with no materials still gets one slot per surface, pointed at, so assigning a material later takes effect.
            if (SlotToSource.IsEmpty())
            {
                for (FGeometrySurface& Surface : Resource.GeometrySurfaces)
                {
                    Surface.MaterialIndex = (int16)SlotToSource.Num();
                    SlotToSource.Add(-1);
                }
            }

            return SlotToSource;
        }

        struct FBoneRemapResult
        {
            bool   bApplied           = false;
            uint32 MatchedBones       = 0;
            uint32 UnmatchedBones     = 0;
            uint32 DroppedInfluences  = 0;
            uint32 ClampedInfluences  = 0;
            FName  FirstUnmatched;
        };

        // A file numbers only the bones its own clusters bind, so modular pieces disagree on order and count.
        FBoneRemapResult RemapJointIndicesToSkeleton(FMeshImportData& Data, const FSkeletonResource& Target)
        {
            FBoneRemapResult Result;

            if (Data.Skeletons.IsEmpty() || !Data.Skeletons[0] || Data.Skeletons[0]->Bones.IsEmpty())
            {
                return Result;
            }

            const FSkeletonResource& Source = *Data.Skeletons[0];

            TVector<int32> SourceToTarget(Source.Bones.Num(), Constants::kIndexNone);
            for (int32 i = 0; i < Source.Bones.Num(); ++i)
            {
                const int32 TargetIndex = Target.FindBoneIndex(Source.Bones[i].Name);
                SourceToTarget[i] = TargetIndex;

                if (TargetIndex >= 0)
                {
                    ++Result.MatchedBones;
                }
                else
                {
                    ++Result.UnmatchedBones;
                    if (Result.FirstUnmatched.IsNone())
                    {
                        Result.FirstUnmatched = Source.Bones[i].Name;
                    }
                }
            }

            // A different rig entirely; rewriting would weight every vertex to bone 0.
            if (Result.MatchedBones == 0)
            {
                return Result;
            }

            for (const TUniquePtr<FMeshResource>& ResourcePtr : Data.Resources)
            {
                FMeshResource* Resource = ResourcePtr.get();
                if (Resource == nullptr || !Resource->bSkinnedMesh)
                {
                    continue;
                }

                const int32 VertexCount = Math::Min(Resource->JointIndices.Num(), Resource->JointWeights.Num());
                for (int32 v = 0; v < VertexCount; ++v)
                {
                    FU16Vector4& Indices = Resource->JointIndices[v];
                    FU8Vector4& Weights = Resource->JointWeights[v];

                    uint32 Surviving = 0;
                    for (int32 w = 0; w < 4; ++w)
                    {
                        if (Weights[w] == 0)
                        {
                            Indices[w] = 0;
                            continue;
                        }

                        const uint32 Old = Indices[w];
                        const int32  New = (Old < static_cast<uint32>(SourceToTarget.Num())) ? SourceToTarget[Old] : Constants::kIndexNone;

                        if (New < 0)
                        {
                            Indices[w] = 0;
                            Weights[w] = 0;
                            ++Result.DroppedInfluences;
                            continue;
                        }

                        if (New > kMaxJointIndex)
                        {
                            ++Result.ClampedInfluences;
                        }

                        Indices[w] = (uint16)Math::Min(New, kMaxJointIndex);
                        Surviving += Weights[w];
                    }

                    // Matches PackSkinWeights, where the quartet sums to 255 and an empty one goes rigid to bone 0.
                    if (Surviving == 0)
                    {
                        Indices = FU16Vector4(0, 0, 0, 0);
                        Weights = FU8Vector4(255, 0, 0, 0);
                        continue;
                    }

                    if (Surviving != 255)
                    {
                        int32 Redistributed = 0;
                        int32 Largest       = 0;
                        for (int32 w = 0; w < 4; ++w)
                        {
                            Weights[w] = (uint8)(((uint32)Weights[w] * 255u) / Surviving);
                            Redistributed += Weights[w];
                            Largest = (Weights[w] > Weights[Largest]) ? w : Largest;
                        }
                        Weights[Largest] = (uint8)(Weights[Largest] + (255 - Redistributed));
                    }
                }
            }

            Result.bApplied = true;
            return Result;
        }

        // Prefers a name match so reordered exports survive, and falls back to the first compatible resource.
        int32 FindResourceForAsset(const FMeshImportData& Data, const FName& AssetName, bool bWantSkinned)
        {
            int32 FirstCompatible = Constants::kIndexNone;
            const FString TargetName = Import::SanitizeAssetName(*AssetName);

            for (int32 i = 0; i < Data.Resources.Num(); ++i)
            {
                const TUniquePtr<FMeshResource>& Resource = Data.Resources[i];
                if (!Resource || Resource->bSkinnedMesh != bWantSkinned)
                {
                    continue;
                }
                if (FirstCompatible == Constants::kIndexNone)
                {
                    FirstCompatible = (int32)i;
                }
                if (Import::SanitizeAssetName(*Resource->Name) == TargetName)
                {
                    return (int32)i;
                }
            }
            return FirstCompatible;
        }

        // Keyed off surface names so reordered materials keep their overrides, with index as the fallback.
        void RemapMaterialSlots(const FMeshResource& OldResource,
                                const TVector<TStrongObjectPtr<CMaterialInterface>>& OldMaterials,
                                const FMeshResource& NewResource,
                                TVector<TStrongObjectPtr<CMaterialInterface>>& OutMaterials)
        {
            const size_t SlotCount = CountMaterialSlots(NewResource);

            OutMaterials.Reset();
            OutMaterials.SetNum(IntCastChecked<int32>(SlotCount));

            TVector<bool> bSlotResolved;
            bSlotResolved.SetNum(IntCastChecked<int32>(SlotCount), false);

            for (const FGeometrySurface& NewSurface : NewResource.GeometrySurfaces)
            {
                const int32 NewSlot = NewSurface.MaterialIndex;
                if (NewSlot < 0 || (size_t)NewSlot >= SlotCount || bSlotResolved[NewSlot])
                {
                    continue;
                }

                for (const FGeometrySurface& OldSurface : OldResource.GeometrySurfaces)
                {
                    if (OldSurface.ID != NewSurface.ID)
                    {
                        continue;
                    }
                    if (OldSurface.MaterialIndex >= 0 && (size_t)OldSurface.MaterialIndex < static_cast<size_t>(OldMaterials.Num()))
                    {
                        OutMaterials[NewSlot] = OldMaterials[OldSurface.MaterialIndex];
                        bSlotResolved[NewSlot] = true;
                    }
                    break;
                }
            }

            for (int32 Slot = 0, Limit = IntCastChecked<int32>(SlotCount); Slot < Limit; ++Slot)
            {
                if (!bSlotResolved[Slot] && Slot < OldMaterials.Num())
                {
                    OutMaterials[Slot] = OldMaterials[Slot];
                }
            }
        }

        CMesh* FindMeshToOverwrite(const FFixedString& Path, bool bSkinned)
        {
            const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path);
            CMesh* Mesh = Data != nullptr ? Cast<CMesh>(LoadObject<CObject>(Data->AssetGUID)) : nullptr;
            return Mesh != nullptr && Mesh->IsSkinned() == bSkinned ? Mesh : nullptr;
        }

        // On the main thread, since the renderer may be reading the old buffers mid-frame.
        void ReplaceMeshInPlace(CMesh* Existing, TUniquePtr<FMeshResource>&& NewResource,
                                CMaterialInterface* OverrideMaterial, const FFixedString& SourcePath)
        {
            MainThread::Enqueue([Existing = TStrongObjectPtr<CMesh>(Existing), NewResource = Move(NewResource),
                                 Override = TStrongObjectPtr<CMaterialInterface>(OverrideMaterial),
                                 Source = FString(*SourcePath)]() mutable
            {
                if (!Existing.IsValid())
                {
                    return;
                }

                NewResource->Name = Existing->GetName();
                TVector<TStrongObjectPtr<CMaterialInterface>> Materials;
                if (Override.IsValid())
                {
                    Materials.SetNum(IntCastChecked<int32>(CountMaterialSlots(*NewResource)), Override);
                }
                else
                {
                    RemapMaterialSlots(Existing->GetMeshResource(), Existing->Materials, *NewResource, Materials);
                }
                Existing->Materials = Move(Materials);

                // SetMeshResource rebuilds bounds and buffers and invalidates the resolve cache, so components follow.
                Existing->SetMeshResource(Move(NewResource));
                Existing->SourcePath = Source;

                CPackage* Package = Existing->GetPackage();
                if (Package == nullptr || !CPackage::SavePackage(Package, Package->GetPackagePath()))
                {
                    LOG_ERROR("[Import] Replaced '{}' in memory but could not save it.", Existing->GetName());
                    return;
                }
                FAssetRegistry::Get().AssetSaved(Existing.Get());
                AssetEvents::BroadcastAssetDataChanged(Existing.Get());
            });
        }

        // The only stage that expands instances, so the cost is paid once and only on merge.
        bool MergeInstancesIntoSingleMesh(FMeshImportData& Data, FString& OutError)
        {
            int64 TotalStaticVerts = 0, TotalStaticIndices = 0;
            int64 TotalSkinnedVerts = 0, TotalSkinnedIndices = 0;

            auto ResourceAt = [&Data](int32 Index) -> FMeshResource*
            {
                return Data.Resources.IsValidIndex(Index) ? Data.Resources[Index].get() : nullptr;
            };

            for (const FSourceMeshInstance& Instance : Data.MeshInstances)
            {
                if (Instance.SlotIndex >= static_cast<uint32>(Data.MeshSlots.Num()))
                {
                    continue;
                }
                const FSourceMeshSlot& Slot = Data.MeshSlots[Instance.SlotIndex];

                if (const FMeshResource* Static = ResourceAt(Slot.StaticResource))
                {
                    TotalStaticVerts   += Static->GetNumVertices();
                    TotalStaticIndices += Static->GetNumIndices();
                }
                if (const FMeshResource* Skinned = ResourceAt(Slot.SkinnedResource))
                {
                    TotalSkinnedVerts   += Skinned->GetNumVertices();
                    TotalSkinnedIndices += Skinned->GetNumIndices();
                }
            }

            // A mesh's vertex arrays count in int32, so a merge past that would fail mid-copy rather than up front.
            constexpr int64 MaxMergedVerts = TNumericLimits<int32>::Max();
            if (TotalStaticVerts > MaxMergedVerts || TotalSkinnedVerts > MaxMergedVerts)
            {
                OutError = FString(*Format(
                    "Merging would flatten every instance into {0} static / {1} skinned vertices, past the {2} "
                    "vertex limit of one mesh. Import with 'Merge Meshes' off to get one asset "
                    "per unique mesh instead.",
                    TotalStaticVerts, TotalSkinnedVerts, MaxMergedVerts));
                return false;
            }

            TUniquePtr<FMeshResource> MergedStatic = MakeUnique<FMeshResource>();
            TUniquePtr<FMeshResource> MergedSkinned = MakeUnique<FMeshResource>();
            MergedSkinned->bSkinnedMesh = true;

            for (const TUniquePtr<FMeshResource>& Resource : Data.Resources)
            {
                if (!Resource) { continue; }
                if (!Resource->bSkinnedMesh && MergedStatic->Name.IsNone())  { MergedStatic->Name  = Resource->Name; }
                if (Resource->bSkinnedMesh  && MergedSkinned->Name.IsNone()) { MergedSkinned->Name = Resource->Name; }
            }

            // Growing per instance would memcpy the whole buffer set and hold two allocations mid-realloc.
            if (TotalStaticVerts > 0)
            {
                MergedStatic->ReserveVertices(static_cast<int32>(TotalStaticVerts));
                MergedStatic->Indices.Reserve(IntCastChecked<int32>(TotalStaticIndices));
            }
            if (TotalSkinnedVerts > 0)
            {
                MergedSkinned->ReserveVertices(static_cast<int32>(TotalSkinnedVerts));
                MergedSkinned->Indices.Reserve(IntCastChecked<int32>(TotalSkinnedIndices));
            }

            TVector<int16> SlotToSource;

            auto AppendInstance = [&](const FMeshResource& Source, FMeshResource& Target, const FMatrix4& World)
            {
                const FMatrix3 NormalMatrix = Math::Transpose(Math::Inverse(FMatrix3(World)));

                const int32 BaseVertex = Target.GetNumVertices();
                const int32 BaseIndex  = Target.GetNumIndices();
                const int32 VertexCount = Source.GetNumVertices();

                Target.ResizeVertices(BaseVertex + VertexCount);

                for (int32 i = 0; i < VertexCount; ++i)
                {
                    Target.Positions[BaseVertex + i] = FVector3(World * FVector4(Source.Positions[i], 1.0f));
                    Target.Normals[BaseVertex + i]   = PackNormal(Math::Normalize(NormalMatrix * UnpackNormal(Source.Normals[i])));
                    Target.UVs[BaseVertex + i]       = Source.UVs[i];
                    Target.UVs1[BaseVertex + i]      = Source.UVs1[i];
                    Target.Colors[BaseVertex + i]    = Source.Colors[i];
                }
                if (Target.bSkinnedMesh && Source.bSkinnedMesh)
                {
                    for (int32 i = 0; i < VertexCount; ++i)
                    {
                        Target.JointIndices[BaseVertex + i] = Source.JointIndices[i];
                        Target.JointWeights[BaseVertex + i] = Source.JointWeights[i];
                    }
                }

                const int32 IndexCount = Source.GetNumIndices();
                Target.Indices.SetNum(BaseIndex + IndexCount);
                for (int32 i = 0; i < IndexCount; ++i)
                {
                    Target.Indices[BaseIndex + i] = Source.Indices[i] + static_cast<uint32>(BaseVertex);
                }

                // Keyed per piece, so each merged piece keeps its own slot even where pieces share a material.
                THashMap<int16, int16> PieceSlots;
                for (const FGeometrySurface& SourceSurface : Source.GeometrySurfaces)
                {
                    FGeometrySurface Surface = SourceSurface;
                    Surface.StartIndex += (uint32)BaseIndex;

                    if (SourceSurface.MaterialIndex >= 0)
                    {
                        auto It = PieceSlots.Find(SourceSurface.MaterialIndex);
                        if (It == nullptr)
                        {
                            const int16 NewSlot = (int16)SlotToSource.Num();
                            SlotToSource.Add(SourceSurface.MaterialIndex);
                            PieceSlots.TryAdd(SourceSurface.MaterialIndex, NewSlot);
                            Surface.MaterialIndex = NewSlot;
                        }
                        else
                        {
                            Surface.MaterialIndex = (*It);
                        }
                    }

                    Target.GeometrySurfaces.Add(Surface);
                }
            };

            for (const FSourceMeshInstance& Instance : Data.MeshInstances)
            {
                if (Instance.SlotIndex >= static_cast<uint32>(Data.MeshSlots.Num()))
                {
                    continue;
                }
                const FSourceMeshSlot& Slot = Data.MeshSlots[Instance.SlotIndex];

                if (const FMeshResource* Static = ResourceAt(Slot.StaticResource))
                {
                    AppendInstance(*Static, *MergedStatic, Instance.WorldTransform);
                }
                // The inverse binds already carry the node transform, so baking it too would apply it twice.
                if (const FMeshResource* Skinned = ResourceAt(Slot.SkinnedResource))
                {
                    AppendInstance(*Skinned, *MergedSkinned, FMatrix4(1.0f));
                }
            }

            Data.MergedMaterialSlotToSource = Move(SlotToSource);

            Data.Resources.Reset();
            Data.MeshSlots.Reset();
            Data.MeshInstances.Reset();

            if (MergedStatic->GetNumVertices() > 0)  { Data.Resources.Add(Move(MergedStatic)); }
            if (MergedSkinned->GetNumVertices() > 0) { Data.Resources.Add(Move(MergedSkinned)); }

            return true;
        }

    }

    namespace
    {
        // Luminous efficacy at 555nm, the constant DCC exporters use to convert watts into lumens.
        constexpr float GLuminousEfficacy = 683.0f;

        // Steradians in a sphere; a point light's candela is its lumens spread over all of them.
        constexpr float GSphereSteradians = 4.0f * Math::Pi<float>();

        // SPointLightComponent's own default, which a relative source's strongest lamp maps onto.
        constexpr float GDefaultPunctualIntensity = 10.0f;

        float RelativeShare(float Intensity, float BrightestOfKind)
        {
            return (BrightestOfKind > 0.0f) ? Math::Clamp(Intensity / BrightestOfKind, 0.0f, 1.0f) : 1.0f;
        }

        bool IsSourceLight(ESourceNodeKind Kind)
        {
            return Kind == ESourceNodeKind::PointLight
                || Kind == ESourceNodeKind::SpotLight
                || Kind == ESourceNodeKind::DirectionalLight;
        }

        FVector3 SourceEmitDirection(const FSourceLight& Light)
        {
            const float LengthSq = Math::LengthSquared(Light.LocalDirection);
            return (LengthSq > Math::Epsilon<float>()) ? Math::Normalize(Light.LocalDirection) : FVector3(0.0f, 0.0f, -1.0f);
        }

        // File-local so the importer header does not have to pull in the component headers.
        void AddSceneEnvironment(ECS::FRegistry& Registry, ECS::FEntity Root, const FVector3& WorldColor)
        {
            // A flat fill, since a procedural atmosphere would add a sun and sky gradient nobody asked for.
            SEnvironmentComponent& Environment = Registry.Emplace<SEnvironmentComponent>(Root);
            Environment.bRenderSky   = true;
            Environment.SkyMode      = ESkyMode::SolidColor;
            Environment.SolidSkyColor = WorldColor;

            // Intensity carries the magnitude so the color stays a hue, and the component clamps it.
            const float Ambient = Math::Max(WorldColor.x, Math::Max(WorldColor.y, WorldColor.z));
            SSkyLightComponent& SkyLight = Registry.Emplace<SSkyLightComponent>(Root);
            SkyLight.bAffectsWorld  = true;
            SkyLight.bAmbientFromSky = false;
            SkyLight.AmbientColor   = (Ambient > 0.0f) ? (WorldColor / Ambient) : FVector3(1.0f);
            SkyLight.Intensity      = Math::Clamp(Ambient, 0.0f, 1.0f);

            // Without this the prefab inherits the host world's grade, which Lumina art-directs by default.
            SPostProcessComponent& PostProcess = Registry.Emplace<SPostProcessComponent>(Root);
            PostProcess.bEnabled        = true;
            PostProcess.bInfiniteExtent = true;
            // A default world ships a volume at priority 0 and ties resolve by iteration order, so outrank it.
            PostProcess.Priority        = 100;

            SPostProcessSettings& Settings = PostProcess.Settings;
            Settings.bEnabled             = true;
            Settings.ToneMapper           = EToneMapper::AGX;
            Settings.ExposureCompensation = 0.0f;
            Settings.bAutoExposure        = false;
            Settings.Temperature          = 0.0f;
            Settings.Tint                 = 0.0f;
            Settings.Contrast             = 1.0f;
            Settings.Saturation           = 1.0f;
            Settings.Gamma                = 1.0f;
            Settings.BloomIntensity       = 0.0f;
            Settings.VignetteIntensity    = 0.0f;
            Settings.ChromaticAberration  = 0.0f;
            Settings.FilmGrainIntensity   = 0.0f;
        }
    }

    float CMeshImporter::ConvertDirectionalIntensity(const FSourceLight& Light, float BrightestOfKind) const
    {
        const float Source = Light.Intensity * LightIntensityScale;
        if (LightUnits == ELightImportUnits::Raw)
        {
            return Source;
        }

        if (Light.Units == ESourceLightUnits::Relative)
        {
            return RelativeShare(Light.Intensity, BrightestOfKind) * DirectionalLightCalibration * LightIntensityScale;
        }

        // lux -> W/m^2 -> engine units.
        return (Source / GLuminousEfficacy) * DirectionalLightCalibration;
    }

    float CMeshImporter::ConvertPunctualIntensity(const FSourceLight& Light, float BrightestOfKind) const
    {
        const float Source = Light.Intensity * LightIntensityScale;
        if (LightUnits == ELightImportUnits::Raw)
        {
            return Source;
        }

        if (Light.Units == ESourceLightUnits::Relative)
        {
            return RelativeShare(Light.Intensity, BrightestOfKind) * GDefaultPunctualIntensity * LightIntensityScale;
        }

        // candela -> lumens -> watts -> engine units.
        const float Watts = (Source * GSphereSteradians) / GLuminousEfficacy;
        return Watts * PunctualLightCalibration;
    }

    CPrefab* CMeshImporter::BuildScenePrefab(const FFixedString& PackagePath, FStringView PrefabName,
                                             const TVector<CMesh*>& ResourceToMesh) const
    {
        const TVector<FSourceSceneNode>& Nodes = SourceData.SceneNodes;
        if (Nodes.IsEmpty())
        {
            return nullptr;
        }

        // Dropping the rest keeps an exporter's empty grouping nodes from becoming thousands of entities.
        TVector<uint8> bKeep(Nodes.Num(), 0);
        for (int32 i = 0; i < Nodes.Num(); ++i)
        {
            // An empty with user properties is a marker the author placed on purpose, such as a spawn point.
            if (Nodes[i].Kind == ESourceNodeKind::Empty && Nodes[i].Extras.IsEmpty())
            {
                continue;
            }
            // Cameras are always parsed, so a declined camera import drops them here rather than leaving a shell.
            if (Nodes[i].Kind == ESourceNodeKind::Camera && !bImportCameras)
            {
                continue;
            }
            if (IsSourceLight(Nodes[i].Kind) && !bImportLights)
            {
                continue;
            }
            for (int32 Walk = (int32)i; Walk != Constants::kIndexNone && bKeep[Walk] == 0; Walk = Nodes[Walk].ParentIndex)
            {
                bKeep[Walk] = 1;
            }
        }

        size_t KeptCount = 0;
        for (uint8 Keep : bKeep)
        {
            KeptCount += Keep;
        }
        if (KeptCount == 0)
        {
            return nullptr;
        }

        CPrefab* Prefab = CFactory::CreateNewOf<CPrefab>(PackagePath);
        if (Prefab == nullptr)
        {
            return nullptr;
        }
        Prefab->SetFlag(OF_NeedsPostLoad);

        ECS::FRegistry& Registry = Prefab->Registry;

        auto MakeEntity = [&Registry](const FName& Name, const FTransform& Transform) -> ECS::FEntity
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<SNameComponent>(Entity, Name);
            Registry.Emplace<STransformComponent>(Entity, Transform);
            Registry.Emplace<SPrefabComponent>(Entity).StableID = FName(FGuid::New().ToShortString());
            return Entity;
        };

        // A prefab instantiates from a single root, so a source with several top-level nodes gets one.
        size_t RootCount = 0;
        for (int32 i = 0; i < Nodes.Num(); ++i)
        {
            if (bKeep[i] != 0 && Nodes[i].ParentIndex == Constants::kIndexNone)
            {
                ++RootCount;
            }
        }

        ECS::FEntity SharedRoot = ECS::NullEntity;
        if (RootCount != 1)
        {
            SharedRoot = MakeEntity(FName(FFixedString(PrefabName.GetData(), PrefabName.Len())), FTransform());
        }

        TVector<ECS::FEntity> NodeEntities;
        NodeEntities.SetNum(Nodes.Num(), ECS::NullEntity);

        // A directional light stores a world vector rather than deriving one from its transform.
        TVector<FQuat> WorldRotations(Nodes.Num(), FQuat(1.0f, 0.0f, 0.0f, 0.0f));

        bool   bClaimedActiveCamera = false;
        uint32 LightsImported       = 0;

        // A relative source states no absolute unit, so its own brightest light of each kind sets the scale.
        float BrightestDirectional = 0.0f;
        float BrightestPunctual    = 0.0f;
        for (const FSourceSceneNode& Node : Nodes)
        {
            if (Node.Light.Units != ESourceLightUnits::Relative)
            {
                continue;
            }
            if (Node.Kind == ESourceNodeKind::DirectionalLight)
            {
                BrightestDirectional = Math::Max(BrightestDirectional, Node.Light.Intensity);
            }
            else if (IsSourceLight(Node.Kind))
            {
                BrightestPunctual = Math::Max(BrightestPunctual, Node.Light.Intensity);
            }
        }

        for (int32 i = 0; i < Nodes.Num(); ++i)
        {
            const FSourceSceneNode& Node = Nodes[i];

            WorldRotations[i] = (Node.ParentIndex != Constants::kIndexNone)
                ? WorldRotations[Node.ParentIndex] * Node.Rotation
                : Node.Rotation;

            if (bKeep[i] == 0)
            {
                continue;
            }

            FTransform Transform;
            Transform.SetLocation(Node.Translation);
            Transform.SetScale(Node.Scale);

            if (Node.Kind == ESourceNodeKind::Camera && bImportCameras)
            {
                // A glTF camera looks down local -Z, so yawing 180 frames the same view rather than the opposite.
                Transform.SetRotation(Node.Rotation * FQuat(FVector3(0.0f, Math::Pi<float>(), 0.0f)));
            }
            else if (Node.Kind == ESourceNodeKind::SpotLight && bImportLights)
            {
                // A spot lights along the entity's +Z, so the source's own emit axis has to be turned onto it.
                Transform.SetRotation(Node.Rotation * Math::RotationBetween(FVector3(0.0f, 0.0f, 1.0f), SourceEmitDirection(Node.Light)));
            }
            else
            {
                Transform.SetRotation(Node.Rotation);
            }

            const ECS::FEntity Entity = MakeEntity(Node.Name, Transform);
            NodeEntities[i] = Entity;

            const ECS::FEntity Parent = (Node.ParentIndex != Constants::kIndexNone) ? NodeEntities[Node.ParentIndex] : SharedRoot;
            if (Parent != ECS::NullEntity)
            {
                Registry.AttachChild(Entity, Parent);
            }

            switch (Node.Kind)
            {
            case ESourceNodeKind::Mesh:
                {
                    if (Node.MeshSlot < 0 || (size_t)Node.MeshSlot >= static_cast<size_t>(SourceData.MeshSlots.Num()))
                    {
                        break;
                    }
                    const FSourceMeshSlot& Slot = SourceData.MeshSlots[Node.MeshSlot];

                    auto MeshAt = [&ResourceToMesh](int32 Index) -> CMesh*
                    {
                        return (Index >= 0 && (size_t)Index < static_cast<size_t>(ResourceToMesh.Num())) ? ResourceToMesh[Index] : nullptr;
                    };

                    CStaticMesh* Static = Cast<CStaticMesh>(MeshAt(Slot.StaticResource));
                    if (Static != nullptr)
                    {
                        Registry.Emplace<SStaticMeshComponent>(Entity).StaticMesh = Static;
                    }
                    ApplyNodeExtras(Registry, Entity, Node, Static, SceneCollision);
                    if (CSkeletalMesh* Skinned = Cast<CSkeletalMesh>(MeshAt(Slot.SkinnedResource)))
                    {
                        Registry.Emplace<SSkeletalMeshComponent>(Entity).SkeletalMesh = Skinned;
                    }
                    break;
                }

            case ESourceNodeKind::PointLight:
                {
                    if (!bImportLights)
                    {
                        break;
                    }

                    SPointLightComponent& Light = Registry.Emplace<SPointLightComponent>(Entity);
                    Light.LightColor = Node.Light.Color;
                    Light.Intensity  = ConvertPunctualIntensity(Node.Light, BrightestPunctual);
                    // glTF range 0 means unbounded, which a clustered renderer cannot express.
                    if (Node.Light.Range > 0.0f)
                    {
                        Light.Attenuation = Node.Light.Range;
                    }
                    ++LightsImported;
                    break;
                }

            case ESourceNodeKind::SpotLight:
                {
                    if (!bImportLights)
                    {
                        break;
                    }

                    SSpotLightComponent& Light = Registry.Emplace<SSpotLightComponent>(Entity);
                    Light.LightColor      = Node.Light.Color;
                    Light.Intensity       = ConvertPunctualIntensity(Node.Light, BrightestPunctual);
                    Light.InnerConeAngle  = Math::Degrees(Node.Light.InnerConeAngle);
                    Light.OuterConeAngle  = Math::Degrees(Node.Light.OuterConeAngle);
                    if (Node.Light.Range > 0.0f)
                    {
                        Light.Attenuation = Node.Light.Range;
                    }
                    ++LightsImported;
                    break;
                }

            case ESourceNodeKind::DirectionalLight:
                {
                    if (!bImportLights)
                    {
                        break;
                    }

                    SDirectionalLightComponent& Light = Registry.Emplace<SDirectionalLightComponent>(Entity);
                    Light.Color     = Node.Light.Color;
                    Light.Intensity = ConvertDirectionalIntensity(Node.Light, BrightestDirectional);
                    // The engine stores the TO-LIGHT vector, which is the reverse of where the source emits.
                    Light.Direction = Math::Normalize(Math::Rotate(WorldRotations[i], -SourceEmitDirection(Node.Light)));
                    ++LightsImported;
                    break;
                }

            case ESourceNodeKind::Camera:
                {
                    // A camera node kept only because a mesh hangs off it still reaches this switch.
                    if (!bImportCameras)
                    {
                        break;
                    }

                    SCameraComponent& Camera = Registry.Emplace<SCameraComponent>(Entity);
                    Camera.FOV       = Math::Degrees(Node.Camera.YFov);
                    Camera.NearPlane = Node.Camera.ZNear;
                    if (Node.Camera.ZFar > Node.Camera.ZNear)
                    {
                        Camera.FarPlane = Node.Camera.ZFar;
                    }

                    // Only the first camera claims activation, so a multi-camera source does not fight on spawn.
                    Camera.bAutoActivate = !bClaimedActiveCamera;
                    bClaimedActiveCamera = true;

                    if (Node.Camera.bOrthographic && Node.Camera.OrthoWidth > 0.0f)
                    {
                        Camera.SetOrthographic(Node.Camera.OrthoWidth);
                    }
                    else
                    {
                        Camera.SetPerspectiveProjection();
                    }
                    Camera.ApplyCameraProperties();
                    break;
                }

            default:
                break;
            }

            if (Node.Kind != ESourceNodeKind::Mesh)
            {
                ApplyNodeExtras(Registry, Entity, Node, nullptr, ESceneCollision::None);
            }
        }

        if (bCreateSceneEnvironment)
        {
            // The instantiation root is the one entity guaranteed to exist for the life of the instance.
            ECS::FEntity EnvironmentRoot = SharedRoot;
            if (EnvironmentRoot == ECS::NullEntity)
            {
                for (int32 i = 0; i < Nodes.Num(); ++i)
                {
                    if (NodeEntities[i] != ECS::NullEntity && Nodes[i].ParentIndex == Constants::kIndexNone)
                    {
                        EnvironmentRoot = NodeEntities[i];
                        break;
                    }
                }
            }

            if (EnvironmentRoot != ECS::NullEntity)
            {
                AddSceneEnvironment(Registry, EnvironmentRoot, WorldColor);
            }
        }

        LOG_INFO("[Import] scene prefab '{}': {} entities from {} source nodes, {} light(s); relative anchors sun={} punctual={}",
                 *PackagePath, KeptCount + (SharedRoot != ECS::NullEntity ? 1 : 0), Nodes.Num(), LightsImported,
                 BrightestDirectional, BrightestPunctual);

        return Prefab;
    }

    FMeshImportOptions CMeshImporter::BuildOptions(bool bSkipFinalization) const
    {
        FMeshImportOptions Options;
        Options.bOptimize         = bOptimize;
        Options.bImportMaterials  = bImportMaterials;
        Options.bImportTextures   = bImportTextures || bImportMaterials;
        Options.bImportMeshes     = bImportMeshes;
        Options.bImportAnimations = bImportAnimations;
        Options.bImportSkeleton   = bImportSkeleton;
        Options.bImportUnskinnedBones = bImportUnskinnedBones;
        Options.bFlipNormals      = bFlipNormals;
        Options.bFlipUVs          = bFlipUVs;
        Options.bFlipU            = bFlipU;
        Options.bMergeMeshes      = bMergeMeshes;
        Options.Scale             = Scale;
        Options.bSkipFinalization = bSkipFinalization;
        Options.DistanceField     = DistanceField;
        return Options;
    }

    bool CMeshImporter::ParseSource(const FImportRequest& Request, FString& OutError, FScopedSlowTask* Progress)
    {
        // User transforms and heavy passes are deferred to BuildAssets; only the skeleton's bone set re-parses.
        FMeshImportOptions PreviewOptions;
        PreviewOptions.bOptimize         = false;
        PreviewOptions.bMergeMeshes      = false;
        PreviewOptions.bFlipNormals      = false;
        PreviewOptions.bFlipUVs          = false;
        PreviewOptions.Scale             = 1.0f;
        PreviewOptions.bSkipFinalization = true;
        PreviewOptions.bImportUnskinnedBones = bImportUnskinnedBones;

        ParsedRequest = Request;
        bParsedWithUnskinnedBones = bImportUnskinnedBones;

        SourceData = FMeshImportData();
        return ParseMeshSource(Request, PreviewOptions, SourceData, OutError, Progress);
    }

    void CMeshImporter::PrepareSettingsPreview()
    {
        if (SourceData.Images.IsEmpty())
        {
            return;
        }

        Jobs::ParallelFor((uint32)SourceData.Images.Num(), [this](uint32 Index)
        {
            FSourceImage& Image = SourceData.Images[Index];
            if (Image.Thumbnail.IsValid())
            {
                return;
            }
            if (Image.IsEmbedded())
            {
                Image.Thumbnail = RenderUtils::CreateImageFromPixels(Image.Bytes, true, FUIntVector2(128, 128), "MeshImport.EmbeddedThumbnail");
            }
            else if (!Image.ResolvedPath.IsEmpty())
            {
                Image.Thumbnail = Import::Textures::CreateTextureFromImport(Image.ResolvedPath, true, FUIntVector2(128, 128));
            }
        });
    }

    void CMeshImporter::ReleaseSourceData()
    {
        SourceData = FMeshImportData();
    }

    bool CMeshImporter::BindSkinningToTargetSkeleton()
    {
        if (TargetSkeleton == nullptr || !bImportMeshes)
        {
            return false;
        }

        bool bAnySkinned = false;
        for (const TUniquePtr<FMeshResource>& Resource : SourceData.Resources)
        {
            if (Resource && Resource->bSkinnedMesh)
            {
                bAnySkinned = true;
                break;
            }
        }

        if (!bAnySkinned)
        {
            return false;
        }

        const FSkeletonResource* Target = TargetSkeleton->GetSkeletonResource();
        if (Target == nullptr || Target->GetNumBones() == 0)
        {
            LOG_WARN("[Import] target skeleton '{}' has no bones; skinned meshes keep this file's own skeleton.",
                     TargetSkeleton->GetName());
            return false;
        }

        const FBoneRemapResult Remap = RemapJointIndicesToSkeleton(SourceData, *Target);

        if (!Remap.bApplied)
        {
            LOG_WARN("[Import] no bone of this file matches '{}' by name, so skinned meshes keep this file's "
                     "own skeleton. Check the rigs are the same or clear Target Skeleton.",
                     TargetSkeleton->GetName());
            return false;
        }

        LOG_INFO("[Import] rebound skinning to '{}': {}/{} bones matched by name.",
                 TargetSkeleton->GetName(), Remap.MatchedBones, Remap.MatchedBones + Remap.UnmatchedBones);

        if (Remap.UnmatchedBones > 0)
        {
            LOG_WARN("[Import] {} bone(s) of this file are missing from '{}' (first: '{}'); {} vertex influence(s) "
                     "were dropped and their weights redistributed.",
                     Remap.UnmatchedBones, TargetSkeleton->GetName(), Remap.FirstUnmatched, Remap.DroppedInfluences);
        }

        if (Remap.ClampedInfluences > 0)
        {
            LOG_ERROR("[Import] '{}' indexes bones past {}, so {} influence(s) clamped and will skin to "
                      "the wrong bone.", TargetSkeleton->GetName(), kMaxJointIndex, Remap.ClampedInfluences);
        }

        return true;
    }

    void CMeshImporter::BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress)
    {
        FMeshImportOptions Options = BuildOptions(false);

        constexpr float kFinalizeBudget = 0.72f;
        constexpr float kCreateBudget   = 0.05f;
        constexpr float kTextureBudget  = 0.13f;
        constexpr float kSaveBudget     = 0.10f;

        // A scene graph merges through the instance table, while FBX and OBJ fall through to concatenation.
        if (bMergeMeshes && !SourceData.MeshInstances.IsEmpty())
        {
            if (Progress)
            {
                Progress->UpdateMessage("Merging instances...");
            }
            if (!MergeInstancesIntoSingleMesh(SourceData, OutResult.Error))
            {
                return;
            }
            Options.bMergeMeshes = false;
        }

        // Must run before finalization bakes the raw joint arrays into meshlet vertices.
        const bool bBoundToTargetSkeleton = BindSkinningToTargetSkeleton();

        FinalizeMeshImportData(SourceData, Options, Progress, kFinalizeBudget);

        FFixedString DestinationDir;
        FFixedString BaseName;
        const int32 LastSlashPos = Request.DestinationPath.FindLastOf('/');
        if (LastSlashPos == Constants::kIndexNone)
        {
            BaseName = Request.DestinationPath;
        }
        else
        {
            DestinationDir = Request.DestinationPath.Mid(0, LastSlashPos + 1);
            BaseName       = Request.DestinationPath.Mid(LastSlashPos + 1, Constants::kIndexNone);
        }
        // A reimport names its package file, and any other dot belongs to the name the destination reserved.
        if (VFS::HasExtension(BaseName, ".lasset"))
        {
            BaseName = BaseName.Mid(0, BaseName.Len() - FStringView(".lasset").Len());
        }

        FFixedString TexturesDir  = DestinationDir; TexturesDir.Append("Textures/");
        FFixedString MaterialsDir = DestinationDir; MaterialsDir.Append("Materials/");

        FUniquePathAllocator Paths(Request.DestinationPath);
        auto DesiredPath = [&](FStringView Suffix) -> FFixedString
        {
            FFixedString Path = DestinationDir;
            Path.Append(BaseName);
            if (!Suffix.IsEmpty())
            {
                Path.Append("_");
                // Clip and skeleton names come from the source file, where a separator would open a folder.
                for (char Character : Suffix)
                {
                    Path.AppendChar(FStringView("\\/:*?\"<>|").Find(Character) == Constants::kIndexNone ? Character : '_');
                }
            }
            return Path;
        };
        auto BuildPath = [&](FStringView Suffix) -> FFixedString
        {
            return Paths.Claim(DesiredPath(Suffix));
        };

        if (Progress)
        {
            Progress->EnterProgressFrame(kCreateBudget, "Creating assets...");
        }

        TVector<TStrongObjectPtr<CObject>>& CreatedObjects = OutResult.CreatedObjects;
        CreatedObjects.Reserve(SourceData.Skeletons.Num() + SourceData.Resources.Num()
                             + SourceData.Animations.Num() + SourceData.Images.Num());

        // Indexed by Resources index, so the prefab can resolve a scene node's mesh slot to its asset.
        TVector<CMesh*> ResourceToMesh(SourceData.Resources.Num(), nullptr);

        TStrongObjectPtr<CSkeleton> PrimarySkeleton;
        const bool bMultipleSkeletons = SourceData.Skeletons.Num() > 1;

        // With a target skeleton the file's own only binds skinned meshes, so skip minting a duplicate.
        bool bFileSkeletonNeeded = TargetSkeleton == nullptr;
        if (!bFileSkeletonNeeded && bImportMeshes && !bBoundToTargetSkeleton)
        {
            for (const TUniquePtr<FMeshResource>& Resource : SourceData.Resources)
            {
                if (Resource && Resource->bSkinnedMesh)
                {
                    bFileSkeletonNeeded = true;
                    break;
                }
            }
        }

        for (int32 i = 0; bImportSkeleton && bFileSkeletonNeeded && i < SourceData.Skeletons.Num(); ++i)
        {
            TUniquePtr<FSkeletonResource>& SkeletonRes = SourceData.Skeletons[i];
            if (!SkeletonRes || !SkeletonRes->bShouldImport)
            {
                continue;
            }

            const FFixedString SkeletonPath = bMultipleSkeletons ? BuildPath(SkeletonRes->Name.ToString()) : BuildPath("Skeleton");

            CSkeleton* NewSkeleton = CFactory::CreateNewOf<CSkeleton>(SkeletonPath);
            if (NewSkeleton == nullptr)
            {
                AppendCreateFailure(OutResult, SkeletonPath);
                continue;
            }
            NewSkeleton->SetFlag(OF_NeedsPostLoad);
            NewSkeleton->SkeletonResource = Move(SkeletonRes);

            if (!PrimarySkeleton)
            {
                PrimarySkeleton = NewSkeleton;
            }
            CreatedObjects.Add(NewSkeleton);
        }

        // Merge mode densified globally at parse time, so everything else is packed here alongside ResourceToMesh.
        TVector<TVector<int16>> ResourceSlotToSource(SourceData.Resources.Num());
        if (!bMergeMeshes)
        {
            for (int32 i = 0; i < SourceData.Resources.Num(); ++i)
            {
                if (SourceData.Resources[i])
                {
                    ResourceSlotToSource[i] = DensifyMaterialSlots(*SourceData.Resources[i]);
                }
            }
        }

        const bool bMultipleMeshes = SourceData.Resources.Num() > 1;
        for (int32 i = 0; bImportMeshes && i < SourceData.Resources.Num(); ++i)
        {
            TUniquePtr<FMeshResource>& MeshResource = SourceData.Resources[i];
            if (!MeshResource)
            {
                continue;
            }

            const FFixedString MeshDesiredPath = bMultipleMeshes ? DesiredPath(MeshResource->Name.ToString()) : DesiredPath({});
            if (Request.bReplaceExisting)
            {
                // A file that once held several meshes named this one File_Mesh, so a re-export down to one mesh still finds it.
                CMesh* Existing = FindMeshToOverwrite(MeshDesiredPath, MeshResource->bSkinnedMesh);
                if (Existing == nullptr && !bMultipleMeshes && !MeshResource->Name.IsNone())
                {
                    Existing = FindMeshToOverwrite(DesiredPath(MeshResource->Name.ToString()), MeshResource->bSkinnedMesh);
                }
                if (Existing != nullptr)
                {
                    ReplaceMeshInPlace(Existing, Move(MeshResource), OverrideMaterial.Get(), Request.SourcePath);
                    continue;
                }
            }
            const FFixedString MeshPath = Paths.Claim(MeshDesiredPath);

            CMesh* NewMesh = nullptr;
            if (!MeshResource->bSkinnedMesh)
            {
                NewMesh = CFactory::CreateNewOf<CStaticMesh>(MeshPath);
            }
            else
            {
                CSkeletalMesh* NewSkeletalMesh = CFactory::CreateNewOf<CSkeletalMesh>(MeshPath);

                CSkeleton* MeshSkeleton = bBoundToTargetSkeleton ? TargetSkeleton.Get() : PrimarySkeleton.Get();
                if (NewSkeletalMesh != nullptr && MeshSkeleton != nullptr)
                {
                    NewSkeletalMesh->Skeleton = MeshSkeleton;
                    NewSkeletalMesh->SkeletonJointIndicesAddress = MeshSkeleton;

                    // Only ever on a skeleton this import owns; the target belongs to another asset.
                    if (MeshSkeleton == PrimarySkeleton.Get() && !PrimarySkeleton->PreviewMesh)
                    {
                        PrimarySkeleton->PreviewMesh = NewSkeletalMesh;
                    }
                }
                NewMesh = NewSkeletalMesh;
            }

            if (NewMesh == nullptr)
            {
                AppendCreateFailure(OutResult, MeshPath);
                continue;
            }
            NewMesh->SetFlag(OF_NeedsPostLoad);
            NewMesh->Materials.Reset();
            NewMesh->Materials.SetNum(IntCastChecked<int32>(CountMaterialSlots(*MeshResource)));
            NewMesh->SourcePath            = FString(*Request.SourcePath);
            NewMesh->DistanceFieldSettings = DistanceField;
            NewMesh->MeshResources         = Move(MeshResource);

            // Bake the exact box now, while the raw float positions are still around to measure.
            NewMesh->GenerateBoundingBox();

            CreatedObjects.Add(NewMesh);
            ResourceToMesh[i] = NewMesh;
        }

        const bool bMultipleAnims = SourceData.Animations.Num() > 1;
        for (int32 i = 0; bImportAnimations && i < SourceData.Animations.Num(); ++i)
        {
            TUniquePtr<FAnimationResource>& Clip = SourceData.Animations[i];
            if (!Clip)
            {
                continue;
            }

            AnimCompression::Build(*Clip);

            const FString AnimSuffix = bMultipleAnims ? Clip->Name.ToString() : FString("Animation");
            CSkeleton* AnimSkeleton = TargetSkeleton != nullptr ? TargetSkeleton.Get() : PrimarySkeleton.Get();

            if (CAnimation* Existing = FindAnimationToOverwrite(DesiredPath(AnimSuffix)))
            {
                // Only an explicit target replaces the skeleton, never one this file just minted.
                ReplaceAnimationInPlace(Existing, Move(Clip), TargetSkeleton.Get());
                continue;
            }

            const FFixedString AnimationPath = BuildPath(AnimSuffix);
            CAnimation* NewAnimation = CFactory::CreateNewOf<CAnimation>(AnimationPath);
            if (NewAnimation == nullptr)
            {
                AppendCreateFailure(OutResult, AnimationPath);
                continue;
            }
            NewAnimation->SetFlag(OF_NeedsPostLoad);
            NewAnimation->AnimationResource = Move(Clip);
            NewAnimation->Skeleton          = AnimSkeleton;

            CreatedObjects.Add(NewAnimation);
        }

        FScopedAssetRegistryBatch RegistryBatch;

        // Resolved by IMAGE INDEX, so binding a channel is an array lookup rather than a path hash.
        TVector<CTexture*> ImageAssets(SourceData.Images.Num(), nullptr);

        const bool bWantTextures = bImportTextures || bImportMaterials;
        if (bWantTextures && !SourceData.Images.IsEmpty())
        {
            if (Progress)
            {
                Progress->UpdateMessage("Importing textures...");
            }

            for (const FMeshImportMaterial& Material : SourceData.Materials)
            {
                if (Material.AlphaMode == EImportAlphaMode::Mask && Material.BaseColorImage != Constants::kIndexNone
                    && Material.BaseColorImage < SourceData.Images.Num())
                {
                    SourceData.Images[Material.BaseColorImage].AlphaCoverageCutoff = Material.AlphaCutoff;
                }
            }

            struct FTextureWork
            {
                FFixedString PackagePath;
                uint32       ImageIndex;
                bool         bNeedsCook;
            };

            TVector<FTextureWork> Work;
            Work.Reserve(SourceData.Images.Num());

            for (int32 i = 0; i < SourceData.Images.Num(); ++i)
            {
                const FSourceImage& Image = SourceData.Images[i];

                const FStringView NameSource = Image.ResolvedPath.IsEmpty()
                    ? FStringView(*Image.Key, Image.Key.Len())
                    : VFS::FileName(Image.ResolvedPath, true);

                FFixedString PackagePath = Lumina::Paths::Combine(TexturesDir, *TextureAssetName(NameSource));

                const bool bAlreadyExists = (FindObject<CPackage>(PackagePath) != nullptr);

                // Images are deduplicated by key already, so a name collision here is always two different textures.
                if (!bAlreadyExists)
                {
                    PackagePath = Paths.Claim(PackagePath);
                }

                CPackage::AddPackageExt(PackagePath);

                Work.Emplace(Move(PackagePath), (uint32)i, !bAlreadyExists);
            }

            TVector<CTexture*> Cooked(Work.Num(), nullptr);

            Jobs::ParallelFor((uint32)Work.Num(), [&](uint32 i)
            {
                const FTextureWork& W = Work[i];
                if (!W.bNeedsCook)
                {
                    return;
                }

                const FSourceImage& Image = SourceData.Images[W.ImageIndex];

                Import::Textures::FTextureCookRequest CookRequest;
                CookRequest.EmbeddedBytes      = Image.Bytes;
                // Embedded payloads have no file, so the key only feeds the color-space heuristic and is not stored.
                CookRequest.SourcePath         = Image.ResolvedPath.IsEmpty() ? Image.Key : Image.ResolvedPath;
                CookRequest.ColorSpace         = Image.IntendedColorSpace;
                CookRequest.AlphaCoverageCutoff = Image.AlphaCoverageCutoff;
                // This loop already saturates the cores, so a full basisu pool per texture would oversubscribe.
                CookRequest.EncodeThreadBudget = 1;
                // Creating a GPU image here would queue a copy against an image this import destroys first.
                CookRequest.bCreateGPUResource  = false;

                Cooked[i] = Import::Textures::ImportTextureAsset(W.PackagePath, CookRequest);
            }, 1);

            for (int32 i = 0; i < Work.Num(); ++i)
            {
                const FTextureWork& W = Work[i];
                CTexture* Texture = Cooked[i];
                if (Texture != nullptr)
                {
                    CreatedObjects.Add(Texture);
                }
                else if (bImportMaterials)
                {
                    // Skipped because the asset already existed, or a duplicate destination in this batch.
                    Texture = LoadObject<CTexture>(W.PackagePath);
                }
                ImageAssets[W.ImageIndex] = Texture;
            }
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(kTextureBudget);
        }

        // The sweep after it compares against this, so it only speaks up when the assets disagree.
        uint32 ExpectedNullSlots = 0;

        if (OverrideMaterial.IsValid() && bImportMeshes)
        {
            for (CMesh* Mesh : ResourceToMesh)
            {
                if (Mesh == nullptr)
                {
                    continue;
                }
                for (size_t Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
                {
                    Mesh->SetMaterialAtSlot(static_cast<int32>(Slot), OverrideMaterial.Get());
                }
            }
        }
        else if (bImportMaterials && !SourceData.Materials.IsEmpty())
        {
            if (Progress)
            {
                Progress->UpdateMessage("Generating materials...");
            }

            {
                const TVector<CMaterialInstance*> Instances = Import::Materials::GenerateMaterials(
                    TSpan<const FMeshImportMaterial>(SourceData.Materials.GetData(), SourceData.Materials.Num()),
                    TSpan<CTexture* const>(ImageAssets.GetData(), ImageAssets.Num()),
                    MaterialsDir, BaseName, CreatedObjects, SourceData.bHasVertexColors);

                // Each of the four ways a surface ends up materialless is counted, since the symptom is identical.
                uint32 SurfacesTotal = 0, NoAssignment = 0, OutOfRange = 0, NoSource = 0, NoInstance = 0;

                for (int32 ResIdx = 0; ResIdx < ResourceToMesh.Num(); ++ResIdx)
                {
                    CMesh* Mesh = ResourceToMesh[ResIdx];
                    if (Mesh == nullptr)
                    {
                        continue;
                    }

                    const TVector<int16>& SlotToSource = bMergeMeshes
                        ? SourceData.MergedMaterialSlotToSource
                        : ResourceSlotToSource[ResIdx];

                    Mesh->ForEachSurface([&](const FGeometrySurface& Surface, uint32)
                    {
                        ++SurfacesTotal;

                        const int32 Slot = Surface.MaterialIndex;
                        if (Slot < 0)
                        {
                            ++NoAssignment;
                            return;
                        }
                        if ((size_t)Slot >= Mesh->GetNumMaterials())
                        {
                            ++OutOfRange;
                            return;
                        }

                        // Identity only for a source that never needed remapping at all.
                        int32 SourceIndex = Slot;
                        if (!SlotToSource.IsEmpty())
                        {
                            SourceIndex = (Slot < SlotToSource.Num()) ? SlotToSource[Slot] : Constants::kIndexNone;
                        }

                        if (SourceIndex < 0 || (size_t)SourceIndex >= static_cast<size_t>(Instances.Num()))
                        {
                            ++NoSource;
                            return;
                        }
                        if (Instances[SourceIndex] == nullptr)
                        {
                            ++NoInstance;
                            return;
                        }

                        Mesh->SetMaterialAtSlot((size_t)Slot, Instances[SourceIndex]);
                    });
                }

                // A source primitive with no material is not a fault, so only the three real faults raise severity.
                const uint32 Faults = OutOfRange + NoSource + NoInstance;
                const uint32 Unresolved = NoAssignment + Faults;
                ExpectedNullSlots = Unresolved;

                if (Faults > 0)
                {
                    LOG_WARN("[Import] {}/{} surfaces imported with no material "
                             "(no source material: {}, slot out of range: {}, no slot mapping: {}, "
                             "material not generated: {}). Merge meshes: {}, source materials: {}, "
                             "generated instances: {}.",
                             Unresolved, SurfacesTotal, NoAssignment, OutOfRange, NoSource, NoInstance,
                             bMergeMeshes, (uint32)SourceData.Materials.Num(), (uint32)Instances.Num());
                }
                else if (Unresolved > 0)
                {
                    LOG_INFO("[Import] {}/{} surfaces have no material because the source assigned none.",
                             Unresolved, SurfacesTotal);
                }
            }
        }
        else if (bImportMeshes)
        {
            // Being skipped here lands every mesh with an all-null Materials array, so say which gate closed.
            LOG_WARN("[Import] no materials were generated: Import Materials is {}, and the source parsed "
                     "{} material(s). Every imported mesh will have empty material slots.",
                     bImportMaterials ? "ON" : "OFF", (uint32)SourceData.Materials.Num());
            ExpectedNullSlots = UINT32_MAX;
        }

        // Counted from the meshes themselves, which is the one thing the assignment loop cannot see.
        if (bImportMeshes)
        {
            uint32 SlotsTotal = 0, SlotsNull = 0, MeshesFullyNull = 0, MeshCount = 0;
            for (CMesh* Mesh : ResourceToMesh)
            {
                if (Mesh == nullptr)
                {
                    continue;
                }
                ++MeshCount;

                const uint32 NumSlots = Mesh->GetNumMaterials();
                uint32 NullHere = 0;
                for (uint32 Slot = 0; Slot < NumSlots; ++Slot)
                {
                    if (Mesh->GetMaterialAtSlot(Slot) == nullptr)
                    {
                        ++NullHere;
                    }
                }

                SlotsTotal += NumSlots;
                SlotsNull  += NullHere;
                if (NumSlots > 0 && NullHere == NumSlots)
                {
                    ++MeshesFullyNull;
                }
            }

            if (SlotsNull > ExpectedNullSlots)
            {
                LOG_WARN("[Import] post-assignment: {}/{} material slots are null across {} mesh(es) "
                         "({} mesh(es) have NO material at all), but assignment only accounted for {}. "
                         "Something cleared slots after they were filled.",
                         SlotsNull, SlotsTotal, MeshCount, MeshesFullyNull, ExpectedNullSlots);
            }
        }

        // Built last so everything it points at exists, and merging already flattened what a prefab preserves.
        if (bImportAsPrefab && !bMergeMeshes && !SourceData.SceneNodes.IsEmpty())
        {
            if (Progress)
            {
                Progress->UpdateMessage("Building scene prefab...");
            }

            const FFixedString PrefabPath = BuildPath("Prefab");
            if (CPrefab* Prefab = BuildScenePrefab(PrefabPath, FStringView(*BaseName, BaseName.Len()), ResourceToMesh))
            {
                CreatedObjects.Add(Prefab);
            }
        }

        if (Progress)
        {
            Progress->UpdateMessage("Saving packages...");
        }

        const float SaveStep = kSaveBudget / (float)std::max<size_t>((size_t)1, CreatedObjects.Num());
        for (const TStrongObjectPtr<CObject>& Created : CreatedObjects)
        {
            CObject* Object = Created.Get();
            if (Object == nullptr)
            {
                continue;
            }
            CPackage* Package = Object->GetPackage();
            if (CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                // A generated material node graph rides in its master's package without being a browsable asset.
                if (Object->IsAsset())
                {
                    FAssetRegistry::Get().AssetCreated(Object);
                }
            }
            else
            {
                OutResult.Error += (OutResult.Error.IsEmpty() ? "Could not save " : ", ") + FString(*Package->GetPackagePath());
                OutResult.UnsavedPackages.Add(Package);
            }

            // Saved means the file now owns the name, and a failure should free it for the next attempt.
            Import::PathReservations::Release(Package->GetPackagePath());

            if (Progress)
            {
                Progress->EnterProgressFrame(SaveStep);
            }
        }
        if (CreatedObjects.IsEmpty() && Progress)
        {
            Progress->EnterProgressFrame(kSaveBudget);
        }

        if (PrimarySkeleton)
        {
            PrimarySkeleton->PreviewMesh = nullptr;
            PrimarySkeleton = nullptr;
        }
    }

    bool CMeshImporter::CanReimport(const CStruct* AssetClass) const
    {
        return AssetClass != nullptr && AssetClass->IsChildOf(CMesh::StaticClass());
    }

    FString CMeshImporter::GetReimportSourcePath(const CObject* Asset) const
    {
        const CMesh* Mesh = Cast<CMesh>(Asset);
        return Mesh != nullptr ? Mesh->SourcePath : FString();
    }

    void CMeshImporter::RebindReimportSkinning(CMesh* Mesh)
    {
        CSkeletalMesh* SkeletalMesh = Cast<CSkeletalMesh>(Mesh);
        if (SkeletalMesh == nullptr)
        {
            return;
        }

        // Never the dialogue's target, since worlds pose this mesh against the skeleton it answers to.
        const FSkeletonResource* Target = SkeletalMesh->Skeleton.IsValid()
                                        ? SkeletalMesh->Skeleton->GetSkeletonResource()
                                        : nullptr;

        if (Target == nullptr || Target->GetNumBones() == 0)
        {
            LOG_WARN("Reimport: '{}' has no skeleton to bind to, so its joint indices keep whatever order "
                     "the source file numbers bones in.", Mesh->GetName());
            return;
        }

        const FBoneRemapResult Remap = RemapJointIndicesToSkeleton(SourceData, *Target);

        if (!Remap.bApplied)
        {
            LOG_ERROR("Reimport: no bone in this file matches '{}' by name. The replaced geometry keeps the "
                      "file's own bone order and will skin to the wrong bones.",
                      SkeletalMesh->Skeleton->GetName());
            return;
        }

        if (Remap.UnmatchedBones > 0)
        {
            LOG_WARN("Reimport: {} bone(s) in the source are missing from '{}' (first: '{}'); {} influence(s) "
                     "dropped. Reimport the skeleton to pick up bones added since.",
                     Remap.UnmatchedBones, SkeletalMesh->Skeleton->GetName(), Remap.FirstUnmatched,
                     Remap.DroppedInfluences);
        }

        if (Remap.ClampedInfluences > 0)
        {
            LOG_ERROR("Reimport: '{}' indexes bones past {}, so {} influence(s) clamped and will skin to "
                      "the wrong bone.", SkeletalMesh->Skeleton->GetName(), kMaxJointIndex, Remap.ClampedInfluences);
        }
    }

    bool CMeshImporter::ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress)
    {
        CMesh* Mesh = Cast<CMesh>(Asset);
        if (Mesh == nullptr)
        {
            return false;
        }

        // Reimport replaces one asset's data and does not mint the assets a fresh import would.
        FMeshImportOptions Options = BuildOptions(false);
        Options.bImportMeshes     = true;
        Options.bImportSkeleton   = false;
        Options.bImportAnimations = false;
        Options.bImportMaterials  = false;
        Options.bImportTextures   = false;

        // Taken from the ASSET, since otherwise a mesh with a distance field loses it on every reimport.
        Options.DistanceField = Mesh->DistanceFieldSettings;

        RebindReimportSkinning(Mesh);

        FinalizeMeshImportData(SourceData, Options, Progress, 0.9f);

        const int32 ResourceIndex = FindResourceForAsset(SourceData, Mesh->GetName(), Mesh->IsSkinned());
        if (ResourceIndex == Constants::kIndexNone)
        {
            LOG_ERROR("Reimport: '{0}' contains no {1} mesh to replace '{2}' with.",
                      *Request.SourcePath, Mesh->IsSkinned() ? "skinned" : "static", *Mesh->GetName());
            return false;
        }

        TUniquePtr<FMeshResource>& NewResource = SourceData.Resources[ResourceIndex];
        if (!NewResource || NewResource->GeometrySurfaces.IsEmpty())
        {
            LOG_ERROR("Reimport: the mesh selected from '{0}' has no surfaces; leaving '{1}' untouched.",
                      *Request.SourcePath, *Mesh->GetName());
            return false;
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.1f, "Replacing mesh data...");
        }

        // Name follows the ASSET, so the object keeps its identity through a reimport.
        NewResource->Name = Mesh->GetName();

        // Otherwise a reimport hands the asset its file-global slot numbering and undoes the dense packing.
        if (!bMergeMeshes)
        {
            DensifyMaterialSlots(*NewResource);
        }

        TVector<TStrongObjectPtr<CMaterialInterface>> RemappedMaterials;
        RemapMaterialSlots(Mesh->GetMeshResource(), Mesh->Materials, *NewResource, RemappedMaterials);
        Mesh->Materials = Move(RemappedMaterials);

        // SetMeshResource rebuilds bounds and buffers and invalidates the resolve cache, so components follow.
        Mesh->SetMeshResource(Move(NewResource));
        Mesh->SourcePath = FString(*Request.SourcePath);

        if (CPackage* Package = Mesh->GetPackage())
        {
            Package->MarkDirty();
        }

        return true;
    }

    void CMeshImporter::DrawSourcePreview()
    {
        // The skeleton and every mesh's bone indices come out of the parse, so this flag cannot apply later.
        if (bImportUnskinnedBones != bParsedWithUnskinnedBones)
        {
            FString Error;
            if (!ParseSource(ParsedRequest, Error, nullptr))
            {
                LOG_ERROR("[Import] Re-parse of '{}' failed, nothing will be imported: {}",
                          *ParsedRequest.SourcePath, *Error);
            }
            PrepareSettingsPreview();
        }

        auto Section = [](auto&& Draw)
        {
            Draw();
            ImGui::Spacing();
            ImGui::Spacing();
        };

        Section([&]
        {
            if (SourceData.Resources.IsEmpty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Meshes ({})###MeshStats", SourceData.Resources.Num());
            if (!ImGui::CollapsingHeader(*Header, ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            if (SourceData.SourceNodeCount > static_cast<uint32>(SourceData.Resources.Num()))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
                ImGui::Text("%u source nodes collapsed to %d unique meshes",
                            SourceData.SourceNodeCount, SourceData.Resources.Num());
                ImGui::PopStyleColor();
            }

            constexpr ImGuiTableFlags Flags =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

            const float Height = std::min<float>(180.0f, (SourceData.Resources.Num() + 1) * ImGui::GetTextLineHeightWithSpacing() + 8.0f);
            if (ImGui::BeginTable("MeshStatsTable", 4, Flags, ImVec2(0, Height)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Verts",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Indices",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Surfaces", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();

                ImGuiListClipper Clipper;
                Clipper.Begin(SourceData.Resources.Num());
                while (Clipper.Step())
                {
                    for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                    {
                        const FMeshResource& Resource = *SourceData.Resources[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(*Resource.Name);
                        ImGui::TableNextColumn(); ImGuiX::Text("{0}", ImGuiX::FormatSize(Resource.GetNumVertices()));
                        ImGui::TableNextColumn(); ImGuiX::Text("{0}", ImGuiX::FormatSize(Resource.Indices.Num()));
                        ImGui::TableNextColumn(); ImGuiX::Text("{0}", Resource.GeometrySurfaces.Num());
                    }
                }
                ImGui::EndTable();
            }
        });

        Section([&]
        {
            if (SourceData.SceneNodes.IsEmpty())
            {
                return;
            }

            uint32 Points = 0, Spots = 0, Directionals = 0, Cameras = 0;
            for (const FSourceSceneNode& Node : SourceData.SceneNodes)
            {
                Points       += (Node.Kind == ESourceNodeKind::PointLight);
                Spots        += (Node.Kind == ESourceNodeKind::SpotLight);
                Directionals += (Node.Kind == ESourceNodeKind::DirectionalLight);
                Cameras      += (Node.Kind == ESourceNodeKind::Camera);
            }

            const uint32 Lights = Points + Spots + Directionals;
            const FFixedString Header = FormatAs<FFixedString>("Scene ({} nodes, {} lights)###SceneGraph",
                                                               SourceData.SceneNodes.Num(), Lights);
            if (!ImGui::CollapsingHeader(*Header, ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            if (!bImportAsPrefab)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                    LE_ICON_ALERT_CIRCLE_OUTLINE " Import As Prefab is off, so no entities, lights or cameras are created.");
            }
            else if (bMergeMeshes)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                    LE_ICON_ALERT_CIRCLE_OUTLINE " Merge Meshes flattens the scene, which skips the prefab entirely.");
            }
            else if (Lights == 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                    LE_ICON_ALERT_CIRCLE_OUTLINE " This source exports no lights, so the prefab relies on the world it is placed in.");
            }

            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
            ImGui::Text("Point %u   Spot %u   Directional %u   Cameras %u", Points, Spots, Directionals, Cameras);
            ImGui::PopStyleColor();
        });

        Section([&]
        {
            if (SourceData.Images.IsEmpty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Textures ({})###Textures", SourceData.Images.Num());
            if (!ImGui::CollapsingHeader(*Header))
            {
                return;
            }

            constexpr float ThumbSize = 64.0f;
            constexpr ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner;
            if (ImGui::BeginTable("TextureTable", 2, Flags))
            {
                ImGui::TableSetupColumn("##thumb", ImGuiTableColumnFlags_WidthFixed, ThumbSize + 8);
                ImGui::TableSetupColumn("Source",  ImGuiTableColumnFlags_WidthStretch);

                ImGuiListClipper Clipper;
                Clipper.Begin(SourceData.Images.Num(), ThumbSize + 8);
                while (Clipper.Step())
                {
                    for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                    {
                        const FSourceImage& Image = SourceData.Images[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (Image.Thumbnail.IsValid())
                        {
                            ImGui::Image(ImGuiX::ToImTextureRef(Image.Thumbnail), ImVec2(ThumbSize, ThumbSize));
                        }
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGuiX::TextWrapped("{0}", Image.Key);
                    }
                }
                ImGui::EndTable();
            }
        });

        Section([&]
        {
            if (SourceData.Skeletons.IsEmpty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Skeletons ({})###Skeletons", SourceData.Skeletons.Num());
            if (!ImGui::CollapsingHeader(*Header))
            {
                return;
            }

            const FSkeletonResource* MeshTarget = TargetSkeleton != nullptr ? TargetSkeleton->GetSkeletonResource() : nullptr;
            if (MeshTarget != nullptr && !SourceData.Skeletons.IsEmpty() && SourceData.Skeletons[0])
            {
                const FSkeletonResource& FileSkeleton = *SourceData.Skeletons[0];

                int32 Matched = 0;
                for (const FSkeletonResource::FBoneInfo& Bone : FileSkeleton.Bones)
                {
                    Matched += MeshTarget->FindBoneIndex(Bone.Name) >= 0 ? 1 : 0;
                }

                ImGui::TextDisabled("Skinned meshes bind to %s; no skeleton is created from this file.",
                                    *TargetSkeleton->GetName());

                if (Matched == FileSkeleton.Bones.Num())
                {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f),
                        LE_ICON_CHECK_CIRCLE_OUTLINE " Every bone matches by name.");
                }
                else if (Matched > 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                        LE_ICON_ALERT_CIRCLE_OUTLINE " %d of %d bones match; unmatched influences are dropped.",
                        Matched, FileSkeleton.Bones.Num());
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                        LE_ICON_ALERT_CIRCLE_OUTLINE " No bone matches; this file's own skeleton is used instead.");
                }
            }

            for (TUniquePtr<FSkeletonResource>& Skeleton : SourceData.Skeletons)
            {
                ImGui::PushID(Skeleton.get());

                bool bImport = Skeleton->bShouldImport;
                if (ImGui::Checkbox("##import", &bImport))
                {
                    Skeleton->bShouldImport = bImport;
                }
                ImGui::SameLine();
                if (ImGui::TreeNodeEx(*Skeleton->Name, ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    auto DrawBone = [&](int32 BoneIdx, auto& Self) -> void
                    {
                        const FSkeletonResource::FBoneInfo& Bone = Skeleton->Bones[BoneIdx];
                        TVector<int32> Children = Skeleton->GetChildBones(BoneIdx);
                        const ImGuiTreeNodeFlags Flags = Children.IsEmpty()
                            ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                            : ImGuiTreeNodeFlags_None;
                        if (ImGui::TreeNodeEx(*Bone.Name, Flags))
                        {
                            for (int32 ChildIdx : Children)
                            {
                                Self(ChildIdx, Self);
                            }
                            if (!Children.IsEmpty())
                            {
                                ImGui::TreePop();
                            }
                        }
                    };
                    for (int32 RootIdx : Skeleton->GetRootBones())
                    {
                        DrawBone(RootIdx, DrawBone);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        });

        Section([&]
        {
            if (SourceData.Animations.IsEmpty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Animations ({})###Animations", SourceData.Animations.Num());
            if (!ImGui::CollapsingHeader(*Header))
            {
                return;
            }

            const FSkeletonResource* Target = TargetSkeleton != nullptr ? TargetSkeleton->GetSkeletonResource() : nullptr;
            if (TargetSkeleton != nullptr)
            {
                ImGui::TextDisabled("Binding to %s", *TargetSkeleton->GetName());
            }

            for (int32 i = 0; i < SourceData.Animations.Num(); ++i)
            {
                const FAnimationResource& Anim = *SourceData.Animations[i];
                ImGui::PushID((int)i);
                if (ImGui::TreeNodeEx(*Anim.Name, ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    ImGui::TextDisabled("Duration: %.2fs   Channels: %d", Anim.Duration, Anim.Channels.Num());

                    // An unmatched channel silently freezes its bone at bind pose, so surface it here.
                    if (Target != nullptr)
                    {
                        int32 Unmatched = 0;
                        for (const FAnimationChannel& Channel : Anim.Channels)
                        {
                            Unmatched += Target->FindBoneIndex(Channel.TargetBone) < 0 ? 1 : 0;
                        }

                        if (Unmatched == 0)
                        {
                            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f),
                                LE_ICON_CHECK_CIRCLE_OUTLINE " Every channel matches a bone.");
                        }
                        else
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                                LE_ICON_ALERT_CIRCLE_OUTLINE " %d of %d channels have no bone of that name.",
                                Unmatched, Anim.Channels.Num());
                        }
                    }

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        });
    }
}
