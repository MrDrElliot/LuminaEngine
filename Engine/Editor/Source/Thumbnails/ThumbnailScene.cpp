#include "ThumbnailScene.h"
#include "World/ECS/Registry.h"
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

        // A submitted copy still reading that image, or writing that readback, must be allowed to land.
        AbandonCapture();

        if (World.IsValid())
        {
            World->TeardownWorld();
        }
        // Dropping the only strong ref frees the world, unlike ForceDestroyNow which would dangle this.
        World        = nullptr;
        CameraEntity = ECS::NullEntity;
        bInitialized = false;
    }

    ECS::FEntity FThumbnailScene::SpawnEntity(FName Name)
    {
        if (!bInitialized || !World.IsValid())
        {
            return ECS::NullEntity;
        }

        const ECS::FEntity Entity = World->ConstructEntity(Name);
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

        for (ECS::FEntity Entity : SpawnedEntities)
        {
            World->DestroyEntity(Entity);
        }
        SpawnedEntities.clear();
    }

    void FThumbnailScene::SetCameraTransform(const FVector3& Position, const FVector3& Target, float FOVDegrees)
    {
        if (!bInitialized || CameraEntity == ECS::NullEntity)
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

        // Extract reads FResolvedSceneView, and an unticked world leaves it default, culling the subject.
        FResolvedSceneView& Resolved = ECS::GetWorldRegistry(*World).Ctx().Get<FResolvedSceneView>();
        Resolved.ViewVolume      = Camera.GetViewVolume();
        Resolved.bHasView        = true;
        Resolved.bHasPostProcess = false;
        Resolved.PostProcessMaterials.clear();
    }

    // GPU-side state of a capture that has been submitted but not yet read back.
    struct FThumbnailScene::FPendingCapture
    {
        RHI::FGPUAllocation Readback = {};
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

        // This capture renders exactly one frame, so a deferred mesh is missing rather than merely late.
        static_cast<FDefaultSceneRenderer*>(World->GetRenderer())->SettleResolveWork();

        const uint8 FrameIndex = (uint8)Render().GetCurrentFrameIndex();
        World->Extract();

        {
            // Queued uploads only land at the next BeginFrame, so culling would chase a null Bounds address.
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
            if (Out.Readback.Gpu == 0)
            {
                return false;
            }
            RHI::SetDebugName(Out.Readback.Gpu, "Readback.Thumbnail");

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
            RHI::CmdCopyTextureToMemory(CL, Output.Texture, RHI::FTextureSlice{}, Out.Readback.Gpu, Out.Width);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Host);

            // Submits WITHOUT waiting and keeps the timeline value, so the caller can poll instead of blocking.
            Out.Semaphore = RHI::Core::GetQueueTimeline(RHI::EQueueType::Graphics);
            Out.Value     = RHI::Core::SubmitOn(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        }

        return true;
    }

    bool FThumbnailScene::ResolveCapture(FPendingCapture& In, FPackageThumbnail& Thumbnail)
    {
        if (In.Readback.Gpu == 0)
        {
            return false;
        }

        const void* MappedMemory = In.Readback.Cpu;
        if (MappedMemory != nullptr)
        {
            ThumbnailUtils::StoreDownsampledRGBA(Thumbnail, static_cast<const uint8*>(MappedMemory),
                In.Width, In.Height, (size_t)In.Width * 4u);
        }

        RHI::Free(In.Readback);
        In.Readback = {};
        return MappedMemory != nullptr;
    }

    bool FThumbnailScene::Capture(FPackageThumbnail& Thumbnail)
    {
        FPendingCapture Local;
        if (!RecordCapture(Local))
        {
            if (Local.Readback.Gpu != 0)
            {
                RHI::Free(Local.Readback);
            }
            return false;
        }

        // Waits on THIS copy only, since a device-wide idle would stall on unrelated in-flight frame work.
        RHI::WaitSemaphore(Local.Semaphore, Local.Value);
        return ResolveCapture(Local, Thumbnail);
    }

    bool FThumbnailScene::BeginCapture()
    {
        if (Pending != nullptr)
        {
            return false;   // one at a time, the next render would overwrite the target this copy reads
        }

        TUniquePtr<FPendingCapture> New = MakeUnique<FPendingCapture>();
        if (!RecordCapture(*New))
        {
            if (New->Readback.Gpu != 0)
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

        // Freeing the destination now would hand a live GPU write freed memory.
        RHI::WaitSemaphore(Pending->Semaphore, Pending->Value);
        if (Pending->Readback.Gpu != 0)
        {
            RHI::Free(Pending->Readback);
        }
        Pending = nullptr;
    }
}
