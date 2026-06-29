#include "ThumbnailManager.h"
#include "ThumbnailScene.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Paths/Paths.h"
#include "Renderer/RHITexture.h"

#include "TaskSystem/TaskSystem.h"
#include "Tools/PrimitiveManager/PrimitiveManager.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/PostProcessComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Scene/RenderScene/SceneMeshes.h"


namespace Lumina
{

    static CThumbnailManager* ThumbnailManagerSingleton = nullptr;

    CThumbnailManager::CThumbnailManager()
    {
    }

    void CThumbnailManager::Initialize()
    {
        (void)CPackage::OnPackageDestroyed.AddMember(this, &ThisClass::OnPackageDestroyed);

        constexpr float kThumbnailFOV       = 35.0f;
        constexpr float kMeshFramingScale   = 3.2f;   // Margin around bounds
        constexpr float kSphereFramingScale = 5.5f;   // Sphere fills ~60% of frame
        
        auto SetupStudioLighting = [](CWorld* World)
        {
            entt::entity Light = World->ConstructEntity("StudioLight");
            World->EmplaceComponent<SDirectionalLightComponent>(Light);
            World->EmplaceComponent<SEnvironmentComponent>(Light);
            World->EmplaceComponent<SSkyLightComponent>(Light);
        };

        // Bounding-sphere framing so wide-flat and tall-narrow meshes frame the same as cubes.
        auto FrameBounds = [](FThumbnailScene& Scene, const FAABB& Bounds)
        {
            const FVector3 Center  = Bounds.GetCenter();
            const float    Radius  = Math::Max(Math::Length(Bounds.GetSize() * 0.5f), 0.5f);
            const FVector3 Dir     = Math::Normalize(FVector3(1.0f, 0.6f, 1.0f));
            const FVector3 CamPos  = Center + Dir * (Radius * kMeshFramingScale);
            Scene.SetCameraTransform(CamPos, Center, kThumbnailFOV);
        };

        // Populate the scene with a skeletal mesh in bind pose (the render path falls back to identity
        // bones when no animation drives it). Shared by skeletal-mesh/skeleton/animation thumbnails.
        auto RenderSkeletalMesh = [SetupStudioLighting, FrameBounds](FThumbnailScene& Scene, CSkeletalMesh* Mesh)
        {
            if (Mesh == nullptr)
            {
                return;
            }

            CWorld* World = Scene.GetWorld();

            SetupStudioLighting(World);

            entt::entity MeshEntity = World->ConstructEntity("SkeletalMesh");
            World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SkeletalMesh = Mesh;

            FrameBounds(Scene, Mesh->GetAABB());
        };

        RegisterThumbnailRenderer(CStaticMesh::StaticClass(), [SetupStudioLighting, FrameBounds](FThumbnailScene& Scene, CObject* Asset)
            {
                CStaticMesh* Mesh = Cast<CStaticMesh>(Asset);
                if (Mesh == nullptr)
                {
                    return;
                }

                CWorld* World = Scene.GetWorld();

                SetupStudioLighting(World);

                entt::entity MeshEntity = World->ConstructEntity("Mesh");
                World->EmplaceComponent<SStaticMeshComponent>(MeshEntity).StaticMesh = Mesh;

                FrameBounds(Scene, Mesh->GetAABB());
            });

        RegisterThumbnailRenderer(CSkeletalMesh::StaticClass(), [RenderSkeletalMesh](FThumbnailScene& Scene, CObject* Asset)
            {
                RenderSkeletalMesh(Scene, Cast<CSkeletalMesh>(Asset));
            });

        // A skeleton shows its preview mesh (the only renderable it carries).
        RegisterThumbnailRenderer(CSkeleton::StaticClass(), [RenderSkeletalMesh](FThumbnailScene& Scene, CObject* Asset)
            {
                CSkeleton* Skeleton = Cast<CSkeleton>(Asset);
                if (Skeleton == nullptr)
                {
                    return;
                }
                RenderSkeletalMesh(Scene, Skeleton->PreviewMesh.Get());
            });

        // An animation shows its skeleton's preview mesh in bind pose (posing would need the anim system ticked).
        RegisterThumbnailRenderer(CAnimation::StaticClass(), [RenderSkeletalMesh](FThumbnailScene& Scene, CObject* Asset)
            {
                CAnimation* Animation = Cast<CAnimation>(Asset);
                if (Animation == nullptr || !Animation->Skeleton.IsValid())
                {
                    return;
                }
                RenderSkeletalMesh(Scene, Animation->Skeleton->PreviewMesh.Get());
            });

        RegisterThumbnailRenderer(CMaterialInterface::StaticClass(), [SetupStudioLighting](FThumbnailScene& Scene, CObject* Asset)
            {
                CMaterialInterface* Material = Cast<CMaterialInterface>(Asset);
                if (Material == nullptr)
                {
                    return;
                }

                CWorld* World = Scene.GetWorld();

                SetupStudioLighting(World);

                // PP materials are screen-space; mesh override falls back to default. Use a volume.
                CMaterial* BaseMaterial = Material->GetMaterial();
                const bool bIsPostProcess = BaseMaterial != nullptr && BaseMaterial->GetMaterialType() == EMaterialType::PostProcess;

                entt::entity MeshEntity = World->ConstructEntity("PreviewSphere");
                SStaticMeshComponent& MeshComp = World->EmplaceComponent<SStaticMeshComponent>(MeshEntity);
                MeshComp.StaticMesh = CPrimitiveManager::Get().SphereMesh;
                if (!bIsPostProcess)
                {
                    MeshComp.MaterialOverrides.push_back(Material);
                }

                if (bIsPostProcess)
                {
                    entt::entity VolumeEntity = World->ConstructEntity("PreviewPostProcessVolume");
                    SPostProcessComponent& Volume = World->EmplaceComponent<SPostProcessComponent>(VolumeEntity);
                    Volume.bInfiniteExtent = true;
                    Volume.PostProcessMaterials.push_back(Material);
                }

                // Sphere mesh has unit radius.
                const FVector3 Dir = Math::Normalize(FVector3(0.0f, 0.25f, 1.0f));
                Scene.SetCameraTransform(Dir * kSphereFramingScale, FVector3(0.0f), kThumbnailFOV);
            });

        RegisterThumbnailRenderer(CParticleSystem::StaticClass(), [SetupStudioLighting](FThumbnailScene& Scene, CObject* Asset)
            {
                CParticleSystem* PS = Cast<CParticleSystem>(Asset);
                if (PS == nullptr)
                {
                    return;
                }

                CWorld* World = Scene.GetWorld();

                SetupStudioLighting(World);

                entt::entity ParticleEntity = World->ConstructEntity("ParticleSystem");
                World->EmplaceComponent<SParticleSystemComponent>(ParticleEntity).ParticleSystem = PS;

                // No AABB on a particle system; fixed pull-back for typical spawn radius.
                const FVector3 Dir = Math::Normalize(FVector3(0.0f, 0.25f, 1.0f));
                Scene.SetCameraTransform(Dir * 4.0f, FVector3(0.0f), kThumbnailFOV);
            });
    }

    CThumbnailManager& CThumbnailManager::Get()
    {
        static std::once_flag Flag;
        std::call_once(Flag, []()
        {
            ThumbnailManagerSingleton = NewObject<CThumbnailManager>();
            ThumbnailManagerSingleton->Initialize();
            ThumbnailManagerSingleton->AddToRoot();
        });

        return *ThumbnailManagerSingleton;
    }

    void CThumbnailManager::AsyncLoadThumbnailsForPackage(const FName& Package)
    {
        Task::AsyncTask(1, 1, [this, Package](uint32, uint32, uint32)
        {
            CPackage* MaybePackage = CPackage::LoadPackage(Package.c_str());
            if (MaybePackage == nullptr)
            {
                return;
            }
            
            FPackageThumbnail* Thumbnail = MaybePackage->GetPackageThumbnail();
            
            FPackageThumbnail::EState Expected = FPackageThumbnail::EState::None;
            if (!Thumbnail->LoadState.compare_exchange_strong(Expected, FPackageThumbnail::EState::Loading, std::memory_order_acquire))
            {
                return;
            }
            
            if (Thumbnail->ImageData.empty())
            {
                FWriteScopeLock Lock(ThumbnailLock);
                Thumbnails.insert_or_assign(Package, Thumbnail);
                return;
            }
            
            constexpr uint32 ThumbExtent = 256;
            RHI::FManagedTexture Image = RHI::Textures::Create(RHI::FTexture2DDesc
            {
                .Width  = ThumbExtent,
                .Height = ThumbExtent,
                .Format = EFormat::RGBA8_UNORM,
            });

            const uint8 BytesPerPixel = RHI::Format::BytesPerBlock(EFormat::RGBA8_UNORM);
            const uint32 RowBytes = ThumbExtent * BytesPerPixel;

            TVector<uint8> FlippedData(Thumbnail->ImageData.size());
            uint8* Destination = FlippedData.data();
            const uint8* Source = Thumbnail->ImageData.data();

            for (uint32 y = 0; y < ThumbExtent; ++y)
            {
                const uint32 FlippedY = ThumbExtent - 1 - y;
                Memory::Memcpy(Destination + FlippedY * RowBytes, Source + y * RowBytes, RowBytes);
            }

            RHI::Textures::Upload(Image, 0, FlippedData.data(), FlippedData.size(), ThumbExtent);

            Thumbnail->LoadedImage = Image;
            Thumbnail->LoadState.store(FPackageThumbnail::EState::Loaded, std::memory_order_release);

            FWriteScopeLock Lock(ThumbnailLock);
            Thumbnails.insert_or_assign(Package, Thumbnail);
        });
    }

    FPackageThumbnail* CThumbnailManager::GetThumbnailForPackage(const FName& Package)
    {
        {
            FReadScopeLock Lock(ThumbnailLock);
            auto It = Thumbnails.find(Package);
            if (It != Thumbnails.end())
            {
                FPackageThumbnail* CachedThumbnail = It->second;
                if (CachedThumbnail && CachedThumbnail->IsReadyForRender())
                {
                    return CachedThumbnail;
                }
                
                if (CachedThumbnail)
                {
                    auto LoadState = CachedThumbnail->LoadState.load(std::memory_order_acquire);
                    if (LoadState == FPackageThumbnail::EState::None) // The package thumbnail may be dirty.
                    {
                        AsyncLoadThumbnailsForPackage(Package);
                    }
                }
                
                return nullptr;
            }
        }
    
        AsyncLoadThumbnailsForPackage(Package);
        return nullptr;
    }

    void CThumbnailManager::RegisterThumbnailRenderer(CClass* AssetClass, FThumbnailRendererFn Renderer)
    {
        if (AssetClass == nullptr)
        {
            return;
        }
        ThumbnailRenderers.insert_or_assign(AssetClass, Move(Renderer));
    }

    bool CThumbnailManager::GenerateThumbnail(CObject* Asset, CPackage* Package)
    {
        if (Asset == nullptr || Package == nullptr)
        {
            return false;
        }

        FThumbnailRendererFn* Renderer = nullptr;
        for (CClass* Klass = Asset->GetClass(); Klass != nullptr; Klass = Cast<CClass>(Klass->GetSuperClass()))
        {
            auto It = ThumbnailRenderers.find(Klass);
            if (It != ThumbnailRenderers.end())
            {
                Renderer = &It->second;
                break;
            }
        }

        if (Renderer == nullptr)
        {
            return false;
        }

        FThumbnailScene Scene(512);
        Scene.Begin();

        if (Scene.GetWorld() == nullptr)
        {
            return false;
        }

        (*Renderer)(Scene, Asset);

        FPackageThumbnail* Thumbnail = Package->GetPackageThumbnail();
        if (Thumbnail == nullptr)
        {
            return false;
        }

        const bool bCaptured = Scene.Capture(*Thumbnail);
        Scene.End();
        return bCaptured;
    }

    void CThumbnailManager::OnPackageDestroyed(FName Package)
    {
        FWriteScopeLock Lock(ThumbnailLock);

        auto It = Thumbnails.find(Package);
        if (It != Thumbnails.end())
        {
            Thumbnails.erase(It);
        }
    }
}
