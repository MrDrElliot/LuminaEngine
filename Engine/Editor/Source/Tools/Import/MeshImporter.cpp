#include "EditorPCH.h"
#include "MeshImporter.h"

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

namespace Lumina
{
    using namespace Import::Mesh;

    namespace
    {
        // Destination asset name: <Prefix><clean stem>, directory and extension stripped.
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

        // Package paths an import is about to mint. FindObject only sees what is already loaded, so a run
        // that creates several assets under one directory needs its own claim set to stay collision-free.
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
                    Candidate.append("_").append_convert(eastl::to_string(N));
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
                    SlotCount = eastl::max(SlotCount, (size_t)Surface.MaterialIndex + 1);
                }
            }
            return bAnyExplicit ? SlotCount : Resource.GeometrySurfaces.size();
        }

        /** Rewrites a resource's surface material indices into a dense per-mesh slot range and returns the
         *  slot -> source mapping. A parser indexes by position in the WHOLE file, leaving dangling slots. */
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

        // Which parsed sub-mesh replaces the asset on reimport. Prefers a name match (survives reordered
        // exports) and falls back to the first compatible resource.
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

        // Carries old material assignments onto new slots, keyed off surface names so a source whose
        // materials were reordered keeps its overrides; index is the fallback.
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

        // Flattens every scene-graph placement into one static/skinned pair, baking each instance's world
        // transform in. The only stage that expands instances, so the cost is paid once and only on merge.
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

            // Vertex indices are baked into a uint32 stream, so a merge past 4.29B vertices would silently
            // wrap and scramble the geometry rather than fail.
            constexpr size_t MaxMergedVerts = (size_t)0xFFFFFFFFu;
            if (TotalStaticVerts > MaxMergedVerts || TotalSkinnedVerts > MaxMergedVerts)
            {
                OutError = FString(std::format(
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

            // Measured first, allocated exactly once: growing these streams per instance would memcpy the
            // whole buffer set repeatedly and hold two allocations at once mid-realloc.
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

                    // Primitives sharing a source material collapse onto one slot; without this every
                    // instance would add its own duplicate of the same material.
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

        // Material generation compiles shaders through GShaderCompiler->Flush(), whose atomic_wait would
        // stall a worker fiber, so it has to land on the main thread.
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
        /** Luminous efficacy at 555nm. The constant DCC exporters use converting watts -> lumens, so
         *  dividing by it recovers the radiometric value the scene was actually authored with. */
        constexpr float GLuminousEfficacy = 683.0f;

        /** Steradians in a sphere; a point light's candela is its lumens spread over all of them. */
        constexpr float GSphereSteradians = 4.0f * Math::Pi<float>();

        /** Authors the environment an imported scene cannot carry itself. File-local so the importer header
         *  does not have to pull in entt or the component headers. */
        void AddSceneEnvironment(entt::registry& Registry, entt::entity Root, const FVector3& WorldColor)
        {
            // A flat fill rather than the Dynamic default: the source authored a constant world color, and
            // a procedural atmosphere would introduce a sun and a sky gradient it never asked for.
            SEnvironmentComponent& Environment = Registry.emplace<SEnvironmentComponent>(Root);
            Environment.bRenderSky   = true;
            Environment.SkyMode      = ESkyMode::SolidColor;
            Environment.SolidSkyColor = WorldColor;

            // The world doubles as the ambient fill, which is what it does in a DCC. Intensity carries the
            // magnitude so the color stays a hue, and the component clamps it to [0,1].
            const float Ambient = Math::Max(WorldColor.x, Math::Max(WorldColor.y, WorldColor.z));
            SSkyLightComponent& SkyLight = Registry.emplace<SSkyLightComponent>(Root);
            SkyLight.bAffectsWorld  = true;
            SkyLight.bAmbientFromSky = false;
            SkyLight.AmbientColor   = (Ambient > 0.0f) ? (WorldColor / Ambient) : FVector3(1.0f);
            SkyLight.Intensity      = Math::Clamp(Ambient, 0.0f, 1.0f);

            // An identity grade. Without this the prefab inherits whatever the host world grades with, and
            // a default Lumina world ships a deliberately art-directed one.
            SPostProcessComponent& PostProcess = Registry.emplace<SPostProcessComponent>(Root);
            PostProcess.bEnabled        = true;
            PostProcess.bInfiniteExtent = true;
            // A default world ships its own global volume at priority 0 and ties resolve by iteration order.
            // Outranking it is the difference between the source's look and the editor's.
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

    float CMeshImporter::ConvertDirectionalIntensity(float SourceIntensity) const
    {
        if (LightUnits == ELightImportUnits::Raw)
        {
            return SourceIntensity * LightIntensityScale;
        }

        // lux -> W/m^2 -> engine units.
        return (SourceIntensity / GLuminousEfficacy) * DirectionalLightCalibration * LightIntensityScale;
    }

    float CMeshImporter::ConvertPunctualIntensity(float SourceIntensity) const
    {
        if (LightUnits == ELightImportUnits::Raw)
        {
            return SourceIntensity * LightIntensityScale;
        }

        // candela -> lumens -> watts -> engine units.
        const float Watts = (SourceIntensity * GSphereSteradians) / GLuminousEfficacy;
        return Watts * PunctualLightCalibration * LightIntensityScale;
    }

    CPrefab* CMeshImporter::BuildScenePrefab(const FFixedString& PackagePath, FStringView PrefabName,
                                             const TVector<CMesh*>& ResourceToMesh) const
    {
        const TVector<FSourceSceneNode>& Nodes = SourceData.SceneNodes;
        if (Nodes.empty())
        {
            return nullptr;
        }

        // A node earns an entity by carrying something, or by sitting on the path to one: dropping the rest
        // keeps an exporter's empty grouping nodes from becoming thousands of empty entities.
        TVector<uint8> bKeep(Nodes.size(), 0);
        for (size_t i = 0; i < Nodes.size(); ++i)
        {
            if (Nodes[i].Kind == ESourceNodeKind::Empty)
            {
                continue;
            }
            // Cameras are always parsed (the dialogue runs after the parse), so a declined camera import
            // has to drop them here rather than leave a bare entity behind.
            if (Nodes[i].Kind == ESourceNodeKind::Camera && !bImportCameras)
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

        entt::registry& Registry = Prefab->Registry;

        auto MakeEntity = [&Registry](const FName& Name, const FTransform& Transform) -> entt::entity
        {
            const entt::entity Entity = Registry.create();
            Registry.emplace<SNameComponent>(Entity, Name);
            Registry.emplace<STransformComponent>(Entity, Transform);
            Registry.emplace<SPrefabComponent>(Entity).StableID = FName(FGuid::New().ToShortString());
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

        entt::entity SharedRoot = entt::null;
        if (RootCount != 1)
        {
            SharedRoot = MakeEntity(FName(FFixedString(PrefabName.data(), PrefabName.size())), FTransform());
        }

        TVector<entt::entity> NodeEntities;
        NodeEntities.resize(Nodes.size(), entt::null);

        // World rotations accumulate parents-first in the same pass; a directional light stores a world
        // vector rather than deriving one from its transform.
        TVector<FQuat> WorldRotations(Nodes.size(), FQuat(1.0f, 0.0f, 0.0f, 0.0f));

        bool bClaimedActiveCamera = false;

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
                // A glTF camera looks down local -Z; the engine's camera forward is the entity's +Z. Yawing 180
                // makes the imported orientation frame the same view instead of the exact opposite one.
                Transform.SetRotation(Node.Rotation * FQuat(FVector3(0.0f, Math::Pi<float>(), 0.0f)));
            }
            else
            {
                Transform.SetRotation(Node.Rotation);
            }

            const entt::entity Entity = MakeEntity(Node.Name, Transform);
            NodeEntities[i] = Entity;

            const entt::entity Parent = (Node.ParentIndex != INDEX_NONE) ? NodeEntities[Node.ParentIndex] : SharedRoot;
            if (Parent != entt::null)
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
                        Registry.emplace<SStaticMeshComponent>(Entity).StaticMesh = Static;
                    }
                    if (CSkeletalMesh* Skinned = Cast<CSkeletalMesh>(MeshAt(Slot.SkinnedResource)))
                    {
                        Registry.emplace<SSkeletalMeshComponent>(Entity).SkeletalMesh = Skinned;
                    }
                    break;
                }

            case ESourceNodeKind::PointLight:
                {
                    SPointLightComponent& Light = Registry.emplace<SPointLightComponent>(Entity);
                    Light.LightColor = Node.Light.Color;
                    Light.Intensity  = ConvertPunctualIntensity(Node.Light.Intensity);
                    // glTF range 0 means unbounded, which a clustered renderer cannot express; the
                    // component default is the finite stand-in.
                    if (Node.Light.Range > 0.0f)
                    {
                        Light.Attenuation = Node.Light.Range;
                    }
                    break;
                }

            case ESourceNodeKind::SpotLight:
                {
                    SSpotLightComponent& Light = Registry.emplace<SSpotLightComponent>(Entity);
                    Light.LightColor      = Node.Light.Color;
                    Light.Intensity       = ConvertPunctualIntensity(Node.Light.Intensity);
                    Light.InnerConeAngle  = Math::Degrees(Node.Light.InnerConeAngle);
                    Light.OuterConeAngle  = Math::Degrees(Node.Light.OuterConeAngle);
                    if (Node.Light.Range > 0.0f)
                    {
                        Light.Attenuation = Node.Light.Range;
                    }
                    break;
                }

            case ESourceNodeKind::DirectionalLight:
                {
                    SDirectionalLightComponent& Light = Registry.emplace<SDirectionalLightComponent>(Entity);
                    Light.Color     = Node.Light.Color;
                    Light.Intensity = ConvertDirectionalIntensity(Node.Light.Intensity);
                    // glTF lights emit along -Z; the engine stores the TO-LIGHT vector, which is +Z.
                    Light.Direction = Math::Normalize(Math::Rotate(WorldRotations[i], FVector3(0.0f, 0.0f, 1.0f)));
                    break;
                }

            case ESourceNodeKind::Camera:
                {
                    // A camera node kept only because a mesh hangs off it still reaches this switch.
                    if (!bImportCameras)
                    {
                        break;
                    }

                    SCameraComponent& Camera = Registry.emplace<SCameraComponent>(Entity);
                    Camera.FOV       = Math::Degrees(Node.Camera.YFov);
                    Camera.NearPlane = Node.Camera.ZNear;
                    if (Node.Camera.ZFar > Node.Camera.ZNear)
                    {
                        Camera.FarPlane = Node.Camera.ZFar;
                    }

                    // Only the first camera claims activation, so a multi-camera source does not fight
                    // over which one wins on spawn.
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
            // The environment belongs on whichever entity is the instantiation root, since that is the one
            // guaranteed to exist for the life of the instance.
            entt::entity EnvironmentRoot = SharedRoot;
            if (EnvironmentRoot == entt::null)
            {
                for (size_t i = 0; i < Nodes.size(); ++i)
                {
                    if (NodeEntities[i] != entt::null && Nodes[i].ParentIndex == INDEX_NONE)
                    {
                        EnvironmentRoot = NodeEntities[i];
                        break;
                    }
                }
            }

            if (EnvironmentRoot != entt::null)
            {
                AddSceneEnvironment(Registry, EnvironmentRoot, WorldColor);
            }
        }

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
        // The preview parse is deliberately neutral: user transforms and every heavy pass are deferred to
        // BuildAssets, so changing a setting in the dialogue never costs a re-parse.
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

    void CMeshImporter::BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress)
    {
        FMeshImportOptions Options = BuildOptions(false);

        constexpr float kFinalizeBudget = 0.72f;
        constexpr float kCreateBudget   = 0.05f;
        constexpr float kTextureBudget  = 0.13f;
        constexpr float kSaveBudget     = 0.10f;

        // A source with a scene graph merges through the instance table, so each placement's world transform
        // is baked and the layout survives. FBX and OBJ fall through to plain concatenation.
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
                Path.append_convert(Suffix.data(), Suffix.length());
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

        // With a target skeleton the file's own is only still needed to bind skinned meshes, whose weights
        // are bone INDICES into it. Nothing else would reference it, so skip minting a duplicate.
        bool bFileSkeletonNeeded = TargetSkeleton == nullptr;
        if (!bFileSkeletonNeeded && bImportMeshes)
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

        // Slot -> source-material mapping per resource. Merge mode densified globally at parse time and
        // carries its own table; everything else is packed here, indexed alongside ResourceToMesh.
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
                if (PrimarySkeleton)
                {
                    NewSkeletalMesh->Skeleton = PrimarySkeleton;
                    if (!PrimarySkeleton->PreviewMesh)
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

            const FFixedString AnimPath = bMultipleAnims ? BuildPath(Clip->Name.ToString()) : BuildPath("Animation");

            CAnimation* NewAnimation = CFactory::CreateNewOf<CAnimation>(AnimPath);
            NewAnimation->SetFlag(OF_NeedsPostLoad);
            NewAnimation->AnimationResource = Move(Clip);
            NewAnimation->Skeleton          = TargetSkeleton != nullptr ? TargetSkeleton : PrimarySkeleton;

            CreatedObjects.push_back(NewAnimation);
        }

        FScopedAssetRegistryBatch RegistryBatch;

        // Textures are resolved by IMAGE INDEX, so binding a material channel is an array lookup rather
        // than a hash of a path string that was rebuilt for every slot.
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
            THashSet<FFixedString> SeenPaths;

            for (size_t i = 0; i < SourceData.Images.size(); ++i)
            {
                const FSourceImage& Image = SourceData.Images[i];

                const FStringView NameSource = Image.ResolvedPath.empty()
                    ? FStringView(Image.Key.c_str(), Image.Key.size())
                    : VFS::FileName(Image.ResolvedPath, true);

                FFixedString PackagePath = Paths::Combine(TexturesDir, TextureAssetName(NameSource).c_str());

                const bool bAlreadyExists = (FindObject<CPackage>(PackagePath) != nullptr);

                // TextureAssetName drops directory and extension, so two source images can sanitize to one name.
                // Images are already deduplicated by key, so a collision here is always two different textures.
                if (!bAlreadyExists && !SeenPaths.insert(PackagePath).second)
                {
                    PackagePath = Paths.Claim(PackagePath);
                    SeenPaths.insert(PackagePath);
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
                // Embedded payloads have no file; the key still names them for the color-space heuristic
                // and is not persisted onto the asset.
                CookRequest.SourcePath         = Image.ResolvedPath.empty() ? Image.Key : Image.ResolvedPath;
                CookRequest.ColorSpace         = Image.IntendedColorSpace;
                // One encode thread each: this loop already saturates the cores, so a full basisu pool per
                // texture would only oversubscribe them.
                CookRequest.EncodeThreadBudget = 1;
                // CPU mips only. Nothing renders these during import and PostLoad creates the image on first use,
                // whereas creating one here queues a copy against an image this import destroys first.
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

        // How many null slots the assignment below knows about. The sweep after it compares against this,
        // so it only speaks up when the assets disagree with what assignment thought it did.
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

                // Every mesh's slots are dense now, so each needs its OWN slot -> source table: merge mode's
                // global one, or the per-resource one built before the assets were created.
                //
                // Each of the four ways a surface can come out of here with no material is counted rather
                // than passed over, because they have different causes and the symptom is identical: an
                // untextured import. NoSource/OutOfRange mean the slot tables and the surfaces disagree;
                // NoInstance means the material itself never got generated (see the [MaterialImport] errors
                // above it); Unassigned means the source primitive genuinely had no material.
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

                // A primitive the source left unmaterialed is not a fault, and a warning that fires on
                // every healthy import is one nobody reads. Only the three that mean something broke --
                // the slot tables disagreeing with the surfaces, or a material that failed to generate --
                // raise the severity.
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
            // The generation block is the ONLY thing that fills a mesh's slots, so being skipped here means
            // every imported mesh lands with an all-null Materials array. Silent until now, and identical
            // in the browser to a generation that ran and failed -- which is why it says which gate closed.
            LOG_WARN("[Import] no materials were generated: Import Materials is {}, and the source parsed "
                     "{} material(s). Every imported mesh will have empty material slots.",
                     bImportMaterials ? "ON" : "OFF", (uint32)SourceData.Materials.size());
            ExpectedNullSlots = UINT32_MAX;
        }

        // Final state of the assets as saved, counted from the meshes themselves rather than from the
        // assignment loop's bookkeeping. Reported only when the two disagree, which is the one thing the
        // assignment loop cannot see for itself: a slot that was filled and then cleared again.
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

        // Built last: it references the mesh assets above and their materials, so everything it points at
        // exists. Merging already flattened the scene, which is the opposite of what a prefab preserves.
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

        const float SaveStep = kSaveBudget / (float)eastl::max<size_t>((size_t)1, CreatedObjects.size());
        for (CObject* Object : CreatedObjects)
        {
            CPackage* Package = Object->GetPackage();
            if (CPackage::SavePackage(Package, Package->GetPackagePath()))
            {
                // A generated material node graph rides along in its master's package but is not itself a
                // browsable asset.
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

    bool CMeshImporter::ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress)
    {
        CMesh* Mesh = Cast<CMesh>(Asset);
        if (Mesh == nullptr)
        {
            return false;
        }

        // Only the geometry matters: reimport replaces one asset's data, it does not mint the skeletons,
        // animations, materials and textures a fresh import would, because those have their own identities.
        FMeshImportOptions Options = BuildOptions(false);
        Options.bImportMeshes     = true;
        Options.bImportSkeleton   = false;
        Options.bImportAnimations = false;
        Options.bImportMaterials  = false;
        Options.bImportTextures   = false;

        // Taken from the ASSET, not the dialogue: without this a mesh with a field silently loses it on
        // every reimport.
        Options.DistanceField = Mesh->DistanceFieldSettings;

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

        // Name follows the ASSET: the object keeps its identity through a reimport.
        NewResource->Name = Mesh->GetName();

        // Match the fresh-import path, or a reimport would hand the asset back its file-global slot
        // numbering and undo the dense packing. Merge mode densifies at parse time instead.
        if (!bMergeMeshes)
        {
            DensifyMaterialSlots(*NewResource);
        }

        TVector<TObjectPtr<CMaterialInterface>> RemappedMaterials;
        RemapMaterialSlots(Mesh->GetMeshResource(), Mesh->Materials, *NewResource, RemappedMaterials);
        Mesh->Materials = Move(RemappedMaterials);

        // Same CObject, same GUID, same package: SetMeshResource rebuilds bounds and GPU buffers and
        // invalidates the resolve-cache entries, so live components pick the new geometry up.
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

            FFixedString Header(FFixedString::CtorSprintf(), "Meshes (%zu)###MeshStats", SourceData.Resources.size());
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

            const float Height = eastl::min<float>(180.0f, (SourceData.Resources.size() + 1) * ImGui::GetTextLineHeightWithSpacing() + 8.0f);
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
            if (SourceData.Images.empty())
            {
                return;
            }

            FFixedString Header(FFixedString::CtorSprintf(), "Textures (%zu)###Textures", SourceData.Images.size());
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

            FFixedString Header(FFixedString::CtorSprintf(), "Skeletons (%zu)###Skeletons", SourceData.Skeletons.size());
            if (!ImGui::CollapsingHeader(Header.c_str()))
            {
                return;
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

            FFixedString Header(FFixedString::CtorSprintf(), "Animations (%zu)###Animations", SourceData.Animations.size());
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

                    // Channels resolve by bone name at sample time, and an unmatched one silently freezes
                    // its bone at bind pose. Surface that here, where it can still be acted on.
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
