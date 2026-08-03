#include "ThumbnailScene.h"
#include "ThumbnailUtils.h"

#include "Core/Math/Math.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHI.h"
#include "Renderer/RHIUpload.h"
#include "World/WorldTypes.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SystemSingletons.h"
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

    bool FThumbnailScene::Capture(FPackageThumbnail& Thumbnail)
    {
        if (!bInitialized || !World.IsValid() || World->GetRenderer() == nullptr)
        {
            return false;
        }

        // Drive the mesh resolve to a fixed point BEFORE extracting. The resolve pre-pass defers
        // anything it cannot finish to the next frame, which the normal loop absorbs invisibly -- but
        // this capture renders exactly one frame and reads it straight back, so a deferred mesh is not
        // "late", it is missing from the image. That is the empty-world thumbnail.
        static_cast<FForwardRenderScene*>(World->GetRenderer())->SettleResolveWork();

        const uint8 FrameIndex = (uint8)GRenderManager->GetCurrentFrameIndex();
        World->Extract();

        bool   bCaptured = false;
        uint32 SourceWidth = 0;
        uint32 SourceHeight = 0;
        RHI::GPUPtr Readback = 0;

        auto RecordCapture = [&]()
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
            // Wait only on THIS copy's completion, not the whole device: a WaitDeviceIdle here would
            // stall on unrelated in-flight frame work.
            RHI::SubmitAndWait(CL);
            RHI::ResetCommandList(CL);

            bCaptured = true;
        };

        RecordCapture();

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
