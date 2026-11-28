#include "ImpostorBaker.h"

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Package/Package.h"
#include "Log/Log.h"
#include "Renderer/MeshImpostor.h"
#include "Renderer/RHI.h"
#include "Renderer/TextureStreamingManager.h"
#include "Renderer/Vertex.h"
#include "Session/SessionOps.h"
#include "TaskSystem/TaskSystem.h"
#include "Thumbnails/ThumbnailScene.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/ForestComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Scene/RenderScene/Default/DefaultSceneRenderer.h"

namespace Lumina
{
    namespace ImpostorBakerInternal
    {
        // Render texels per atlas texel along each axis, averaged so leaves and silhouettes resolve smoothly, unless the render would pass kMaxRenderSize.
        constexpr uint32 kSupersample   = 2;
        constexpr uint32 kMaxRenderSize = 4096;
        // Emission dimmer than this is noise from the capture, not a light worth an atlas.
        constexpr float  kMinEmissive   = 1.0e-3f;
        // Streamed textures answer the capture's own feedback, so it renders a feedback round trip at least, and stops waiting on a loader that never goes quiet.
        constexpr uint32 kMinWarmFrames = 8;
        constexpr uint32 kMaxWarmFrames = 240;
        constexpr int32  kDilatePasses  = 8;
        // Each build holds about 100 MB of readback and texels, and already spreads its views over every worker.
        constexpr int32  kMaxBuildsInFlight = 3;
        // Editor frames between looks for forest meshes that still need a bake.
        constexpr uint32 kAutoScanIntervalFrames = 300;

        using ENamedImage = FDefaultSceneRenderer::ENamedImage;
        constexpr uint32      kReadCount = 6;
        constexpr ENamedImage kReadImages[kReadCount] = { ENamedImage::VisBuffer, ENamedImage::GBufferA, ENamedImage::GBufferB,
                                                          ENamedImage::DepthAttachment, ENamedImage::GBufferC, ENamedImage::GBufferD };
        // GBufferC.a holds the receive-shadow bit, then the shading model in three bits, as PackGBuffer in GBuffer.slang writes it.
        constexpr uint32 kShadingModelShift = 1;
        constexpr uint32 kShadingModelMask  = 7;

        // One mesh's bake shape, from its settings fitted to what the capture can render.
        struct FBakeLayout
        {
            uint32 Frames      = 8;
            uint32 FrameSize   = 128;
            uint32 Supersample = kSupersample;
            bool   bFullSphere = false;

            uint32 GetAtlasSize() const { return Frames * FrameSize; }
            uint32 GetRenderSize() const { return GetAtlasSize() * Supersample; }
        };

        FBakeLayout MakeLayout(const SImpostorSettings& Settings)
        {
            FBakeLayout Layout;
            Layout.Frames      = Math::Clamp(Settings.Views, 3u, 16u);
            Layout.bFullSphere = Settings.bFullSphere;
            Layout.FrameSize   = 16u;
            while (Layout.FrameSize * 2u <= Math::Clamp(Settings.ViewResolution, 16u, 512u) && Layout.Frames * Layout.FrameSize * 2u <= kMaxRenderSize)
            {
                Layout.FrameSize *= 2u;
            }
            Layout.Supersample = Layout.GetAtlasSize() * kSupersample <= kMaxRenderSize ? kSupersample : 1u;
            return Layout;
        }

        struct FTexel
        {
            FVector3 Color    = FVector3(0.0f);
            float    Coverage = 0.0f;
            FVector3 Normal   = FVector3(0.0f, 1.0f, 0.0f);
            float    Depth    = 0.5f;
            float    AO       = 1.0f;
            FVector3 Emissive = FVector3(0.0f);
            bool     bFilled  = false;
        };

        // GBufferB holds the octahedral normal in its first two channels, RGBA16 at High and Ultra reflection quality and R10G10B10A2 below.
        FVector3 DecodeGBufferNormal(const uint8* Normals, uint32 Pixel, bool bWideNormals)
        {
            FVector2 Encoded;
            if (bWideNormals)
            {
                const uint16* Texel = reinterpret_cast<const uint16*>(Normals) + (uint64)Pixel * 4u;
                Encoded = FVector2((float)Texel[0], (float)Texel[1]) / 65535.0f;
            }
            else
            {
                const uint32 Packed = reinterpret_cast<const uint32*>(Normals)[Pixel];
                Encoded = FVector2((float)(Packed & 1023u), (float)((Packed >> 10) & 1023u)) / 1023.0f;
            }
            return OctDecode(Encoded * 2.0f - FVector2(1.0f));
        }

        float DecodeSmallFloat(uint32 Bits, uint32 MantissaBits)
        {
            const uint32 Mantissa = Bits & ((1u << MantissaBits) - 1u);
            const uint32 Exponent = Bits >> MantissaBits;
            const float  Fraction = (float)Mantissa / (float)(1u << MantissaBits);
            if (Exponent == 0u)
            {
                return Fraction * Math::Pow(2.0f, -14.0f);
            }
            // Infinity and NaN carry no emission worth keeping.
            if (Exponent == 31u)
            {
                return 0.0f;
            }
            return (1.0f + Fraction) * Math::Pow(2.0f, (float)Exponent - 15.0f);
        }

        FVector3 DecodeR11G11B10(uint32 Packed)
        {
            return FVector3(DecodeSmallFloat(Packed & 2047u, 6), DecodeSmallFloat((Packed >> 11) & 2047u, 6), DecodeSmallFloat(Packed >> 22, 5));
        }

        // Turns the view's direction up at the capture camera, with its image axes along +X and -Z as the draw reads them.
        FQuat FaceViewUp(const FVector3& Dir)
        {
            FVector3 Right, Up;
            Impostor::FrameBasis(Dir, Right, Up);
            return Math::Inverse(Math::ToQuat(FMatrix3(Right, Dir, -Up)));
        }

        uint8 ToByte(float Value)
        {
            return (uint8)Math::Clamp((int32)(Value * 255.0f + 0.5f), 0, 255);
        }

        // Empty texels take their covered neighbors' attributes, so filtering at a silhouette never pulls in black.
        void DilateView(TVector<FTexel>& Texels, uint32 AtlasSize, uint32 ViewSize, uint32 OriginX, uint32 OriginY)
        {
            TVector<FTexel> Tile;
            Tile.SetNum(ViewSize * ViewSize);
            for (uint32 Y = 0; Y < ViewSize; ++Y)
            {
                for (uint32 X = 0; X < ViewSize; ++X)
                {
                    Tile[Y * ViewSize + X] = Texels[(OriginY + Y) * AtlasSize + OriginX + X];
                }
            }

            for (int32 Pass = 0; Pass < kDilatePasses; ++Pass)
            {
                TVector<FTexel> Next = Tile;
                for (uint32 Y = 0; Y < ViewSize; ++Y)
                {
                    for (uint32 X = 0; X < ViewSize; ++X)
                    {
                        FTexel& Out = Next[Y * ViewSize + X];
                        if (Out.bFilled)
                        {
                            continue;
                        }

                        FVector3 Color(0.0f);
                        FVector3 Normal(0.0f);
                        float    Depth = 0.0f;
                        float    AO    = 0.0f;
                        FVector3 Emissive(0.0f);
                        int32    Count = 0;
                        const int32 Offsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                        for (const auto& Offset : Offsets)
                        {
                            const int32 NX = (int32)X + Offset[0];
                            const int32 NY = (int32)Y + Offset[1];
                            // Neighbors across a view's edge belong to another view.
                            if (NX < 0 || NY < 0 || NX >= (int32)ViewSize || NY >= (int32)ViewSize)
                            {
                                continue;
                            }
                            const FTexel& N = Tile[NY * ViewSize + NX];
                            if (!N.bFilled)
                            {
                                continue;
                            }
                            Color  += N.Color;
                            Normal += N.Normal;
                            Depth  += N.Depth;
                            AO     += N.AO;
                            Emissive += N.Emissive;
                            ++Count;
                        }
                        if (Count > 0)
                        {
                            const float Inv = 1.0f / (float)Count;
                            Out.Color   = Color * Inv;
                            Out.Normal  = Math::Length(Normal) > 1e-6f ? Math::Normalize(Normal) : FVector3(0.0f, 1.0f, 0.0f);
                            Out.Depth   = Depth * Inv;
                            Out.AO       = AO * Inv;
                            Out.Emissive = Emissive * Inv;
                            Out.bFilled  = true;
                        }
                    }
                }
                Tile = Move(Next);
            }

            for (uint32 Y = 0; Y < ViewSize; ++Y)
            {
                for (uint32 X = 0; X < ViewSize; ++X)
                {
                    Texels[(OriginY + Y) * AtlasSize + OriginX + X] = Tile[Y * ViewSize + X];
                }
            }
        }

        // Each coarser texel weighs its four by coverage, so a half-empty one keeps the color of what is there.
        TVector<FTexel> Downsample(const TVector<FTexel>& Texels, uint32 Size)
        {
            const uint32 Half = Size / 2u;
            TVector<FTexel> Out;
            Out.SetNum(Half * Half);
            Jobs::ParallelFor(Half, [&](uint32 Y)
            {
                for (uint32 X = 0; X < Half; ++X)
                {
                    const FTexel* Quad[4] = { &Texels[(2 * Y) * Size + 2 * X], &Texels[(2 * Y) * Size + 2 * X + 1],
                                              &Texels[(2 * Y + 1) * Size + 2 * X], &Texels[(2 * Y + 1) * Size + 2 * X + 1] };
                    float Weight = 0.0f;
                    for (const FTexel* T : Quad)
                    {
                        Weight += T->Coverage;
                    }

                    FTexel& R = Out[Y * Half + X];
                    R.Color  = FVector3(0.0f);
                    R.Normal = FVector3(0.0f);
                    R.Depth  = 0.0f;
                    R.AO     = 0.0f;
                    for (const FTexel* T : Quad)
                    {
                        const float W = Weight > 0.0f ? T->Coverage / Weight : 0.25f;
                        R.Color    += T->Color * W;
                        R.Normal   += T->Normal * W;
                        R.Depth    += T->Depth * W;
                        R.AO       += T->AO * W;
                        R.Emissive += T->Emissive * W;
                        R.Coverage += T->Coverage * 0.25f;
                    }
                    R.Normal  = Math::Length(R.Normal) > 1e-6f ? Math::Normalize(R.Normal) : FVector3(0.0f, 1.0f, 0.0f);
                    R.bFilled = true;
                }
            }, 0, Jobs::EJobPriority::Low);
            return Out;
        }

        void AppendMip(const TVector<FTexel>& Texels, FMeshImpostor& Out)
        {
            for (const FTexel& T : Texels)
            {
                Out.BaseColor.Append({ ToByte(T.Color.x), ToByte(T.Color.y), ToByte(T.Color.z), ToByte(T.Coverage) });
                const FVector2 Oct = OctEncode(T.Normal) * 0.5f + FVector2(0.5f);
                Out.NormalDepth.Append({ ToByte(Oct.x), ToByte(Oct.y), ToByte(T.Depth), ToByte(T.AO) });
                if (Out.EmissiveScale > 0.0f)
                {
                    const FVector3 Fraction = T.Emissive / Out.EmissiveScale;
                    Out.Emissive.Append({ ToByte(Math::Sqrt(Fraction.x)), ToByte(Math::Sqrt(Fraction.y)), ToByte(Math::Sqrt(Fraction.z)), 255 });
                }
            }
        }
    }

    struct FImpostorBaker::FJob
    {
        TWeakObjectPtr<CStaticMesh>  Mesh;
        FVector3                     Center = FVector3(0.0f);
        float                        Radius = 0.0f;
        // Per view, row-major over the grid, the turn that faces the view up at the camera and where its copy stands.
        TVector<FQuat>               Rotations;
        TVector<FVector3>            CellCenters;
        ImpostorBakerInternal::FBakeLayout Layout;
        // Editor frames rendered while texture streaming settles.
        uint32                       Frames = 0;

        FMatrix4                     ViewProjection = FMatrix4(1.0f);
        RHI::FGPUAllocation          Readbacks[ImpostorBakerInternal::kReadCount] = {};
        bool                         bWideNormals  = false;
        uint64                       CopyValue     = 0;
        bool                         bBuildStarted = false;
        Jobs::FTaskEvent             Built;
        // Left empty when the capture saw nothing.
        FMeshImpostor                Impostor;
        float                        CoveredShare = 0.0f;
    };

    void FImpostorBakerTestAccess::MarkActive(FImpostorBaker& Baker, CStaticMesh* Mesh)
    {
        TUniquePtr<FImpostorBaker::FJob> Job = MakeUnique<FImpostorBaker::FJob>();
        Job->Mesh    = TWeakObjectPtr<CStaticMesh>(Mesh);
        Baker.Active = Move(Job);
    }

    void FImpostorBakerTestAccess::MarkBuilding(FImpostorBaker& Baker, CStaticMesh* Mesh)
    {
        TUniquePtr<FImpostorBaker::FJob> Job = MakeUnique<FImpostorBaker::FJob>();
        Job->Mesh = TWeakObjectPtr<CStaticMesh>(Mesh);
        Baker.Building.Add(Move(Job));
    }

    void FImpostorBakerTestAccess::Reset(FImpostorBaker& Baker)
    {
        Baker.Active.Reset();
        Baker.Building.Empty();
    }

    FImpostorBaker::FImpostorBaker() = default;

    FImpostorBaker::~FImpostorBaker()
    {
        for (TUniquePtr<FJob>& Job : Building)
        {
            if (Job->bBuildStarted)
            {
                Job->Built.Wait();
            }
            else
            {
                RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), Job->CopyValue);
            }
            for (RHI::FGPUAllocation& Readback : Job->Readbacks)
            {
                RHI::Retire(Readback);
            }
        }
    }

    void FImpostorBaker::Enqueue(CStaticMesh* Mesh)
    {
        if (Mesh == nullptr || Algo::AnyOf(Queue, [Mesh](const TWeakObjectPtr<CStaticMesh>& Queued) { return Queued.Get() == Mesh; })
            || (Active != nullptr && Active->Mesh.Get() == Mesh)
            || Algo::AnyOf(Building, [Mesh](const TUniquePtr<FJob>& Job) { return Job->Mesh.Get() == Mesh; }))
        {
            return;
        }
        Queue.Add(TWeakObjectPtr<CStaticMesh>(Mesh));
    }

    void FImpostorBaker::EnqueueFoliage(CWorld& World)
    {
        for (ECS::FEntity Entity : World.View<SForestComponent>())
        {
            for (const SForestSpecies& Species : World.GetComponent<SForestComponent>(Entity).Species)
            {
                Enqueue(Species.Type.Mesh.Get());
            }
        }
        for (ECS::FEntity Entity : World.View<SFoliageComponent>())
        {
            for (const SFoliageType& Type : World.GetComponent<SFoliageComponent>(Entity).Types)
            {
                Enqueue(Type.Mesh.Get());
            }
        }
        LOG_INFO("Impostor bake queued {} meshes.", Queue.Num());
    }

    void FImpostorBaker::EnqueueStaticMeshes(CWorld& World)
    {
        for (ECS::FEntity Entity : World.View<SStaticMeshComponent>())
        {
            const SStaticMeshComponent& Component = World.GetComponent<SStaticMeshComponent>(Entity);
            CStaticMesh*    Mesh    = Component.GetStaticMesh();
            const CPackage* Package = Mesh != nullptr ? Mesh->GetPackage() : nullptr;
            if (!Component.bAllowImpostor || Package == nullptr)
            {
                continue;
            }
            // Engine meshes such as the default ground cube are shared by every project, so a scene bake leaves them alone.
            const FFixedString PackagePath = Package->GetPackagePath();
            if (!FStringView(*PackagePath, PackagePath.Len()).StartsWith("/Engine/"))
            {
                Enqueue(Mesh);
            }
        }
        LOG_INFO("Impostor bake queued {} meshes.", Queue.Num());
    }

    bool FImpostorBaker::StartNext()
    {
        using namespace ImpostorBakerInternal;

        while (!Queue.IsEmpty())
        {
            CStaticMesh* Mesh = Queue[0].Get();
            Queue.RemoveAt(0);
            if (Mesh == nullptr || Mesh->TryGetMeshResource() == nullptr || Mesh->TryGetMeshResource()->bSkinnedMesh)
            {
                continue;
            }

            const SAABB& Bounds = Mesh->GetAABB();
            TUniquePtr<FJob> Job = MakeUnique<FJob>();
            Job->Mesh       = TWeakObjectPtr<CStaticMesh>(Mesh);
            Job->Layout     = MakeLayout(Mesh->ImpostorSettings);
            Job->Center     = (Bounds.Min + Bounds.Max) * 0.5f;
            Job->Radius     = Math::Length(Bounds.Max - Job->Center);
            if (!(Job->Radius > 0.0f))
            {
                continue;
            }

            if (AcquireScene(Job->Layout.GetRenderSize()) == nullptr)
            {
                return false;
            }
            Scene->ResetContents();
            CWorld* World = Scene->GetWorld();

            // Every view's copy stands in its own cell of a grid the camera looks straight down on, turned so the view faces up.
            const uint32 Views = Job->Layout.Frames;
            const float  Cell    = 2.0f * Job->Radius;
            for (uint32 J = 0; J < Views; ++J)
            {
                for (uint32 I = 0; I < Views; ++I)
                {
                    const FQuat    Rotation = FaceViewUp(Impostor::FrameDirection(I, J, Views, Job->Layout.bFullSphere));
                    const FVector3 CellCenter(((float)I + 0.5f - (float)Views * 0.5f) * Cell, 0.0f, ((float)J + 0.5f - (float)Views * 0.5f) * Cell);
                    Job->Rotations.Add(Rotation);
                    Job->CellCenters.Add(CellCenter);

                    FTransform Transform;
                    Transform.SetRotation(Rotation);
                    Transform.SetLocation(CellCenter - Rotation * Job->Center);
                    const ECS::FEntity Copy = Scene->SpawnEntity("ImpostorView", Transform);
                    World->EmplaceComponent<SStaticMeshComponent>(Copy).SetStaticMesh(Mesh);
                }
            }
            Scene->SetCameraOrthographic(FVector3(0.0f, 4.0f * Job->Radius, 0.0f), FVector3(0.0f, -1.0f, 0.0f), FVector3(0.0f, 0.0f, 1.0f),
                                              (float)Views * Cell);

            Active = Move(Job);
            return true;
        }
        return false;
    }

    FThumbnailScene* FImpostorBaker::AcquireScene(uint32 RenderSize)
    {
        if (Scene != nullptr && Scene->GetRenderTargetSize() == RenderSize)
        {
            return Scene.Get();
        }

        // Only reached with no copy pending, so no readback still reads the old targets.
        Scene.Reset();
        Scene = MakeUnique<FThumbnailScene>(RenderSize);
        Scene->Begin();
        CWorld* World = Scene->GetWorld();
        if (World == nullptr)
        {
            Scene.Reset();
            return nullptr;
        }

        SWorldRenderSettings& Settings = World->GetRenderSettings();
        Settings.bExactCapture = true;
        Settings.bUseLODs      = false;
        Settings.bFrustumCull  = false;
        return Scene.Get();
    }

    void FImpostorBaker::QueueMissingFoliageImpostors()
    {
        FString Error;
        CWorld* World = SessionOps::IsSimulating() ? nullptr : SessionOps::GetSceneWorld(Error);
        if (World == nullptr)
        {
            return;
        }

        uint32 Added = 0;
        auto QueueIfMissing = [&](CStaticMesh* Mesh)
        {
            if (Mesh == nullptr || Mesh->HasImpostor() || Mesh->TryGetMeshResource() == nullptr
                || Algo::AnyOf(AutoQueued, [Mesh](const TWeakObjectPtr<CStaticMesh>& Seen) { return Seen.Get() == Mesh; }))
            {
                return;
            }
            AutoQueued.Add(TWeakObjectPtr<CStaticMesh>(Mesh));
            Enqueue(Mesh);
            ++Added;
        };

        for (ECS::FEntity Entity : World->View<SForestComponent>())
        {
            for (const SForestSpecies& Species : World->GetComponent<SForestComponent>(Entity).Species)
            {
                QueueIfMissing(Species.Type.Mesh.Get());
            }
        }
        for (ECS::FEntity Entity : World->View<SFoliageComponent>())
        {
            for (const SFoliageType& Type : World->GetComponent<SFoliageComponent>(Entity).Types)
            {
                QueueIfMissing(Type.Mesh.Get());
            }
        }
        if (Added != 0u)
        {
            LOG_INFO("Impostor bake queued {} foliage meshes that had none. Save them to keep the bakes.", Added);
        }
    }

    void FImpostorBaker::Tick()
    {
        using namespace ImpostorBakerInternal;

        PollBuild();
        // A render now would overwrite the images a copy is still reading.
        if (Algo::AnyOf(Building, [](const TUniquePtr<FJob>& Job) { return !Job->bBuildStarted; }))
        {
            return;
        }

        if (Active == nullptr && Queue.IsEmpty())
        {
            if (FramesToAutoScan == 0u)
            {
                FramesToAutoScan = kAutoScanIntervalFrames;
                QueueMissingFoliageImpostors();
            }
            --FramesToAutoScan;
        }

        if (Active == nullptr && !StartNext())
        {
            // Its render targets are large, so the world lives only while a batch has meshes left.
            Scene.Reset();
            return;
        }

        FJob& Job = *Active;
        if (Job.Mesh.Get() == nullptr)
        {
            Active.Reset();
            return;
        }

        auto IsWarm = [](const FJob& InJob)
        {
            const FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
            return InJob.Frames >= kMaxWarmFrames || (InJob.Frames >= kMinWarmFrames && (Streaming == nullptr || Streaming->GetStats().NumLoadsInFlight == 0));
        };
        if (!IsWarm(Job))
        {
            if (!Scene->RenderFrame())
            {
                Active.Reset();
                return;
            }
            ++Job.Frames;
            if (!IsWarm(Job))
            {
                return;
            }
        }

        // With every build slot taken this one holds its images and renders nothing more.
        if (Building.Num() < kMaxBuildsInFlight)
        {
            Capture();
        }
    }

    void FImpostorBaker::Capture()
    {
        using namespace ImpostorBakerInternal;

        FJob& Job = *Active;
        auto* Renderer = static_cast<FDefaultSceneRenderer*>(Scene->GetWorld()->GetRenderer());

        const uint32   RenderSize = Job.Layout.GetRenderSize();
        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::FragmentTests | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::DepthStencilWrite,
            RHI::EStageFlags::Transfer,
            RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
        for (uint32 Index = 0; Index < kReadCount; ++Index)
        {
            const FSceneImage& Image = Renderer->GetPrimaryNamedImage(kReadImages[Index]);
            const uint64 TexelBytes = RHI::Format::BytesPerBlock(Image.Desc.Format);
            Job.Readbacks[Index] = RHI::Malloc((uint64)RenderSize * RenderSize * TexelBytes, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            if (kReadImages[Index] == ENamedImage::GBufferB)
            {
                Job.bWideNormals = Image.Desc.Format == EFormat::RGBA16_UNORM;
            }
            RHI::SetDebugName(Job.Readbacks[Index].Gpu, "Readback.Impostor");
            RHI::CmdCopyTextureToMemory(CL, Image.Texture, RHI::FTextureSlice{}, Job.Readbacks[Index], RenderSize);
        }
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite, RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);
        Job.CopyValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{ &CL, 1 });

        const SCameraComponent& Camera = Scene->GetWorld()->GetComponent<SCameraComponent>(Scene->GetCameraEntity());
        Job.ViewProjection = Camera.GetViewVolume().GetProjectionMatrix() * Camera.GetViewVolume().GetViewMatrix();
        Building.Add(Move(Active));
    }

    void FImpostorBaker::PollBuild()
    {
        const uint64 CopiesDone = RHI::GetSemaphoreValue(RHI::GetQueueTimeline(RHI::EQueueType::Graphics));
        for (TUniquePtr<FJob>& Job : Building)
        {
            if (Job->bBuildStarted || CopiesDone < Job->CopyValue)
            {
                continue;
            }
            Job->bBuildStarted = true;
            Jobs::Spawn(Jobs::EJobPriority::Low, Job->Built, [](FJob* InJob) -> Jobs::TTask<void>
            {
                BuildAtlas(*InJob);
                co_return;
            }, Job.Get());
        }

        Building.RemoveAll([this](TUniquePtr<FJob>& Job)
        {
            if (!Job->bBuildStarted || !Job->Built.IsSignaled())
            {
                return false;
            }
            Apply(*Job);
            return true;
        });
    }

    void FImpostorBaker::BuildAtlas(FJob& Job)
    {
        using namespace ImpostorBakerInternal;

        const FBakeLayout& Layout      = Job.Layout;
        const uint32       Views     = Layout.Frames;
        const uint32       ViewSize  = Layout.FrameSize;
        const uint32       Samples    = Layout.Supersample;
        const uint32       Size        = Layout.GetRenderSize();
        const uint32*      Vis         = reinterpret_cast<const uint32*>(Job.Readbacks[0].Cpu);
        const uint8*       Albedo      = reinterpret_cast<const uint8*>(Job.Readbacks[1].Cpu);
        const uint8*       Normal      = reinterpret_cast<const uint8*>(Job.Readbacks[2].Cpu);
        const float*       Depth       = reinterpret_cast<const float*>(Job.Readbacks[3].Cpu);
        const uint8*       Material    = reinterpret_cast<const uint8*>(Job.Readbacks[4].Cpu);
        const uint32*      EmissiveRaw = reinterpret_cast<const uint32*>(Job.Readbacks[5].Cpu);

        const FMatrix4& ViewProjection   = Job.ViewProjection;
        const FMatrix4 InvViewProjection = Math::Inverse(ViewProjection);

        const uint32 AtlasSize = Layout.GetAtlasSize();
        const float  Diameter  = 2.0f * Job.Radius;
        TVector<FTexel> Texels;
        Texels.SetNum(AtlasSize * AtlasSize);
        TVector<uint32> CoveredPerView;
        CoveredPerView.SetNumZeroed(Views * Views);
        TVector<float> BrightestPerView;
        BrightestPerView.SetNumZeroed(Views * Views);

        // Views never read across their edges, so each one resamples and dilates on its own worker, below the editor's frame work.
        Jobs::ParallelFor(Views * Views, [&](uint32 View)
        {
            const uint32   I          = View % Views;
            const uint32   J          = View / Views;
            const FQuat&   Rotation   = Job.Rotations[View];
            const FQuat    Unrotate   = Math::Inverse(Rotation);
            const FVector3 CellCenter = Job.CellCenters[View];
            const FVector3 Dir        = Impostor::FrameDirection(I, J, Views, Layout.bFullSphere);
            FVector3 Right, Up;
            Impostor::FrameBasis(Dir, Right, Up);

            for (uint32 T = 0; T < ViewSize; ++T)
            {
                for (uint32 S = 0; S < ViewSize; ++S)
                {
                    FVector3 Color(0.0f);
                    FVector3 Normals(0.0f);
                    float    Depths = 0.0f;
                    float    AO     = 0.0f;
                    FVector3 Emissive(0.0f);
                    uint32   Hits   = 0;
                    for (uint32 B = 0; B < Samples; ++B)
                    {
                        for (uint32 A = 0; A < Samples; ++A)
                        {
                            // The same point of the view's plane the draw reads at this UV, placed through the copy's turn into the render.
                            const float U = ((float)S + ((float)A + 0.5f) / (float)Samples) / (float)ViewSize;
                            const float V = ((float)T + ((float)B + 0.5f) / (float)Samples) / (float)ViewSize;
                            const FVector3 Local = Job.Center + (Right * (U - 0.5f) + Up * (V - 0.5f)) * Diameter;
                            const FVector3 World = CellCenter + Rotation * (Local - Job.Center);
                            const FVector4 Clip  = ViewProjection * FVector4(World, 1.0f);
                            const float PX = (Clip.x / Clip.w * 0.5f + 0.5f) * (float)Size;
                            const float PY = (Clip.y / Clip.w * 0.5f + 0.5f) * (float)Size;
                            if (PX < 0.0f || PY < 0.0f || PX >= (float)Size || PY >= (float)Size)
                            {
                                continue;
                            }
                            const uint32 Pixel = (uint32)PY * Size + (uint32)PX;
                            if (Vis[Pixel] == 0u)
                            {
                                continue;
                            }

                            ++Hits;
                            Color   += FVector3((float)Albedo[Pixel * 4u], (float)Albedo[Pixel * 4u + 1u], (float)Albedo[Pixel * 4u + 2u]) / 255.0f;
                            AO      += (float)Albedo[Pixel * 4u + 3u] / 255.0f;
                            Normals += Unrotate * DecodeGBufferNormal(Normal, Pixel, Job.bWideNormals);
                            // Foliage spends GBufferD on transmission, which is not light the impostor gives off.
                            const uint32 ShadingModel = ((uint32)Material[Pixel * 4u + 3u] >> kShadingModelShift) & kShadingModelMask;
                            if (ShadingModel != (uint32)EMaterialShadingModel::Foliage)
                            {
                                Emissive += DecodeR11G11B10(EmissiveRaw[Pixel]);
                            }

                            const FVector4 Surface = InvViewProjection * FVector4(Clip.x / Clip.w, Clip.y / Clip.w, Depth[Pixel], 1.0f);
                            const FVector3 SurfaceLocal = Unrotate * (FVector3(Surface.x, Surface.y, Surface.z) / Surface.w - CellCenter);
                            Depths  += Math::Clamp(Math::Dot(SurfaceLocal, Dir) / Job.Radius, -1.0f, 1.0f) * 0.5f + 0.5f;
                        }
                    }

                    FTexel& Out = Texels[(J * ViewSize + T) * AtlasSize + I * ViewSize + S];
                    Out.Coverage = (float)Hits / (float)(Samples * Samples);
                    if (Hits > 0u)
                    {
                        const float Inv = 1.0f / (float)Hits;
                        Out.Color   = Color * Inv;
                        Out.AO      = AO * Inv;
                        Out.Depth   = Depths * Inv;
                        Out.Normal   = Math::Length(Normals) > 1e-6f ? Math::Normalize(Normals) : Dir;
                        Out.Emissive = Emissive * Inv;
                        Out.bFilled  = true;
                        ++CoveredPerView[View];
                        BrightestPerView[View] = Math::Max(BrightestPerView[View], Math::Max(Out.Emissive.x, Math::Max(Out.Emissive.y, Out.Emissive.z)));
                    }
                }
            }
            DilateView(Texels, AtlasSize, ViewSize, I * ViewSize, J * ViewSize);
        }, 0, Jobs::EJobPriority::Low);

        const uint32 Covered = Algo::Accumulate(CoveredPerView, 0u);
        if (Covered == 0u)
        {
            return;
        }

        FMeshImpostor& Impostor = Job.Impostor;
        const float Brightest = *Algo::MaxElement(BrightestPerView);
        Impostor.Frames        = Views;
        Impostor.FrameSize     = ViewSize;
        Impostor.Center        = Job.Center;
        Impostor.Radius        = Job.Radius;
        Impostor.bFullSphere   = Layout.bFullSphere;
        Impostor.EmissiveScale = Brightest > kMinEmissive ? Brightest : 0.0f;
        AppendMip(Texels, Impostor);
        for (uint32 Side = AtlasSize; Side > Views; Side /= 2u)
        {
            Texels = Downsample(Texels, Side);
            AppendMip(Texels, Impostor);
        }

        Job.CoveredShare = (float)Covered / (float)(AtlasSize * AtlasSize);
    }

    void FImpostorBaker::Apply(FJob& Job)
    {
        using namespace ImpostorBakerInternal;

        for (RHI::FGPUAllocation& Readback : Job.Readbacks)
        {
            RHI::Retire(Readback);
        }

        CStaticMesh* Mesh = Job.Mesh.Get();
        if (Mesh == nullptr)
        {
            return;
        }
        if (Job.Impostor.BaseColor.IsEmpty())
        {
            LOG_WARN("Impostor bake for '{}' saw nothing, so the mesh keeps no impostor.", Mesh->GetName().ToString());
            return;
        }

        Mesh->SetImpostor(Move(Job.Impostor));
        if (CPackage* Package = Mesh->GetPackage())
        {
            Package->MarkDirty();
        }
        LOG_INFO("Baked a {}x{} impostor at {} texels a view for '{}'{}{} after {} frames, {:.0f}% of its texels covered. Save it to keep the bake.",
                 Job.Layout.Frames, Job.Layout.Frames, Job.Layout.FrameSize, Mesh->GetName().ToString(),
                 Job.Layout.bFullSphere ? ", full sphere" : "", Job.Impostor.HasEmissive() ? ", with emission" : "",
                 Job.Frames, 100.0f * Job.CoveredShare);
    }
}
