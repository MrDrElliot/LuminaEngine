#include "ImpostorBaker.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Package/Package.h"
#include "Log/Log.h"
#include "Renderer/MeshImpostor.h"
#include "Renderer/RHI.h"
#include "Renderer/TextureStreamingManager.h"
#include "Renderer/Vertex.h"
#include "Session/SessionOps.h"
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
        constexpr uint32 kFrames        = 8;
        constexpr uint32 kFrameSize     = 128;
        // Render texels per atlas texel along each axis, averaged so leaves and silhouettes resolve smoothly.
        constexpr uint32 kSupersample   = 2;
        // Streamed textures answer the capture's own feedback, so it renders a feedback round trip at least, and stops waiting on a loader that never goes quiet.
        constexpr uint32 kMinWarmFrames = 8;
        constexpr uint32 kMaxWarmFrames = 240;
        constexpr int32  kDilatePasses  = 8;
        // Editor frames between looks for forest meshes that still need a bake.
        constexpr uint32 kAutoScanIntervalFrames = 300;

        using ENamedImage = FDefaultSceneRenderer::ENamedImage;
        constexpr uint32      kReadCount = 4;
        constexpr ENamedImage kReadImages[kReadCount] = { ENamedImage::VisBuffer, ENamedImage::GBufferA, ENamedImage::GBufferB, ENamedImage::DepthAttachment };

        struct FTexel
        {
            FVector3 Color    = FVector3(0.0f);
            float    Coverage = 0.0f;
            FVector3 Normal   = FVector3(0.0f, 1.0f, 0.0f);
            float    Depth    = 0.5f;
            float    AO       = 1.0f;
            bool     bFilled  = false;
        };

        // GBufferB is R10G10B10A2 with the octahedral normal in its first two channels, as OctEncode01 in Common.slang writes it.
        FVector3 DecodeGBufferNormal(uint32 Packed)
        {
            return OctDecode(FVector2((float)(Packed & 1023u), (float)((Packed >> 10) & 1023u)) / 1023.0f * 2.0f - FVector2(1.0f));
        }

        FQuat RotationOnto(const FVector3& From, const FVector3& To)
        {
            const FVector3 Axis = Math::Cross(From, To);
            if (Math::Length(Axis) < 1e-6f)
            {
                return FQuat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            return Math::FromAxisAngle(Axis, Math::Acos(Math::Clamp(Math::Dot(From, To), -1.0f, 1.0f)));
        }

        uint8 ToByte(float Value)
        {
            return (uint8)Math::Clamp((int32)(Value * 255.0f + 0.5f), 0, 255);
        }

        // Empty texels take their covered neighbors' attributes, so filtering at a silhouette never pulls in black.
        void Dilate(TVector<FTexel>& Texels, uint32 AtlasSize, uint32 FrameSize)
        {
            for (int32 Pass = 0; Pass < kDilatePasses; ++Pass)
            {
                TVector<FTexel> Next = Texels;
                for (uint32 Y = 0; Y < AtlasSize; ++Y)
                {
                    for (uint32 X = 0; X < AtlasSize; ++X)
                    {
                        FTexel& Out = Next[Y * AtlasSize + X];
                        if (Out.bFilled)
                        {
                            continue;
                        }

                        FVector3 Color(0.0f);
                        FVector3 Normal(0.0f);
                        float    Depth = 0.0f;
                        float    AO    = 0.0f;
                        int32    Count = 0;
                        const int32 Offsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                        for (const auto& Offset : Offsets)
                        {
                            const int32 NX = (int32)X + Offset[0];
                            const int32 NY = (int32)Y + Offset[1];
                            // Neighbors across a view's edge belong to another view.
                            if (NX < 0 || NY < 0 || NX >= (int32)AtlasSize || NY >= (int32)AtlasSize
                                || (uint32)NX / FrameSize != X / FrameSize || (uint32)NY / FrameSize != Y / FrameSize)
                            {
                                continue;
                            }
                            const FTexel& N = Texels[NY * AtlasSize + NX];
                            if (!N.bFilled)
                            {
                                continue;
                            }
                            Color  += N.Color;
                            Normal += N.Normal;
                            Depth  += N.Depth;
                            AO     += N.AO;
                            ++Count;
                        }
                        if (Count > 0)
                        {
                            const float Inv = 1.0f / (float)Count;
                            Out.Color   = Color * Inv;
                            Out.Normal  = Math::Length(Normal) > 1e-6f ? Math::Normalize(Normal) : FVector3(0.0f, 1.0f, 0.0f);
                            Out.Depth   = Depth * Inv;
                            Out.AO      = AO * Inv;
                            Out.bFilled = true;
                        }
                    }
                }
                Texels = Move(Next);
            }
        }

        // Each coarser texel weighs its four by coverage, so a half-empty one keeps the color of what is there.
        TVector<FTexel> Downsample(const TVector<FTexel>& Texels, uint32 Size)
        {
            const uint32 Half = Size / 2u;
            TVector<FTexel> Out;
            Out.SetNum(Half * Half);
            for (uint32 Y = 0; Y < Half; ++Y)
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
                        R.Coverage += T->Coverage * 0.25f;
                    }
                    R.Normal  = Math::Length(R.Normal) > 1e-6f ? Math::Normalize(R.Normal) : FVector3(0.0f, 1.0f, 0.0f);
                    R.bFilled = true;
                }
            }
            return Out;
        }

        void AppendMip(const TVector<FTexel>& Texels, FMeshImpostor& Out)
        {
            for (const FTexel& T : Texels)
            {
                Out.BaseColor.Append({ ToByte(T.Color.x), ToByte(T.Color.y), ToByte(T.Color.z), ToByte(T.Coverage) });
                const FVector2 Oct = OctEncode(T.Normal) * 0.5f + FVector2(0.5f);
                Out.NormalDepth.Append({ ToByte(Oct.x), ToByte(Oct.y), ToByte(T.Depth), ToByte(T.AO) });
            }
        }
    }

    struct FImpostorBaker::FJob
    {
        TWeakObjectPtr<CStaticMesh>  Mesh;
        TUniquePtr<FThumbnailScene>  Scene;
        FVector3                     Center = FVector3(0.0f);
        float                        Radius = 0.0f;
        // Per view, row-major over the grid, the turn that faces the view up at the camera and where its copy stands.
        TVector<FQuat>               Rotations;
        TVector<FVector3>            CellCenters;
        uint32                       RenderSize = 0;
        uint32                       Frames     = 0;
    };

    FImpostorBaker::FImpostorBaker() = default;
    FImpostorBaker::~FImpostorBaker() = default;

    void FImpostorBaker::Enqueue(CStaticMesh* Mesh)
    {
        if (Mesh == nullptr || Algo::AnyOf(Queue, [Mesh](const TWeakObjectPtr<CStaticMesh>& Queued) { return Queued.Get() == Mesh; }))
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
            Job->Center     = (Bounds.Min + Bounds.Max) * 0.5f;
            Job->Radius     = Math::Length(Bounds.Max - Job->Center);
            Job->RenderSize = kFrames * kFrameSize * kSupersample;
            if (!(Job->Radius > 0.0f))
            {
                continue;
            }

            Job->Scene = MakeUnique<FThumbnailScene>(Job->RenderSize);
            Job->Scene->Begin();
            CWorld* World = Job->Scene->GetWorld();
            SWorldRenderSettings& Settings = World->GetRenderSettings();
            Settings.bExactCapture = true;
            Settings.bUseLODs      = false;
            Settings.bFrustumCull  = false;

            // Every view's copy stands in its own cell of a grid the camera looks straight down on, turned so the view faces up.
            const float Cell = 2.0f * Job->Radius;
            for (uint32 J = 0; J < kFrames; ++J)
            {
                for (uint32 I = 0; I < kFrames; ++I)
                {
                    const FQuat    Rotation = RotationOnto(Impostor::FrameDirection(I, J, kFrames), FVector3(0.0f, 1.0f, 0.0f));
                    const FVector3 CellCenter(((float)I + 0.5f - (float)kFrames * 0.5f) * Cell, 0.0f, ((float)J + 0.5f - (float)kFrames * 0.5f) * Cell);
                    Job->Rotations.Add(Rotation);
                    Job->CellCenters.Add(CellCenter);

                    FTransform Transform;
                    Transform.SetRotation(Rotation);
                    Transform.SetLocation(CellCenter - Rotation * Job->Center);
                    const ECS::FEntity Copy = World->ConstructEntity("ImpostorView", Transform);
                    World->EmplaceComponent<SStaticMeshComponent>(Copy).SetStaticMesh(Mesh);
                }
            }
            Job->Scene->SetCameraOrthographic(FVector3(0.0f, 4.0f * Job->Radius, 0.0f), FVector3(0.0f, -1.0f, 0.0f), FVector3(0.0f, 0.0f, 1.0f),
                                              (float)kFrames * Cell);

            Active = Move(Job);
            return true;
        }
        return false;
    }

    void FImpostorBaker::QueueMissingForestImpostors()
    {
        FString Error;
        CWorld* World = SessionOps::IsSimulating() ? nullptr : SessionOps::GetSceneWorld(Error);
        if (World == nullptr)
        {
            return;
        }

        uint32 Added = 0;
        for (ECS::FEntity Entity : World->View<SForestComponent>())
        {
            for (const SForestSpecies& Species : World->GetComponent<SForestComponent>(Entity).Species)
            {
                CStaticMesh* Mesh = Species.Type.Mesh.Get();
                if (Mesh == nullptr || Mesh->HasImpostor() || Mesh->TryGetMeshResource() == nullptr
                    || Algo::AnyOf(AutoQueued, [Mesh](const TWeakObjectPtr<CStaticMesh>& Seen) { return Seen.Get() == Mesh; }))
                {
                    continue;
                }
                AutoQueued.Add(TWeakObjectPtr<CStaticMesh>(Mesh));
                Enqueue(Mesh);
                ++Added;
            }
        }
        if (Added != 0u)
        {
            LOG_INFO("Impostor bake queued {} forest meshes that had none. Save them to keep the bakes.", Added);
        }
    }

    void FImpostorBaker::Tick()
    {
        using namespace ImpostorBakerInternal;

        if (Active == nullptr && Queue.IsEmpty())
        {
            if (FramesToAutoScan == 0u)
            {
                FramesToAutoScan = kAutoScanIntervalFrames;
                QueueMissingForestImpostors();
            }
            --FramesToAutoScan;
        }

        if (Active == nullptr && !StartNext())
        {
            return;
        }

        FJob& Job = *Active;
        if (Job.Mesh.Get() == nullptr || !Job.Scene->RenderFrame())
        {
            Active.Reset();
            return;
        }

        ++Job.Frames;
        const FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
        const bool bSettled = Job.Frames >= kMinWarmFrames && (Streaming == nullptr || Streaming->GetStats().NumLoadsInFlight == 0);
        if (!bSettled && Job.Frames < kMaxWarmFrames)
        {
            return;
        }

        Finish(Job);
        Active.Reset();
    }

    void FImpostorBaker::Finish(FJob& Job)
    {
        using namespace ImpostorBakerInternal;

        CStaticMesh* Mesh = Job.Mesh.Get();
        auto* Renderer = static_cast<FDefaultSceneRenderer*>(Job.Scene->GetWorld()->GetRenderer());
        const uint32 Size = Job.RenderSize;

        RHI::FGPUAllocation Readbacks[kReadCount] = {};
        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::FragmentTests | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::DepthStencilWrite,
            RHI::EStageFlags::Transfer,
            RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
        for (uint32 Index = 0; Index < kReadCount; ++Index)
        {
            const FSceneImage& Image = Renderer->GetPrimaryNamedImage(kReadImages[Index]);
            Readbacks[Index] = RHI::Malloc((uint64)Size * Size * 4u, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Readbacks[Index].Gpu, "Readback.Impostor");
            RHI::CmdCopyTextureToMemory(CL, Image.Texture, RHI::FTextureSlice{}, Readbacks[Index], Size);
        }
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite, RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{ &CL, 1 }));

        const uint32* Vis    = reinterpret_cast<const uint32*>(Readbacks[0].Cpu);
        const uint8*  Albedo = reinterpret_cast<const uint8*>(Readbacks[1].Cpu);
        const uint32* Normal = reinterpret_cast<const uint32*>(Readbacks[2].Cpu);
        const float*  Depth  = reinterpret_cast<const float*>(Readbacks[3].Cpu);

        const SCameraComponent& Camera = Job.Scene->GetWorld()->GetComponent<SCameraComponent>(Job.Scene->GetCameraEntity());
        const FMatrix4 ViewProjection    = Camera.GetViewVolume().GetProjectionMatrix() * Camera.GetViewVolume().GetViewMatrix();
        const FMatrix4 InvViewProjection = Math::Inverse(ViewProjection);

        const uint32 AtlasSize = kFrames * kFrameSize;
        const float  Diameter  = 2.0f * Job.Radius;
        TVector<FTexel> Texels;
        Texels.SetNum(AtlasSize * AtlasSize);
        uint32 Covered = 0;

        for (uint32 J = 0; J < kFrames; ++J)
        {
            for (uint32 I = 0; I < kFrames; ++I)
            {
                const uint32   View       = J * kFrames + I;
                const FQuat&   Rotation   = Job.Rotations[View];
                const FQuat    Unrotate   = Math::Inverse(Rotation);
                const FVector3 CellCenter = Job.CellCenters[View];
                const FVector3 Dir        = Impostor::FrameDirection(I, J, kFrames);
                FVector3 Right, Up;
                Impostor::FrameBasis(Dir, Right, Up);

                for (uint32 T = 0; T < kFrameSize; ++T)
                {
                    for (uint32 S = 0; S < kFrameSize; ++S)
                    {
                        FVector3 Color(0.0f);
                        FVector3 Normals(0.0f);
                        float    Depths = 0.0f;
                        float    AO     = 0.0f;
                        uint32   Hits   = 0;
                        for (uint32 B = 0; B < kSupersample; ++B)
                        {
                            for (uint32 A = 0; A < kSupersample; ++A)
                            {
                                // The same point of the view's plane the draw reads at this UV, placed through the copy's turn into the render.
                                const float U = ((float)S + ((float)A + 0.5f) / (float)kSupersample) / (float)kFrameSize;
                                const float V = ((float)T + ((float)B + 0.5f) / (float)kSupersample) / (float)kFrameSize;
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
                                Normals += Unrotate * DecodeGBufferNormal(Normal[Pixel]);

                                const FVector4 Surface = InvViewProjection * FVector4(Clip.x / Clip.w, Clip.y / Clip.w, Depth[Pixel], 1.0f);
                                const FVector3 SurfaceLocal = Unrotate * (FVector3(Surface.x, Surface.y, Surface.z) / Surface.w - CellCenter);
                                Depths  += Math::Clamp(Math::Dot(SurfaceLocal, Dir) / Job.Radius, -1.0f, 1.0f) * 0.5f + 0.5f;
                            }
                        }

                        FTexel& Out = Texels[(J * kFrameSize + T) * AtlasSize + I * kFrameSize + S];
                        Out.Coverage = (float)Hits / (float)(kSupersample * kSupersample);
                        if (Hits > 0u)
                        {
                            const float Inv = 1.0f / (float)Hits;
                            Out.Color   = Color * Inv;
                            Out.AO      = AO * Inv;
                            Out.Depth   = Depths * Inv;
                            Out.Normal  = Math::Length(Normals) > 1e-6f ? Math::Normalize(Normals) : Dir;
                            Out.bFilled = true;
                            ++Covered;
                        }
                    }
                }
            }
        }

        for (RHI::FGPUAllocation& Readback : Readbacks)
        {
            RHI::Retire(Readback);
        }

        if (Covered == 0u)
        {
            LOG_WARN("Impostor bake for '{}' saw nothing, so the mesh keeps no impostor.", Mesh->GetName().ToString());
            return;
        }

        Dilate(Texels, AtlasSize, kFrameSize);

        FMeshImpostor Impostor;
        Impostor.Frames    = kFrames;
        Impostor.FrameSize = kFrameSize;
        Impostor.Center    = Job.Center;
        Impostor.Radius    = Job.Radius;
        AppendMip(Texels, Impostor);
        for (uint32 Side = AtlasSize; Side > kFrames; Side /= 2u)
        {
            Texels = Downsample(Texels, Side);
            AppendMip(Texels, Impostor);
        }

        Mesh->SetImpostor(Move(Impostor));
        if (CPackage* Package = Mesh->GetPackage())
        {
            Package->MarkDirty();
        }
        LOG_INFO("Baked a {}x{} impostor for '{}' after {} frames, {:.0f}% of its texels covered. Save it to keep the bake.",
                 kFrames, kFrames, Mesh->GetName().ToString(), Job.Frames, 100.0f * (float)Covered / (float)(AtlasSize * AtlasSize));
    }
}
