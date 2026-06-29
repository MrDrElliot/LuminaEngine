#include "ThumbnailScene.h"
#include "ThumbnailUtils.h"

#include "Core/Math/Math.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RenderThread.h"
#include "Renderer/RHI.h"
#include "World/WorldTypes.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "World/Scene/RenderScene/Forward/ForwardRenderScene.h"

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
            Scene->GetSceneRenderSettings().bDrawBillboards = false;
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
    }

    bool FThumbnailScene::Capture(FPackageThumbnail& Thumbnail)
    {
        if (!bInitialized || !World.IsValid() || World->GetRenderer() == nullptr)
        {
            return false;
        }

        // Flush so the thumbnail world (created in Begin) is realized before we render it.
        FlushRenderingCommands();

        // Extract reads the ECS (game thread) and bumps the frame slot's Produced count;
        // SignalFrameConsumed below balances it or the next capture deadlocks.
        const uint8 FrameIndex = (uint8)GRenderManager->GetCurrentFrameIndex();
        World->Extract();

        bool   bCaptured = false;
        uint32 SourceWidth = 0;
        uint32 SourceHeight = 0;
        RHI::GPUPtr Readback = 0;

        // Submit on the render thread (the sole graphics submitter) so we never race the
        // swapchain frame. Capture is game-thread only, so EnqueueAndWait can't self-deadlock.
        auto RecordCapture = [&]()
        {
            IRenderScene* Scene = World->GetRenderer();
            Scene->PrepareRender(FrameIndex);
            Scene->RenderView(FrameIndex);

            auto* Forward = static_cast<FForwardRenderScene*>(Scene);
            const FSceneImage& Output = Forward->GetDisplayImage();
            if (!Output.IsValid())
            {
                return;
            }

            SourceWidth  = Output.GetSizeX();
            SourceHeight = Output.GetSizeY();
            Readback = RHI::Malloc((uint64)SourceWidth * SourceHeight * 4u, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
            RHI::CmdCopyTextureToMemory(CL, Output.Texture, RHI::FTextureSlice{}, Readback, SourceWidth);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Host);
            RHI::Submit(CL);
            RHI::WaitDeviceIdle();
            RHI::ResetCommandList(CL);

            bCaptured = true;
        };

        if (GRenderThread != nullptr && GRenderThread->IsRunning())
        {
            GRenderThread->EnqueueAndWait("ThumbnailCapture", [&RecordCapture]() { RecordCapture(); });
        }
        else
        {
            RecordCapture();
        }

        // Balance the Produced++ that Extract did, since we bypassed the render lambda.
        if (IRenderScene* Scene = World->GetRenderer())
        {
            Scene->SignalFrameConsumed(FrameIndex);
        }

        if (!bCaptured || Readback == 0)
        {
            if (Readback != 0)
            {
                RHI::Free(Readback);
            }
            return false;
        }

        const void* MappedMemory = RHI::ToHost(Readback);
        if (MappedMemory != nullptr)
        {
            ThumbnailUtils::StoreDownsampledRGBA(Thumbnail, static_cast<const uint8*>(MappedMemory),
                SourceWidth, SourceHeight, (size_t)SourceWidth * 4u);
        }

        RHI::Free(Readback);
        return MappedMemory != nullptr;
    }
}
