#include "ThumbnailScene.h"
#include "ThumbnailUtils.h"

#include "Core/Math/Math.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHIUpload.h"
#include "World/WorldTypes.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "World/Scene/RenderScene/Default/DefaultSceneRenderer.h"

namespace Lumina
{
    FThumbnailScene::FThumbnailScene(uint32 RenderTargetSize)
        : RTSize(RenderTargetSize)
    {
    }

    FThumbnailScene::~FThumbnailScene()
    {
        End();
    }

    void FThumbnailScene::Begin()
    {
        if (bInitialized)
        {
            return;
        }

        World = NewObject<CWorld>(nullptr, "ThumbnailWorld", FGuid::New(), OF_Transient);
        World->InitializeWorld(EWorldType::Editor);
        
        if (IRenderScene* Scene = World->GetRenderer())
        {
            Scene->Resize(FUIntVector2(RTSize, RTSize));

            FSceneRenderSettings& Settings = Scene->GetSceneRenderSettings();
            Settings.bDrawBillboards = false;
            
            Settings.bOcclusionCull       = false;
            Settings.bShadowOcclusionCull = false;
        }

        CameraEntity = World->ConstructEntity("ThumbnailCamera");
        SCameraComponent& Camera = World->EmplaceComponent<SCameraComponent>(CameraEntity);
        Camera.SetAspectRatio(1.0f);
        World->SetActiveCamera(CameraEntity);

        bInitialized = true;
    }

    void FThumbnailScene::End()
    {
        if (!bInitialized)
        {
            return;
        }

        // Before the world (and its render target) go away: a submitted copy still reading that image, or
        // still writing that readback buffer, must be allowed to land first.
        AbandonCapture();

        if (World.IsValid())
        {
            World->TeardownWorld();
        }
        // Releasing the only strong ref to the transient preview world drops its refcount to zero and
        // frees it, no ForceDestroyNow (which would dangle this TObjectPtr, then touch freed memory).
        World        = nullptr;
        CameraEntity = entt::null;
        bInitialized = false;
    }

    entt::entity FThumbnailScene::SpawnEntity(FName Name)
    {
        if (!bInitialized || !World.IsValid())
        {
            return entt::null;
        }

        const entt::entity Entity = World->ConstructEntity(Name);
        SpawnedEntities.push_back(Entity);
        return Entity;
    }

    void FThumbnailScene::ResetContents()
    {
        if (!bInitialized || !World.IsValid())
        {
            SpawnedEntities.clear();
            return;
        }

        for (entt::entity Entity : SpawnedEntities)
        {
            World->DestroyEntity(Entity);
        }
        SpawnedEntities.clear();
    }

    void FThumbnailScene::SetCameraTransform(const FVector3& Position, const FVector3& Target, float FOVDegrees)
    {
        if (!bInitialized || CameraEntity == entt::null)
        {
            return;
        }

        STransformComponent& Transform = World->GetComponent<STransformComponent>(CameraEntity);
        Transform.SetLocation(Position);
        const FQuat Rotation = Math::FindLookAtRotation(Target, Position);
        Transform.SetRotation(Rotation);

        // World is never ticked so CameraSystem doesn't run; set view directly here.
        SCameraComponent& Camera = World->GetComponent<SCameraComponent>(CameraEntity);
        Camera.SetFOV(FOVDegrees);
        Camera.SetAspectRatio(1.0f);
        const FVector3 Forward = Math::Normalize(Target - Position);
        const FVector3 WorldUp(0.0f, 1.0f, 0.0f);
        const FVector3 Right   = Math::Normalize(Math::Cross(WorldUp, Forward));
        const FVector3 Up      = Math::Normalize(Math::Cross(Forward, Right));
        Camera.SetView(Position, Forward, Up);

        // Publish it as the RESOLVED view too. CWorld::Extract does not read the camera component -- it
        // reads the FResolvedSceneView context singleton, which only SCameraSystem writes, and that system
        // only runs on a world tick. An unticked world therefore leaves bHasView false and Extract falls
        // back to a default-constructed FViewVolume: origin, 90 degrees, 0.01 near. Every thumbnail entity
        // is spawned at the origin, so the frustum sat inside the subject and culled it, leaving the sky
        // (a fullscreen pass that needs no scene data) as the only thing in the image. That was the
        // empty-world thumbnail.
        FResolvedSceneView& Resolved = ECS::GetWorldRegistry(*World).ctx().get<FResolvedSceneView>();
        Resolved.ViewVolume      = Camera.GetViewVolume();
        Resolved.bHasView        = true;
        Resolved.bHasPostProcess = false;
        Resolved.PostProcessMaterials.clear();
    }

    // GPU-side state of a capture that has been submitted but not yet read back.
    struct FThumbnailScene::FPendingCapture
    {
        RHI::GPUPtr      Readback  = 0;
        uint32           Width     = 0;
        uint32           Height    = 0;
        RHI::FSemaphoreH Semaphore = {};
        uint64           Value     = 0;   // graphics timeline value that signals the copy is done
    };

    bool FThumbnailScene::RecordCapture(FPendingCapture& Out)
    {
        if (!bInitialized || !World.IsValid() || World->GetRenderer() == nullptr)
        {
            return false;
        }

        // Drive the mesh resolve to a fixed point BEFORE extracting. The resolve pre-pass defers
        // anything it cannot finish to the next frame, which the normal loop absorbs invisibly -- but
        // this capture renders exactly one frame and reads it straight back, so a deferred mesh is not
        // "late", it is missing from the image. That is the empty-world thumbnail.
        static_cast<FDefaultSceneRenderer*>(World->GetRenderer())->SettleResolveWork();

        const uint8 FrameIndex = (uint8)GRenderManager->GetCurrentFrameIndex();
        World->Extract();

        {
            // This render bypasses the frame pipeline, and queued RHI uploads only become resident at
            // the next Core::BeginFrame. A mesh that finished loading since the last frame (exactly the
            // asset being thumbnailed, or scene meshes streaming in) still has its meshlet header/bounds
            // copies pending -- rendering now would make CullMeshlets read uninitialized header memory
            // and chase a null Bounds address (GPU MMU page fault at a near-zero VA). Flush them first;
            // the extra submit is ordered ahead of this frame's rendering.
            RHI::FlushUploadsAndWait();

            IRenderScene* Scene = World->GetRenderer();
            Scene->PrepareRender(FrameIndex);
            Scene->RenderView(FrameIndex);

            auto* Renderer = static_cast<FDefaultSceneRenderer*>(Scene);
            const FSceneImage& Output = Renderer->GetDisplayImage();
            if (!Output.IsValid())
            {
                return false;
            }

            Out.Width  = Output.GetSizeX();
            Out.Height = Output.GetSizeY();
            Out.Readback = RHI::Malloc((uint64)Out.Width * Out.Height * 4u, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            if (Out.Readback == 0)
            {
                return false;
            }

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
            RHI::CmdCopyTextureToMemory(CL, Output.Texture, RHI::FTextureSlice{}, Out.Readback, Out.Width);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Host);

            // Submit WITHOUT waiting, and keep the timeline value this submission signals so the caller can
            // POLL it. This used to be SubmitAndWait, i.e. a synchronous GPU round-trip on the game thread
            // for every thumbnail. SubmitOn also hands the command list to the frame slot's retire list, so
            // it is reclaimed for us -- SubmitAndWait had to reset it by hand because it bypassed that.
            Out.Semaphore = RHI::Core::GetQueueTimeline(RHI::EQueueType::Graphics);
            Out.Value     = RHI::Core::SubmitOn(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        }

        return true;
    }

    bool FThumbnailScene::ResolveCapture(FPendingCapture& In, FPackageThumbnail& Thumbnail)
    {
        if (In.Readback == 0)
        {
            return false;
        }

        const void* MappedMemory = RHI::ToHost(In.Readback);
        if (MappedMemory != nullptr)
        {
            ThumbnailUtils::StoreDownsampledRGBA(Thumbnail, static_cast<const uint8*>(MappedMemory),
                In.Width, In.Height, (size_t)In.Width * 4u);
        }

        RHI::Free(In.Readback);
        In.Readback = 0;
        return MappedMemory != nullptr;
    }

    bool FThumbnailScene::Capture(FPackageThumbnail& Thumbnail)
    {
        FPendingCapture Local;
        if (!RecordCapture(Local))
        {
            if (Local.Readback != 0)
            {
                RHI::Free(Local.Readback);
            }
            return false;
        }

        // Wait only on THIS copy's completion, not the whole device: a WaitDeviceIdle here would stall on
        // unrelated in-flight frame work.
        RHI::WaitSemaphore(Local.Semaphore, Local.Value);
        return ResolveCapture(Local, Thumbnail);
    }

    bool FThumbnailScene::BeginCapture()
    {
        if (Pending != nullptr)
        {
            return false;   // one at a time -- the next render would overwrite the target this copy reads
        }

        TUniquePtr<FPendingCapture> New = MakeUnique<FPendingCapture>();
        if (!RecordCapture(*New))
        {
            if (New->Readback != 0)
            {
                RHI::Free(New->Readback);
            }
            return false;
        }

        Pending = Move(New);
        return true;
    }

    bool FThumbnailScene::IsCaptureReady() const
    {
        return Pending != nullptr && RHI::GetSemaphoreValue(Pending->Semaphore) >= Pending->Value;
    }

    bool FThumbnailScene::FinishCapture(FPackageThumbnail& Thumbnail)
    {
        if (Pending == nullptr)
        {
            return false;
        }

        const bool bResolved = ResolveCapture(*Pending, Thumbnail);
        Pending = nullptr;
        return bResolved;
    }

    bool FThumbnailScene::WaitAndFinishCapture(FPackageThumbnail& Thumbnail)
    {
        if (Pending == nullptr)
        {
            return false;
        }

        RHI::WaitSemaphore(Pending->Semaphore, Pending->Value);
        return FinishCapture(Thumbnail);
    }

    void FThumbnailScene::AbandonCapture()
    {
        if (Pending == nullptr)
        {
            return;
        }

        // The copy may still be in flight; freeing its destination now would hand a live GPU write freed
        // memory. Waiting is fine here -- this only runs on teardown or when the scene is repurposed.
        RHI::WaitSemaphore(Pending->Semaphore, Pending->Value);
        if (Pending->Readback != 0)
        {
            RHI::Free(Pending->Readback);
        }
        Pending = nullptr;
    }
}
