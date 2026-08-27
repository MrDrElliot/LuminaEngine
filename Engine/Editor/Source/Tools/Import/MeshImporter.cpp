#include "EditorPCH.h"
#include "MeshImporter.h"
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
#include "TaskSystem/Future.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/Import/MaterialImport.h"
#include "Tools/Import/TextureImporter.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Log/Log.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    using namespace Import::Mesh;

    namespace
    {
        // Directory and extension are stripped, leaving the prefix and a clean stem.
        FFixedString TextureAssetName(FStringView Raw)
        {
            const size_t Slash = Raw.find_last_of("/\\");
            FStringView Stem = (Slash == FStringView::npos) ? Raw : Raw.substr(Slash + 1);
            const size_t Dot = Stem.find_last_of('.');
            if (Dot != FStringView::npos && Dot > 0)
            {
                Stem = Stem.substr(0, Dot);
            }
            return FFixedString(Import::MakeAssetName("T_", Stem).c_str());
        }

        // FindObject only sees loaded objects, so a multi-asset run needs its own claim set.
        class FUniquePathAllocator
        {
        public:

            FFixedString Claim(const FFixedString& Desired)
            {
                if (IsFree(Desired))
                {
                    Claimed.insert(Desired);
                    return Desired;
                }

                for (uint32 N = 1; N < 10000; ++N)
                {
                    FFixedString Candidate = Desired;
                    Candidate.append("_").append(Format("{}", N));
                    if (IsFree(Candidate))
                    {
                        Claimed.insert(Candidate);
                        return Candidate;
                    }
                }
                return Desired;
            }

        private:

            bool IsFree(const FFixedString& Path) const
            {
                return Claimed.find(Path) == Claimed.end() && FindObject<CPackage>(Path) == nullptr;
            }

            THashSet<FFixedString> Claimed;
        };

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
            return bAnyExplicit ? SlotCount : Resource.GeometrySurfaces.size();
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
                const auto It = SourceToSlot.find(Source);
                if (It != SourceToSlot.end())
                {
                    Surface.MaterialIndex = It->second;
                    continue;
                }

                const int16 NewSlot = (int16)SlotToSource.size();
                SourceToSlot.emplace(Source, NewSlot);
                SlotToSource.push_back(Source);
                Surface.MaterialIndex = NewSlot;
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

            if (Data.Skeletons.empty() || !Data.Skeletons[0] || Data.Skeletons[0]->Bones.empty())
            {
                return Result;
            }

            const FSkeletonResource& Source = *Data.Skeletons[0];

            TVector<int32> SourceToTarget(Source.Bones.size(), INDEX_NONE);
            for (size_t i = 0; i < Source.Bones.size(); ++i)
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

                const size_t VertexCount = Math::Min(Resource->JointIndices.size(), Resource->JointWeights.size());
                for (size_t v = 0; v < VertexCount; ++v)
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
                        const int32  New = (Old < SourceToTarget.size()) ? SourceToTarget[Old] : INDEX_NONE;

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
            int32 FirstCompatible = INDEX_NONE;
            const FString TargetName = Import::SanitizeAssetName(AssetName.c_str());

            for (size_t i = 0; i < Data.Resources.size(); ++i)
            {
                const TUniquePtr<FMeshResource>& Resource = Data.Resources[i];
                if (!Resource || Resource->bSkinnedMesh != bWantSkinned)
                {
                    continue;
                }
                if (FirstCompatible == INDEX_NONE)
                {
                    FirstCompatible = (int32)i;
                }
                if (Import::SanitizeAssetName(Resource->Name.c_str()) == TargetName)
                {
                    return (int32)i;
                }
            }
            return FirstCompatible;
        }

        // Keyed off surface names so reordered materials keep their overrides, with index as the fallback.
        void RemapMaterialSlots(const FMeshResource& OldResource,
                                const TVector<TObjectPtr<CMaterialInterface>>& OldMaterials,
                                const FMeshResource& NewResource,
                                TVector<TObjectPtr<CMaterialInterface>>& OutMaterials)
        {
            const size_t SlotCount = CountMaterialSlots(NewResource);

            OutMaterials.clear();
            OutMaterials.resize(SlotCount);

            TVector<bool> bSlotResolved;
            bSlotResolved.resize(SlotCount, false);

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
                    if (OldSurface.MaterialIndex >= 0 && (size_t)OldSurface.MaterialIndex < OldMaterials.size())
                    {
                        OutMaterials[NewSlot] = OldMaterials[OldSurface.MaterialIndex];
                        bSlotResolved[NewSlot] = true;
                    }
                    break;
                }
            }

            for (size_t Slot = 0; Slot < SlotCount; ++Slot)
            {
                if (!bSlotResolved[Slot] && Slot < OldMaterials.size())
                {
                    OutMaterials[Slot] = OldMaterials[Slot];
                }
            }
        }

        // The only stage that expands instances, so the cost is paid once and only on merge.
        bool MergeInstancesIntoSingleMesh(FMeshImportData& Data, FString& OutError)
        {
            size_t TotalStaticVerts = 0, TotalStaticIndices = 0;
            size_t TotalSkinnedVerts = 0, TotalSkinnedIndices = 0;

            auto ResourceAt = [&Data](int32 Index) -> FMeshResource*
            {
                return (Index >= 0 && (size_t)Index < Data.Resources.size()) ? Data.Resources[Index].get() : nullptr;
            };

            for (const FSourceMeshInstance& Instance : Data.MeshInstances)
            {
                if (Instance.SlotIndex >= Data.MeshSlots.size())
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

            // Indices bake into a uint32 stream, so a merge past 4.29B vertices would wrap rather than fail.
            constexpr size_t MaxMergedVerts = (size_t)0xFFFFFFFFu;
            if (TotalStaticVerts > MaxMergedVerts || TotalSkinnedVerts > MaxMergedVerts)
            {
                OutError = FString(Format(
                    "Merging would flatten every instance into {0} static / {1} skinned vertices, past the {2} "
                    "vertex limit of the 32-bit index stream. Import with 'Merge Meshes' off to get one asset "
                    "per unique mesh instead.",
                    TotalStaticVerts, TotalSkinnedVerts, MaxMergedVerts).c_str());
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
                MergedStatic->ReserveVertices(TotalStaticVerts);
                MergedStatic->Indices.reserve(TotalStaticIndices);
            }
            if (TotalSkinnedVerts > 0)
            {
                MergedSkinned->ReserveVertices(TotalSkinnedVerts);
                MergedSkinned->Indices.reserve(TotalSkinnedIndices);
            }

            THashMap<int16, int16> SlotRemap;

            auto AppendInstance = [&](const FMeshResource& Source, FMeshResource& Target, const FMatrix4& World)
            {
                const FMatrix3 NormalMatrix = Math::Transpose(Math::Inverse(FMatrix3(World)));

                const size_t BaseVertex = Target.GetNumVertices();
                const size_t BaseIndex  = Target.GetNumIndices();
                const size_t VertexCount = Source.GetNumVertices();

                Target.ResizeVertices(BaseVertex + VertexCount);

                for (size_t i = 0; i < VertexCount; ++i)
                {
                    Target.Positions[BaseVertex + i] = FVector3(World * FVector4(Source.Positions[i], 1.0f));
                    Target.Normals[BaseVertex + i]   = PackNormal(Math::Normalize(NormalMatrix * UnpackNormal(Source.Normals[i])));
                    Target.UVs[BaseVertex + i]       = Source.UVs[i];
                    Target.UVs1[BaseVertex + i]      = Source.UVs1[i];
                    Target.Colors[BaseVertex + i]    = Source.Colors[i];
                }
                if (Target.bSkinnedMesh && Source.bSkinnedMesh)
                {
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        Target.JointIndices[BaseVertex + i] = Source.JointIndices[i];
                        Target.JointWeights[BaseVertex + i] = Source.JointWeights[i];
                    }
                }

                const size_t IndexCount = Source.GetNumIndices();
                Target.Indices.resize(BaseIndex + IndexCount);
                for (size_t i = 0; i < IndexCount; ++i)
                {
                    Target.Indices[BaseIndex + i] = Source.Indices[i] + (uint32)BaseVertex;
                }

                for (const FGeometrySurface& SourceSurface : Source.GeometrySurfaces)
                {
                    FGeometrySurface Surface = SourceSurface;
                    Surface.StartIndex += (uint32)BaseIndex;

                    // Without this every instance would add its own duplicate of the same source material.
                    if (SourceSurface.MaterialIndex >= 0)
                    {
                        auto It = SlotRemap.find(SourceSurface.MaterialIndex);
                        if (It == SlotRemap.end())
                        {
                            const int16 NewSlot = (int16)SlotRemap.size();
                            SlotRemap.emplace(SourceSurface.MaterialIndex, NewSlot);
                            Surface.MaterialIndex = NewSlot;
                        }
                        else
                        {
                            Surface.MaterialIndex = It->second;
                        }
                    }

                    Target.GeometrySurfaces.push_back(Surface);
                }
            };

            for (const FSourceMeshInstance& Instance : Data.MeshInstances)
            {
                if (Instance.SlotIndex >= Data.MeshSlots.size())
                {
                    continue;
                }
                const FSourceMeshSlot& Slot = Data.MeshSlots[Instance.SlotIndex];

                if (const FMeshResource* Static = ResourceAt(Slot.StaticResource))
                {
                    AppendInstance(*Static, *MergedStatic, Instance.WorldTransform);
                }
                if (const FMeshResource* Skinned = ResourceAt(Slot.SkinnedResource))
                {
                    AppendInstance(*Skinned, *MergedSkinned, Instance.WorldTransform);
                }
            }

            Data.MergedMaterialSlotToSource.assign(SlotRemap.size(), 0);
            for (const auto& Pair : SlotRemap)
            {
                if (Pair.second >= 0 && (size_t)Pair.second < Data.MergedMaterialSlotToSource.size())
                {
                    Data.MergedMaterialSlotToSource[Pair.second] = Pair.first;
                }
            }

            Data.Resources.clear();
            Data.MeshSlots.clear();
            Data.MeshInstances.clear();

            if (MergedStatic->GetNumVertices() > 0)  { Data.Resources.push_back(Move(MergedStatic)); }
            if (MergedSkinned->GetNumVertices() > 0) { Data.Resources.push_back(Move(MergedSkinned)); }

            return true;
        }

        // Flush uses an atomic_wait that would stall a worker fiber, so this must land on the main thread.
        void RunOnMainThread(const TFunction<void()>& Work)
        {
            if (Threading::IsMainThread())
            {
                Work();
                return;
            }

            TPromise<void> Promise;
            TFuture<void> Future = Promise.GetFuture();
            MainThread::Enqueue([&Work, Promise = Move(Promise)]() mutable
            {
                Work();
                Promise.SetValue();
            });
            Future.Wait();
        }
    }

    namespace
    {
        // Luminous efficacy at 555nm, the constant DCC exporters use to convert watts into lumens.
        constexpr float GLuminousEfficacy = 683.0f;

        /** Steradians in a sphere; a point light's candela is its lumens spread over all of them. */
        constexpr float GSphereSteradians = 4.0f * Math::Pi<float>();

        /** SPointLightComponent's own default, which is what a relative source's strongest lamp maps onto. */
        constexpr float GDefaultPunctualIntensity = 10.0f;

        /** A relative light's share of the brightest one of its kind in the same file. */
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

        /** The direction the source light emits in its node's local space, guarded against a degenerate one. */
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
        if (Nodes.empty())
        {
            return nullptr;
        }

        // Dropping the rest keeps an exporter's empty grouping nodes from becoming thousands of entities.
        TVector<uint8> bKeep(Nodes.size(), 0);
        for (size_t i = 0; i < Nodes.size(); ++i)
        {
            if (Nodes[i].Kind == ESourceNodeKind::Empty)
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
            for (int32 Walk = (int32)i; Walk != INDEX_NONE && bKeep[Walk] == 0; Walk = Nodes[Walk].ParentIndex)
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
        for (size_t i = 0; i < Nodes.size(); ++i)
        {
            if (bKeep[i] != 0 && Nodes[i].ParentIndex == INDEX_NONE)
            {
                ++RootCount;
            }
        }

        ECS::FEntity SharedRoot = ECS::NullEntity;
        if (RootCount != 1)
        {
            SharedRoot = MakeEntity(FName(FFixedString(PrefabName.data(), PrefabName.size())), FTransform());
        }

        TVector<ECS::FEntity> NodeEntities;
        NodeEntities.resize(Nodes.size(), ECS::NullEntity);

        // A directional light stores a world vector rather than deriving one from its transform.
        TVector<FQuat> WorldRotations(Nodes.size(), FQuat(1.0f, 0.0f, 0.0f, 0.0f));

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

        for (size_t i = 0; i < Nodes.size(); ++i)
        {
            const FSourceSceneNode& Node = Nodes[i];

            WorldRotations[i] = (Node.ParentIndex != INDEX_NONE)
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

            const ECS::FEntity Parent = (Node.ParentIndex != INDEX_NONE) ? NodeEntities[Node.ParentIndex] : SharedRoot;
            if (Parent != ECS::NullEntity)
            {
                ECS::Utils::AddToParent(Registry, Entity, Parent);
            }

            switch (Node.Kind)
            {
            case ESourceNodeKind::Mesh:
                {
                    if (Node.MeshSlot < 0 || (size_t)Node.MeshSlot >= SourceData.MeshSlots.size())
                    {
                        break;
                    }
                    const FSourceMeshSlot& Slot = SourceData.MeshSlots[Node.MeshSlot];

                    auto MeshAt = [&ResourceToMesh](int32 Index) -> CMesh*
                    {
                        return (Index >= 0 && (size_t)Index < ResourceToMesh.size()) ? ResourceToMesh[Index] : nullptr;
                    };

                    if (CStaticMesh* Static = Cast<CStaticMesh>(MeshAt(Slot.StaticResource)))
                    {
                        Registry.Emplace<SStaticMeshComponent>(Entity).StaticMesh = Static;
                    }
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
        }

        if (bCreateSceneEnvironment)
        {
            // The instantiation root is the one entity guaranteed to exist for the life of the instance.
            ECS::FEntity EnvironmentRoot = SharedRoot;
            if (EnvironmentRoot == ECS::NullEntity)
            {
                for (size_t i = 0; i < Nodes.size(); ++i)
                {
                    if (NodeEntities[i] != ECS::NullEntity && Nodes[i].ParentIndex == INDEX_NONE)
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
                 PackagePath.c_str(), KeptCount + (SharedRoot != ECS::NullEntity ? 1 : 0), Nodes.size(), LightsImported,
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
        // User transforms and heavy passes are deferred to BuildAssets, so a setting change never re-parses.
        FMeshImportOptions PreviewOptions;
        PreviewOptions.bOptimize         = false;
        PreviewOptions.bMergeMeshes      = false;
        PreviewOptions.bFlipNormals      = false;
        PreviewOptions.bFlipUVs          = false;
        PreviewOptions.Scale             = 1.0f;
        PreviewOptions.bSkipFinalization = true;

        SourceData = FMeshImportData();
        return ParseMeshSource(Request, PreviewOptions, SourceData, OutError, Progress);
    }

    void CMeshImporter::PrepareSettingsPreview()
    {
        if (SourceData.Images.empty())
        {
            return;
        }

        Task::ParallelFor((uint32)SourceData.Images.size(), [this](uint32 Index)
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
            else if (!Image.ResolvedPath.empty())
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
        if (bMergeMeshes && !SourceData.MeshInstances.empty())
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
        const size_t LastSlashPos = Request.DestinationPath.find_last_of('/');
        if (LastSlashPos == FFixedString::npos)
        {
            BaseName = Request.DestinationPath;
        }
        else
        {
            DestinationDir = Request.DestinationPath.substr(0, LastSlashPos + 1);
            BaseName       = Request.DestinationPath.substr(LastSlashPos + 1, FFixedString::npos);
        }
        if (const size_t DotPos = BaseName.find_last_of('.'); DotPos != FFixedString::npos)
        {
            BaseName = BaseName.substr(0, DotPos);
        }

        FFixedString TexturesDir  = DestinationDir; TexturesDir.append("Textures/");
        FFixedString MaterialsDir = DestinationDir; MaterialsDir.append("Materials/");

        FUniquePathAllocator Paths;
        auto BuildPath = [&](FStringView Suffix) -> FFixedString
        {
            FFixedString Path = DestinationDir;
            Path.append(BaseName);
            if (!Suffix.empty())
            {
                Path.append("_");
                Path.append(Suffix.data(), Suffix.length());
            }
            return Paths.Claim(Path);
        };

        if (Progress)
        {
            Progress->EnterProgressFrame(kCreateBudget, "Creating assets...");
        }

        TVector<CObject*>& CreatedObjects = OutResult.CreatedObjects;
        CreatedObjects.reserve(SourceData.Skeletons.size() + SourceData.Resources.size()
                             + SourceData.Animations.size() + SourceData.Images.size());

        // Indexed by Resources index, so the prefab can resolve a scene node's mesh slot to its asset.
        TVector<CMesh*> ResourceToMesh(SourceData.Resources.size(), nullptr);

        TObjectPtr<CSkeleton> PrimarySkeleton;
        const bool bMultipleSkeletons = SourceData.Skeletons.size() > 1;

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

        for (size_t i = 0; bImportSkeleton && bFileSkeletonNeeded && i < SourceData.Skeletons.size(); ++i)
        {
            TUniquePtr<FSkeletonResource>& SkeletonRes = SourceData.Skeletons[i];
            if (!SkeletonRes || !SkeletonRes->bShouldImport)
            {
                continue;
            }

            const FFixedString SkeletonPath = bMultipleSkeletons ? BuildPath(SkeletonRes->Name.ToString()) : BuildPath("Skeleton");

            CSkeleton* NewSkeleton = CFactory::CreateNewOf<CSkeleton>(SkeletonPath);
            NewSkeleton->SetFlag(OF_NeedsPostLoad);
            NewSkeleton->SkeletonResource = Move(SkeletonRes);

            if (!PrimarySkeleton)
            {
                PrimarySkeleton = NewSkeleton;
            }
            CreatedObjects.push_back(NewSkeleton);
        }

        // Merge mode densified globally at parse time, so everything else is packed here alongside ResourceToMesh.
        TVector<TVector<int16>> ResourceSlotToSource(SourceData.Resources.size());
        if (!bMergeMeshes)
        {
            for (size_t i = 0; i < SourceData.Resources.size(); ++i)
            {
                if (SourceData.Resources[i])
                {
                    ResourceSlotToSource[i] = DensifyMaterialSlots(*SourceData.Resources[i]);
                }
            }
        }

        const bool bMultipleMeshes = SourceData.Resources.size() > 1;
        for (size_t i = 0; bImportMeshes && i < SourceData.Resources.size(); ++i)
        {
            TUniquePtr<FMeshResource>& MeshResource = SourceData.Resources[i];
            if (!MeshResource)
            {
                continue;
            }

            const FFixedString MeshPath = bMultipleMeshes ? BuildPath(MeshResource->Name.ToString()) : BuildPath({});

            CMesh* NewMesh = nullptr;
            if (!MeshResource->bSkinnedMesh)
            {
                NewMesh = CFactory::CreateNewOf<CStaticMesh>(MeshPath);
            }
            else
            {
                CSkeletalMesh* NewSkeletalMesh = CFactory::CreateNewOf<CSkeletalMesh>(MeshPath);

                CSkeleton* MeshSkeleton = bBoundToTargetSkeleton ? TargetSkeleton.Get() : PrimarySkeleton.Get();
                if (MeshSkeleton != nullptr)
                {
                    NewSkeletalMesh->Skeleton = MeshSkeleton;

                    // Only ever on a skeleton this import owns; the target belongs to another asset.
                    if (MeshSkeleton == PrimarySkeleton.Get() && !PrimarySkeleton->PreviewMesh)
                    {
                        PrimarySkeleton->PreviewMesh = NewSkeletalMesh;
                    }
                }
                NewMesh = NewSkeletalMesh;
            }

            NewMesh->SetFlag(OF_NeedsPostLoad);
            NewMesh->Materials.clear();
            NewMesh->Materials.resize(CountMaterialSlots(*MeshResource));
            NewMesh->SourcePath            = FString(Request.SourcePath.c_str());
            NewMesh->DistanceFieldSettings = DistanceField;
            NewMesh->MeshResources         = Move(MeshResource);

            // Bake the exact box now, while the raw float positions are still around to measure.
            NewMesh->GenerateBoundingBox();

            CreatedObjects.push_back(NewMesh);
            ResourceToMesh[i] = NewMesh;
        }

        const bool bMultipleAnims = SourceData.Animations.size() > 1;
        for (size_t i = 0; bImportAnimations && i < SourceData.Animations.size(); ++i)
        {
            TUniquePtr<FAnimationResource>& Clip = SourceData.Animations[i];
            if (!Clip)
            {
                continue;
            }

            AnimCompression::Build(*Clip);

            const FFixedString AnimPath = bMultipleAnims ? BuildPath(Clip->Name.ToString()) : BuildPath("Animation");

            CAnimation* NewAnimation = CFactory::CreateNewOf<CAnimation>(AnimPath);
            NewAnimation->SetFlag(OF_NeedsPostLoad);
            NewAnimation->AnimationResource = Move(Clip);
            NewAnimation->Skeleton          = TargetSkeleton != nullptr ? TargetSkeleton : PrimarySkeleton;

            CreatedObjects.push_back(NewAnimation);
        }

        FScopedAssetRegistryBatch RegistryBatch;

        // Resolved by IMAGE INDEX, so binding a channel is an array lookup rather than a path hash.
        TVector<CTexture*> ImageAssets(SourceData.Images.size(), nullptr);

        const bool bWantTextures = bImportTextures || bImportMaterials;
        if (bWantTextures && !SourceData.Images.empty())
        {
            if (Progress)
            {
                Progress->UpdateMessage("Importing textures...");
            }

            struct FTextureWork
            {
                FFixedString PackagePath;
                uint32       ImageIndex;
                bool         bNeedsCook;
            };

            TVector<FTextureWork> Work;
            Work.reserve(SourceData.Images.size());

            for (size_t i = 0; i < SourceData.Images.size(); ++i)
            {
                const FSourceImage& Image = SourceData.Images[i];

                const FStringView NameSource = Image.ResolvedPath.empty()
                    ? FStringView(Image.Key.c_str(), Image.Key.size())
                    : VFS::FileName(Image.ResolvedPath, true);

                FFixedString PackagePath = Paths::Combine(TexturesDir, TextureAssetName(NameSource).c_str());

                const bool bAlreadyExists = (FindObject<CPackage>(PackagePath) != nullptr);

                // Images are deduplicated by key already, so a name collision here is always two different textures.
                if (!bAlreadyExists)
                {
                    PackagePath = Paths.Claim(PackagePath);
                }

                CPackage::AddPackageExt(PackagePath);

                Work.push_back(FTextureWork{ Move(PackagePath), (uint32)i, !bAlreadyExists });
            }

            TVector<CTexture*> Cooked(Work.size(), nullptr);

            Task::ParallelFor((uint32)Work.size(), [&](uint32 i)
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
                CookRequest.SourcePath         = Image.ResolvedPath.empty() ? Image.Key : Image.ResolvedPath;
                CookRequest.ColorSpace         = Image.IntendedColorSpace;
                // This loop already saturates the cores, so a full basisu pool per texture would oversubscribe.
                CookRequest.EncodeThreadBudget = 1;
                // Creating a GPU image here would queue a copy against an image this import destroys first.
                CookRequest.bCreateGPUResource  = false;

                Cooked[i] = Import::Textures::ImportTextureAsset(W.PackagePath, CookRequest);
            }, 1);

            for (size_t i = 0; i < Work.size(); ++i)
            {
                const FTextureWork& W = Work[i];
                CTexture* Texture = Cooked[i];
                if (Texture != nullptr)
                {
                    CreatedObjects.push_back(Texture);
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

        if (bImportMaterials && !SourceData.Materials.empty())
        {
            if (Progress)
            {
                Progress->UpdateMessage("Generating materials...");
            }

            RunOnMainThread([&]()
            {
                const TVector<CMaterialInstance*> Instances = Import::Materials::GenerateMaterials(
                    TSpan<const FMeshImportMaterial>(SourceData.Materials.data(), SourceData.Materials.size()),
                    TSpan<CTexture* const>(ImageAssets.data(), ImageAssets.size()),
                    MaterialsDir, BaseName, CreatedObjects, SourceData.bHasVertexColors);

                // Each of the four ways a surface ends up materialless is counted, since the symptom is identical.
                uint32 SurfacesTotal = 0, NoAssignment = 0, OutOfRange = 0, NoSource = 0, NoInstance = 0;

                for (size_t ResIdx = 0; ResIdx < ResourceToMesh.size(); ++ResIdx)
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
                        if (!SlotToSource.empty())
                        {
                            SourceIndex = (Slot < (int32)SlotToSource.size()) ? SlotToSource[Slot] : INDEX_NONE;
                        }

                        if (SourceIndex < 0 || (size_t)SourceIndex >= Instances.size())
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
                             bMergeMeshes, (uint32)SourceData.Materials.size(), (uint32)Instances.size());
                }
                else if (Unresolved > 0)
                {
                    LOG_INFO("[Import] {}/{} surfaces have no material because the source assigned none.",
                             Unresolved, SurfacesTotal);
                }
            });
        }
        else if (bImportMeshes)
        {
            // Being skipped here lands every mesh with an all-null Materials array, so say which gate closed.
            LOG_WARN("[Import] no materials were generated: Import Materials is {}, and the source parsed "
                     "{} material(s). Every imported mesh will have empty material slots.",
                     bImportMaterials ? "ON" : "OFF", (uint32)SourceData.Materials.size());
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
        if (bImportAsPrefab && !bMergeMeshes && !SourceData.SceneNodes.empty())
        {
            if (Progress)
            {
                Progress->UpdateMessage("Building scene prefab...");
            }

            const FFixedString PrefabPath = BuildPath("Prefab");
            if (CPrefab* Prefab = BuildScenePrefab(PrefabPath, FStringView(BaseName.c_str(), BaseName.size()), ResourceToMesh))
            {
                CreatedObjects.push_back(Prefab);
            }
        }

        if (Progress)
        {
            Progress->UpdateMessage("Saving packages...");
        }

        const float SaveStep = kSaveBudget / (float)std::max<size_t>((size_t)1, CreatedObjects.size());
        for (CObject* Object : CreatedObjects)
        {
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
                LOG_ERROR("[Import] failed to save {}; asset will not be registered", Package->GetPackagePath());
            }

            if (Progress)
            {
                Progress->EnterProgressFrame(SaveStep);
            }
        }
        if (CreatedObjects.empty() && Progress)
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
        if (ResourceIndex == INDEX_NONE)
        {
            LOG_ERROR("Reimport: '{0}' contains no {1} mesh to replace '{2}' with.",
                      Request.SourcePath.c_str(), Mesh->IsSkinned() ? "skinned" : "static", Mesh->GetName().c_str());
            return false;
        }

        TUniquePtr<FMeshResource>& NewResource = SourceData.Resources[ResourceIndex];
        if (!NewResource || NewResource->GeometrySurfaces.empty())
        {
            LOG_ERROR("Reimport: the mesh selected from '{0}' has no surfaces; leaving '{1}' untouched.",
                      Request.SourcePath.c_str(), Mesh->GetName().c_str());
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

        TVector<TObjectPtr<CMaterialInterface>> RemappedMaterials;
        RemapMaterialSlots(Mesh->GetMeshResource(), Mesh->Materials, *NewResource, RemappedMaterials);
        Mesh->Materials = Move(RemappedMaterials);

        // SetMeshResource rebuilds bounds and buffers and invalidates the resolve cache, so components follow.
        Mesh->SetMeshResource(Move(NewResource));
        Mesh->SourcePath = FString(Request.SourcePath.c_str());

        if (CPackage* Package = Mesh->GetPackage())
        {
            Package->MarkDirty();
        }

        return true;
    }

    void CMeshImporter::DrawSourcePreview()
    {
        auto Section = [](auto&& Draw)
        {
            Draw();
            ImGui::Spacing();
            ImGui::Spacing();
        };

        Section([&]
        {
            if (SourceData.Resources.empty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Meshes ({})###MeshStats", SourceData.Resources.size());
            if (!ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            if (SourceData.SourceNodeCount > SourceData.Resources.size())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
                ImGui::Text("%u source nodes collapsed to %zu unique meshes",
                            SourceData.SourceNodeCount, SourceData.Resources.size());
                ImGui::PopStyleColor();
            }

            constexpr ImGuiTableFlags Flags =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

            const float Height = std::min<float>(180.0f, (SourceData.Resources.size() + 1) * ImGui::GetTextLineHeightWithSpacing() + 8.0f);
            if (ImGui::BeginTable("MeshStatsTable", 4, Flags, ImVec2(0, Height)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Verts",    ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Indices",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Surfaces", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();

                ImGuiListClipper Clipper;
                Clipper.Begin((int)SourceData.Resources.size());
                while (Clipper.Step())
                {
                    for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                    {
                        const FMeshResource& Resource = *SourceData.Resources[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(Resource.Name.c_str());
                        ImGui::TableNextColumn(); ImGuiX::Text("{0}", ImGuiX::FormatSize(Resource.GetNumVertices()));
                        ImGui::TableNextColumn(); ImGuiX::Text("{0}", ImGuiX::FormatSize(Resource.Indices.size()));
                        ImGui::TableNextColumn(); ImGuiX::Text("{0}", Resource.GeometrySurfaces.size());
                    }
                }
                ImGui::EndTable();
            }
        });

        Section([&]
        {
            if (SourceData.SceneNodes.empty())
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
                                                               SourceData.SceneNodes.size(), Lights);
            if (!ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
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
            if (SourceData.Images.empty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Textures ({})###Textures", SourceData.Images.size());
            if (!ImGui::CollapsingHeader(Header.c_str()))
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
                Clipper.Begin((int)SourceData.Images.size(), ThumbSize + 8);
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
            if (SourceData.Skeletons.empty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Skeletons ({})###Skeletons", SourceData.Skeletons.size());
            if (!ImGui::CollapsingHeader(Header.c_str()))
            {
                return;
            }

            const FSkeletonResource* MeshTarget = TargetSkeleton != nullptr ? TargetSkeleton->GetSkeletonResource() : nullptr;
            if (MeshTarget != nullptr && !SourceData.Skeletons.empty() && SourceData.Skeletons[0])
            {
                const FSkeletonResource& FileSkeleton = *SourceData.Skeletons[0];

                int32 Matched = 0;
                for (const FSkeletonResource::FBoneInfo& Bone : FileSkeleton.Bones)
                {
                    Matched += MeshTarget->FindBoneIndex(Bone.Name) >= 0 ? 1 : 0;
                }

                ImGui::TextDisabled("Skinned meshes bind to %s; no skeleton is created from this file.",
                                    TargetSkeleton->GetName().c_str());

                if (Matched == (int32)FileSkeleton.Bones.size())
                {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f),
                        LE_ICON_CHECK_CIRCLE_OUTLINE " Every bone matches by name.");
                }
                else if (Matched > 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                        LE_ICON_ALERT_CIRCLE_OUTLINE " %d of %zu bones match; unmatched influences are dropped.",
                        Matched, FileSkeleton.Bones.size());
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
                if (ImGui::TreeNodeEx(Skeleton->Name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    auto DrawBone = [&](int32 BoneIdx, auto& Self) -> void
                    {
                        const FSkeletonResource::FBoneInfo& Bone = Skeleton->Bones[BoneIdx];
                        TVector<int32> Children = Skeleton->GetChildBones(BoneIdx);
                        const ImGuiTreeNodeFlags Flags = Children.empty()
                            ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                            : ImGuiTreeNodeFlags_None;
                        if (ImGui::TreeNodeEx(Bone.Name.c_str(), Flags))
                        {
                            for (int32 ChildIdx : Children)
                            {
                                Self(ChildIdx, Self);
                            }
                            if (!Children.empty())
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
            if (SourceData.Animations.empty())
            {
                return;
            }

            const FFixedString Header = FormatAs<FFixedString>("Animations ({})###Animations", SourceData.Animations.size());
            if (!ImGui::CollapsingHeader(Header.c_str()))
            {
                return;
            }

            const FSkeletonResource* Target = TargetSkeleton != nullptr ? TargetSkeleton->GetSkeletonResource() : nullptr;
            if (TargetSkeleton != nullptr)
            {
                ImGui::TextDisabled("Binding to %s", TargetSkeleton->GetName().c_str());
            }

            for (size_t i = 0; i < SourceData.Animations.size(); ++i)
            {
                const FAnimationResource& Anim = *SourceData.Animations[i];
                ImGui::PushID((int)i);
                if (ImGui::TreeNodeEx(Anim.Name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    ImGui::TextDisabled("Duration: %.2fs   Channels: %zu", Anim.Duration, Anim.Channels.size());

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
                                LE_ICON_ALERT_CIRCLE_OUTLINE " %d of %zu channels have no bone of that name.",
                                Unmatched, Anim.Channels.size());
                        }
                    }

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        });
    }
}
