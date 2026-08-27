#include "Core/Threading/Thread.h"
#include "World/ECS/Registry.h"
#include "ThumbnailManager.h"
#include "Memory/MemoryTracking.h"
#include "ThumbnailCache.h"
#include "ThumbnailScene.h"
#include "ThumbnailUtils.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectFlags.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Paths/Paths.h"
#include "Renderer/RHITexture.h"

#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
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

    CThumbnailManager::~CThumbnailManager() = default;

    void CThumbnailManager::Initialize()
    {
        (void)CPackage::OnPackageDestroyed.AddMember(this, &ThisClass::OnPackageDestroyed);

        // Any registry change may have altered content hashes, so flag a sweep for stale records.
        (void)FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddLambda([this]()
        {
            bRegistryDirty.store(true, std::memory_order_relaxed);
        });

        constexpr float kThumbnailFOV       = 35.0f;
        constexpr float kMeshFramingScale   = 3.2f;   // Margin around bounds
        constexpr float kSphereFramingScale = 5.5f;   // Sphere fills ~60% of frame

        auto SetupStudioLighting = [](FThumbnailScene& Scene)
        {
            CWorld* World = Scene.GetWorld();
            ECS::FEntity Light = Scene.SpawnEntity("StudioLight");
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

        // The render path falls back to identity bones when no animation drives the mesh.
        auto RenderSkeletalMesh = [SetupStudioLighting, FrameBounds](FThumbnailScene& Scene, CSkeletalMesh* Mesh)
        {
            if (Mesh == nullptr)
            {
                return;
            }

            CWorld* World = Scene.GetWorld();

            SetupStudioLighting(Scene);

            ECS::FEntity MeshEntity = Scene.SpawnEntity("SkeletalMesh");
            World->EmplaceComponent<SSkeletalMeshComponent>(MeshEntity).SetSkeletalMesh(Mesh);

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

                SetupStudioLighting(Scene);

                ECS::FEntity MeshEntity = Scene.SpawnEntity("Mesh");
                World->EmplaceComponent<SStaticMeshComponent>(MeshEntity).SetStaticMesh(Mesh);

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

                SetupStudioLighting(Scene);

                // PP materials are screen-space; mesh override falls back to default. Use a volume.
                CMaterial* BaseMaterial = Material->GetMaterial();
                const bool bIsPostProcess = BaseMaterial != nullptr && BaseMaterial->GetMaterialType() == EMaterialType::PostProcess;

                ECS::FEntity MeshEntity = Scene.SpawnEntity("PreviewSphere");
                SStaticMeshComponent& MeshComp = World->EmplaceComponent<SStaticMeshComponent>(MeshEntity);
                MeshComp.SetStaticMesh(CPrimitiveManager::Get().SphereMesh);
                if (!bIsPostProcess)
                {
                    MeshComp.MaterialOverrides.push_back(Material);
                }

                if (bIsPostProcess)
                {
                    ECS::FEntity VolumeEntity = Scene.SpawnEntity("PreviewPostProcessVolume");
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

                SetupStudioLighting(Scene);

                ECS::FEntity ParticleEntity = Scene.SpawnEntity("ParticleSystem");
                World->EmplaceComponent<SParticleSystemComponent>(ParticleEntity).ParticleSystem = PS;

                // No AABB on a particle system; fixed pull-back for typical spawn radius.
                const FVector3 Dir = Math::Normalize(FVector3(0.0f, 0.25f, 1.0f));
                Scene.SetCameraTransform(Dir * 4.0f, FVector3(0.0f), kThumbnailFOV);
            });
    }

    CThumbnailManager& CThumbnailManager::Get()
    {
        static FOnceFlag Flag;
        CallOnce(Flag, []()
        {
            ThumbnailManagerSingleton = NewObject<CThumbnailManager>();
            ThumbnailManagerSingleton->Initialize();
            ThumbnailManagerSingleton->AddToRoot();
        });

        return *ThumbnailManagerSingleton;
    }

    FPackageThumbnail* CThumbnailManager::GetThumbnailForPackage(const FName& Package)
    {
        {
            FReadScopeLock Lock(ThumbnailLock);
            auto It = Thumbnails.find(Package);
            if (It != Thumbnails.end())
            {
                FThumbnailRecord* Record = It->second.get();
                if (Record != nullptr && Record->Thumbnail.IsReadyForRender())
                {
                    return &Record->Thumbnail;
                }
                // In-flight or failed, so show the generic icon and do not re-enqueue.
                return nullptr;
            }
        }

        // First sighting, so insert a placeholder to dedup concurrent callers and resolve asynchronously.
        {
            FWriteScopeLock Lock(ThumbnailLock);
            if (Thumbnails.find(Package) != Thumbnails.end())
            {
                return nullptr;   // another caller raced us in; let its resolve run
            }
            auto Record = MakeUnique<FThumbnailRecord>();
            Record->Thumbnail.LoadState.store(FPackageThumbnail::EState::Requested, std::memory_order_release);
            Thumbnails.insert_or_assign(Package, Move(Record));
        }

        ResolveThumbnailAsync(Package);
        return nullptr;
    }

    void CThumbnailManager::ResolveThumbnailAsync(FName Package)
    {
        LUMINA_MEMORY_SCOPE("Thumbnails");
        Task::AsyncTask(1, 1, [this, Package](uint32, uint32, uint32)
        {
            // The returned pointer is only valid under the registry's momentary lock, so snapshot it now.
            FGuid        GUID;
            uint64       ContentHash = 0;
            FName        ClassName;
            FFixedString PackagePath;
            {
                const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Package.c_str());
                if (Data == nullptr)
                {
                    SetRecordState(Package, FPackageThumbnail::EState::Failed);
                    return;
                }
                GUID        = Data->AssetGUID;
                ContentHash = Data->ContentHash;
                ClassName   = Data->AssetClass;
                PackagePath = Data->Path;
            }

            // Record the identity so a later invalidation sweep can compare content hashes.
            {
                FWriteScopeLock Lock(ThumbnailLock);
                auto It = Thumbnails.find(Package);
                if (It != Thumbnails.end() && It->second)
                {
                    It->second->GUID        = GUID;
                    It->second->ContentHash = ContentHash;
                }
            }

            // 1. Sidecar cache (fast path; no package load once warm).
            {
                FPackageThumbnail Cached;
                if (ThumbnailCache::Load(GUID, ContentHash, Cached))
                {
                    UploadAndStore(Package, Cached);
                    return;
                }
            }

            // 2. Legacy thumbnail embedded in the .lasset. Migrate it into the sidecar for next launch.
            CPackage* Pkg = CPackage::LoadPackage(PackagePath);
            if (Pkg != nullptr)
            {
                if (FPackageThumbnail* Embedded = Pkg->GetPackageThumbnail())
                {
                    if (!Embedded->ImageData.empty())
                    {
                        ThumbnailCache::Save(GUID, ContentHash, *Embedded);
                        UploadAndStore(Package, *Embedded);
                        return;
                    }
                }
            }

            // Never load the object here, since that would race the editor's own loader on the same object.
            CClass* Klass = FindObject<CClass>(ClassName);
            if (Klass == nullptr || !CanGenerateFor(Klass))
            {
                SetRecordState(Package, FPackageThumbnail::EState::Failed);
                return;
            }

            FRenderRequest Request;
            Request.Package     = Package;
            Request.GUID        = GUID;
            Request.ContentHash = ContentHash;

            {
                FScopeLock Lock(RenderQueueMutex);
                RenderQueue.push_back(Move(Request));
            }
        });
    }

    void CThumbnailManager::UploadAndStore(const FName& Package, const FPackageThumbnail& Source)
    {
        LUMINA_MEMORY_SCOPE("Thumbnails");
        const uint32 Width  = Source.ImageWidth;
        const uint32 Height = Source.ImageHeight;
        if (Width == 0 || Height == 0 || Source.ImageData.size() < (size_t)Width * Height * 4u)
        {
            SetRecordState(Package, FPackageThumbnail::EState::Failed);
            return;
        }

        const FString DebugName = FString("Thumbnail.") + Package.ToString();

        RHI::FManagedTexture Image = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Width,
            .Height = Height,
            .Format = EFormat::RGBA8_UNORM,
            .DebugName = DebugName.c_str(),
        });

        // Stored bottom-up; flip back to top-down for display (matches ThumbnailUtils::StoreDownsampledRGBA).
        const uint32 RowBytes = Width * 4u;
        TVector<uint8> Flipped(Source.ImageData.size());
        for (uint32 y = 0; y < Height; ++y)
        {
            const uint32 FlippedY = Height - 1 - y;
            Memory::Memcpy(Flipped.data() + FlippedY * RowBytes, Source.ImageData.data() + y * RowBytes, RowBytes);
        }
        RHI::Textures::Upload(Image, 0, Flipped.data(), Flipped.size(), Width);

        FWriteScopeLock Lock(ThumbnailLock);
        auto It = Thumbnails.find(Package);
        if (It == Thumbnails.end() || It->second == nullptr)
        {
            RHI::Textures::Release(Image);   // record vanished (invalidated) while we uploaded
            return;
        }

        FPackageThumbnail& Dest = It->second->Thumbnail;
        if (Dest.LoadedImage.IsValid())
        {
            RHI::Textures::Release(Dest.LoadedImage);
        }
        Dest.ImageWidth  = Width;
        Dest.ImageHeight = Height;
        Dest.ImageData.clear();   // the GPU texture is what displays; drop the CPU copy to save memory
        Dest.LoadedImage = Image;
        Dest.LoadState.store(FPackageThumbnail::EState::Loaded, std::memory_order_release);
    }

    void CThumbnailManager::SetRecordState(const FName& Package, FPackageThumbnail::EState State)
    {
        FWriteScopeLock Lock(ThumbnailLock);
        auto It = Thumbnails.find(Package);
        if (It != Thumbnails.end() && It->second)
        {
            It->second->Thumbnail.LoadState.store(State, std::memory_order_release);
        }
    }

    CThumbnailManager::FThumbnailRendererFn* CThumbnailManager::FindRenderer(CClass* AssetClass)
    {
        for (CClass* Klass = AssetClass; Klass != nullptr; Klass = Cast<CClass>(Klass->GetSuperClass()))
        {
            auto It = ThumbnailRenderers.find(Klass);
            if (It != ThumbnailRenderers.end())
            {
                return &It->second;
            }
        }
        return nullptr;
    }

    CThumbnailManager::FThumbnailPainterFn* CThumbnailManager::FindPainter(CClass* AssetClass)
    {
        for (CClass* Klass = AssetClass; Klass != nullptr; Klass = Cast<CClass>(Klass->GetSuperClass()))
        {
            auto It = ThumbnailPainters.find(Klass);
            if (It != ThumbnailPainters.end())
            {
                return &It->second;
            }
        }
        return nullptr;
    }

    bool CThumbnailManager::CanGenerateFor(CClass* AssetClass)
    {
        return FindPainter(AssetClass) != nullptr || FindRenderer(AssetClass) != nullptr;
    }

    bool CThumbnailManager::RenderThumbnail(CObject* Asset, FPackageThumbnail& Out)
    {
        LUMINA_MEMORY_SCOPE("Thumbnails");
        if (Asset == nullptr)
        {
            return false;
        }

        // Painted first, since a class that can be drawn should never pay for a scene capture.
        if (FThumbnailPainterFn* Painter = FindPainter(Asset->GetClass()))
        {
            return (*Painter)(Asset, ThumbnailUtils::kThumbnailResolution, Out);
        }

        FThumbnailRendererFn* Renderer = FindRenderer(Asset->GetClass());
        if (Renderer == nullptr)
        {
            return false;
        }

        // ONE preview world is reused, since a scene per thumbnail made browsing a folder feel locked up.
        if (!AcquireScene())
        {
            return false;
        }

        // This path BLOCKS, so an in-flight browser capture must be collected before the target is reused.
        FlushPendingCapture();

        PersistentScene->ResetContents();
        (*Renderer)(*PersistentScene, Asset);
        return PersistentScene->Capture(Out);
    }

    bool CThumbnailManager::AcquireScene()
    {
        if (PersistentScene == nullptr)
        {
            PersistentScene = MakeUnique<FThumbnailScene>(512);
        }

        PersistentScene->Begin();
        if (PersistentScene->GetWorld() == nullptr)
        {
            PersistentScene    = nullptr;   // failed init, so do not cache a dead scene for every later capture
            bHasPendingRequest = false;
            return false;
        }
        return true;
    }

    bool CThumbnailManager::BeginThumbnailRender(CObject* Asset)
    {
        FThumbnailRendererFn* Renderer = FindRenderer(Asset->GetClass());
        if (Renderer == nullptr || !AcquireScene())
        {
            return false;
        }

        PersistentScene->ResetContents();
        (*Renderer)(*PersistentScene, Asset);
        return PersistentScene->BeginCapture();
    }

    void CThumbnailManager::FlushPendingCapture()
    {
        if (!bHasPendingRequest)
        {
            return;
        }

        FPackageThumbnail Captured;
        if (PersistentScene != nullptr && PersistentScene->WaitAndFinishCapture(Captured))
        {
            ThumbnailCache::Save(PendingRequest.GUID, PendingRequest.ContentHash, Captured);
            UploadAndStore(PendingRequest.Package, Captured);
        }
        else
        {
            SetRecordState(PendingRequest.Package, FPackageThumbnail::EState::Failed);
        }

        bHasPendingRequest = false;
    }

    void CThumbnailManager::ProcessRenderQueue(uint32 Budget)
    {
        LUMINA_MEMORY_SCOPE("Thumbnails");
        if (bRegistryDirty.exchange(false, std::memory_order_acquire))
        {
            SweepInvalidatedRecords();
        }

        // Nothing else may start while one is in flight, because the render target it reads is shared.
        if (bHasPendingRequest)
        {
            if (PersistentScene == nullptr || !PersistentScene->HasPendingCapture())
            {
                bHasPendingRequest = false;   // scene went away underneath it
            }
            else if (!PersistentScene->IsCaptureReady())
            {
                return;                       // still on the GPU, do NOT wait for it
            }
            else
            {
                FPackageThumbnail Captured;
                if (PersistentScene->FinishCapture(Captured))
                {
                    ThumbnailCache::Save(PendingRequest.GUID, PendingRequest.ContentHash, Captured);
                    UploadAndStore(PendingRequest.Package, Captured);
                }
                else
                {
                    SetRecordState(PendingRequest.Package, FPackageThumbnail::EState::Failed);
                }
                bHasPendingRequest = false;
            }
        }

        // Loading a CObject off the editor's loader path races it and tears its resources.
        TVector<FRenderRequest> Work;
        {
            FScopeLock Lock(RenderQueueMutex);
            Work.swap(RenderQueue);
        }
        if (Work.empty())
        {
            // Releasing while idle also keeps the scene from outliving the RHI at shutdown.
            constexpr uint32 kSceneIdleFramesBeforeRelease = 240;   // ~4s at 60fps
            if (PersistentScene != nullptr && !PersistentScene->HasPendingCapture()
                && ++SceneIdleFrames >= kSceneIdleFramesBeforeRelease)
            {
                PersistentScene = nullptr;
                SceneIdleFrames = 0;
            }
            return;
        }

        SceneIdleFrames = 0;

        constexpr uint32 kMaxDeferChecks = 900;   // ~15s at 60fps
        uint32 Rendered = 0;
        TVector<FRenderRequest> Keep;
        Keep.reserve(Work.size());

        auto DeferOrDrop = [&](FRenderRequest& Request)
        {
            if (++Request.DeferChecks < kMaxDeferChecks)
            {
                Keep.push_back(Move(Request));
            }
            else
            {
                SetRecordState(Request.Package, FPackageThumbnail::EState::Failed);   // gave up -> generic icon
            }
        };

        // Newest first so visible/just-scrolled tiles win.
        for (size_t Index = Work.size(); Index-- > 0; )
        {
            FRenderRequest& Request = Work[Index];

            // bHasPendingRequest gates as hard as the budget, since only one capture can be in flight.
            if (Rendered >= Budget || bHasPendingRequest)
            {
                Keep.push_back(Move(Request));   // budget spent this frame; re-check next frame
                continue;
            }

            CObject* Asset = FindObject<CObject>(Request.GUID);
            const bool bResident = Asset != nullptr
                && !Asset->HasAnyFlag(OF_NeedsLoad | OF_Loading | OF_NeedsPostLoad | OF_MarkedDestroy);
            if (!bResident)
            {
                DeferOrDrop(Request);   // not loaded yet -> keep watching (bounded)
                continue;
            }

            // A material whose shaders are not compiled yet would capture as the default material, so defer.
            if (CMaterialInterface* Material = Cast<CMaterialInterface>(Asset))
            {
                if (!Material->IsReadyForRender())
                {
                    DeferOrDrop(Request);
                    continue;
                }
            }

            // Painted classes need no world, no GPU and no readback, so they still complete inline.
            if (FThumbnailPainterFn* Painter = FindPainter(Asset->GetClass()))
            {
                FPackageThumbnail Captured;
                if ((*Painter)(Asset, ThumbnailUtils::kThumbnailResolution, Captured))
                {
                    ThumbnailCache::Save(Request.GUID, Request.ContentHash, Captured);
                    UploadAndStore(Request.Package, Captured);
                }
                else
                {
                    SetRecordState(Request.Package, FPackageThumbnail::EState::Failed);
                }
                ++Rendered;
                continue;
            }

            // Rendered classes submit here and complete on a LATER frame.
            if (BeginThumbnailRender(Asset))
            {
                PendingRequest     = Request;
                bHasPendingRequest = true;
                ++Rendered;
            }
            else
            {
                SetRecordState(Request.Package, FPackageThumbnail::EState::Failed);
            }
        }

        if (!Keep.empty())
        {
            FScopeLock Lock(RenderQueueMutex);   // fresh resolves may have arrived during the scan; keep both
            for (FRenderRequest& R : Keep)
            {
                RenderQueue.push_back(Move(R));
            }
        }
    }

    void CThumbnailManager::SweepInvalidatedRecords()
    {
        FWriteScopeLock Lock(ThumbnailLock);
        for (auto It = Thumbnails.begin(); It != Thumbnails.end(); )
        {
            FThumbnailRecord* Record = It->second.get();
            bool bDrop = (Record == nullptr);

            if (!bDrop)
            {
                const auto State = Record->Thumbnail.LoadState.load(std::memory_order_acquire);
                if (State == FPackageThumbnail::EState::Failed)
                {
                    // The asset may exist now (registry was mid-discovery earlier); let it re-resolve.
                    bDrop = true;
                }
                else if (Record->GUID.IsValid())
                {
                    if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(Record->GUID))
                    {
                        bDrop = (Data->ContentHash != Record->ContentHash);   // content changed -> regenerate
                    }
                }
            }

            It = bDrop ? Thumbnails.erase(It) : std::next(It);
        }
    }

    void CThumbnailManager::InvalidateThumbnail(const FName& Package)
    {
        FWriteScopeLock Lock(ThumbnailLock);
        Thumbnails.erase(Package);
    }

    void CThumbnailManager::RegisterThumbnailRenderer(CClass* AssetClass, FThumbnailRendererFn Renderer)
    {
        if (AssetClass == nullptr)
        {
            return;
        }
        ThumbnailRenderers.insert_or_assign(AssetClass, Move(Renderer));
    }

    void CThumbnailManager::RegisterThumbnailPainter(CClass* AssetClass, FThumbnailPainterFn Painter)
    {
        if (AssetClass == nullptr)
        {
            return;
        }
        ThumbnailPainters.insert_or_assign(AssetClass, Move(Painter));
    }

    bool CThumbnailManager::GenerateThumbnail(CObject* Asset, CPackage* Package)
    {
        if (Asset == nullptr || Package == nullptr)
        {
            return false;
        }

        FPackageThumbnail* Thumbnail = Package->GetPackageThumbnail();
        if (Thumbnail == nullptr)
        {
            return false;
        }

        // Writes into the package's slot so the editor save path still embeds it in the .lasset.
        return RenderThumbnail(Asset, *Thumbnail);
    }

    void CThumbnailManager::OnPackageDestroyed(FName Package)
    {
        FGuid GUID;
        {
            FWriteScopeLock Lock(ThumbnailLock);
            auto It = Thumbnails.find(Package);
            if (It != Thumbnails.end())
            {
                if (It->second)
                {
                    GUID = It->second->GUID;
                }
                Thumbnails.erase(It);
            }
        }

        // Drop the sidecar too so a deleted asset doesn't leave an orphan (harmless, but keeps the cache tidy).
        if (GUID.IsValid())
        {
            ThumbnailCache::Delete(GUID);
        }
    }
}
