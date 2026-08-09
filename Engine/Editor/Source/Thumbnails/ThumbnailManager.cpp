#include "ThumbnailManager.h"
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

        // Any registry change (import/save/delete) may have altered content hashes; flag a sweep so
        // stale in-memory records regenerate. The actual erase happens on Extract (ProcessRenderQueue).
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
            entt::entity Light = Scene.SpawnEntity("StudioLight");
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

            SetupStudioLighting(Scene);

            entt::entity MeshEntity = Scene.SpawnEntity("SkeletalMesh");
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

                entt::entity MeshEntity = Scene.SpawnEntity("Mesh");
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

                entt::entity MeshEntity = Scene.SpawnEntity("PreviewSphere");
                SStaticMeshComponent& MeshComp = World->EmplaceComponent<SStaticMeshComponent>(MeshEntity);
                MeshComp.SetStaticMesh(CPrimitiveManager::Get().SphereMesh);
                if (!bIsPostProcess)
                {
                    MeshComp.MaterialOverrides.push_back(Material);
                }

                if (bIsPostProcess)
                {
                    entt::entity VolumeEntity = Scene.SpawnEntity("PreviewPostProcessVolume");
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

                entt::entity ParticleEntity = Scene.SpawnEntity("ParticleSystem");
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
                // Present but in-flight (Requested/queued) or Failed: show the generic icon, don't re-enqueue.
                return nullptr;
            }
        }

        // First sighting of this asset: insert a placeholder (dedups concurrent callers) and resolve async.
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
        Task::AsyncTask(1, 1, [this, Package](uint32, uint32, uint32)
        {
            // Snapshot the identity out of the registry immediately; the returned pointer is only valid
            // under the registry's momentary lock.
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

            // 3. Queue for render. Only bother if this class has a renderer; the extract-phase drain renders it
            // once it is resident (we never load the object here -- that races the editor's loader).
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
        const uint32 Width  = Source.ImageWidth;
        const uint32 Height = Source.ImageHeight;
        if (Width == 0 || Height == 0 || Source.ImageData.size() < (size_t)Width * Height * 4u)
        {
            SetRecordState(Package, FPackageThumbnail::EState::Failed);
            return;
        }

        RHI::FManagedTexture Image = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Width,
            .Height = Height,
            .Format = EFormat::RGBA8_UNORM,
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
        if (Asset == nullptr)
        {
            return false;
        }

        // Painted first: it needs no world, no GPU and no readback, so a class that can be drawn should
        // never pay for a scene capture. Everything downstream (cache record, .lasset embed, GPU upload)
        // consumes the same FPackageThumbnail either way.
        if (FThumbnailPainterFn* Painter = FindPainter(Asset->GetClass()))
        {
            return (*Painter)(Asset, ThumbnailUtils::kThumbnailResolution, Out);
        }

        FThumbnailRendererFn* Renderer = FindRenderer(Asset->GetClass());
        if (Renderer == nullptr)
        {
            return false;
        }

        // Reuse ONE preview world across captures. This used to construct and tear down a whole
        // FThumbnailScene per thumbnail, which meant every single capture paid a CWorld construction, an
        // FForwardRenderScene::Init (a full RHI::WaitDeviceIdle, a sky/IBL bake, a shadow-atlas allocation
        // and ~40 render targets) and the matching teardown -- on the game thread, once per frame for as
        // long as the queue was non-empty. That is what made browsing a folder feel like the editor had
        // locked up.
        //
        // ResetContents() destroys the previous asset's entities and keeps the camera, so each capture
        // still starts from a clean scene. Begin() is idempotent.
        if (!AcquireScene())
        {
            return false;
        }

        // This path BLOCKS, so an in-flight browser capture has to be collected first -- one scene means
        // one render target, and repurposing it would corrupt that pending copy.
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
            PersistentScene    = nullptr;   // failed init: don't cache a dead scene for every later capture
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
        if (bRegistryDirty.exchange(false, std::memory_order_acquire))
        {
            SweepInvalidatedRecords();
        }

        // Collect the capture submitted on an earlier frame. This is the whole point of the async path: the
        // game thread submits, returns, and picks the bytes up once the GPU has signalled -- instead of
        // blocking on a readback per thumbnail. Nothing else may start while one is in flight, because the
        // scene (and therefore the render target the pending copy reads) is shared.
        if (bHasPendingRequest)
        {
            if (PersistentScene == nullptr || !PersistentScene->HasPendingCapture())
            {
                bHasPendingRequest = false;   // scene went away underneath it
            }
            else if (!PersistentScene->IsCaptureReady())
            {
                return;                       // still on the GPU -- do NOT wait for it
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

        // Take the whole queue and render up to Budget assets that are ALREADY resident + fully loaded, keeping
        // the rest for a later frame. We NEVER load the object here (or on a worker): loading a CObject off the
        // editor's own loader path races it on the same non-atomic object and tears its resources -> crash.
        // So an asset without a cached/embedded thumbnail only renders once something else has loaded it (the
        // open level, or the user opening it). FindObject is a cheap hash lookup, so scanning the queue each
        // frame is fine; kMaxDeferChecks bounds how long a never-loaded asset lingers before we give up.
        TVector<FRenderRequest> Work;
        {
            FScopeLock Lock(RenderQueueMutex);
            Work.swap(RenderQueue);
        }
        if (Work.empty())
        {
            // Nothing queued: hold the scene briefly in case more tiles scroll into view, then let it go.
            // Keeping a 512^2 render scene (plus shadow atlas, sky cube and IBL targets) resident for the
            // whole session to serve an empty queue is the wrong trade -- and releasing it while idle also
            // keeps it from outliving the RHI at shutdown, which is the one ordering this class has no
            // explicit hook for.
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

            // bHasPendingRequest gates as hard as the budget does: only one capture can be in flight.
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

            // A material whose shaders aren't compiled yet would capture as the default material -- defer.
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

            It = bDrop ? Thumbnails.erase(It) : eastl::next(It);
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
