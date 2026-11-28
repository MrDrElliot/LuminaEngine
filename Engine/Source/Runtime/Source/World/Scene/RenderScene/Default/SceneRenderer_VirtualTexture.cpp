#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "Assets/AssetTypes/Textures/RuntimeVirtualTexture.h"
#include "Containers/HashPrimitives.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/VirtualTextureComponent.h"

namespace Lumina
{
    namespace SceneRendererVirtualTextureInternal
    {
        using FFrameData    = FDefaultSceneRenderer::FFrameData;
        using EHeightChange = FDefaultSceneRenderer::FFrameData::EHeightChange;

        // A writer that jumps farther than this between frames teleported, so it starts a new trail instead of painting a line across the map.
        constexpr float  kWriterTeleportDistance = 50.0f;

        // A capture redraws the whole window over this many frames, which keeps up with page streaming and material changes.
        constexpr uint32 kRefreshFrames = 64u;

        // After a terrain edit the window redraws this much faster, for this many frames.
        constexpr uint32 kFastRefreshFrames = 8u;

        // One stamp covers at most this many 8 texel tiles a side, 512 texels.
        constexpr uint32 kMaxStampTiles = 64u;

        // An 8-bit channel cannot move by less than one step, so decay waits until it adds up to one.
        constexpr float  kDecayStepRGBA8 = 1.0f / 255.0f;

        // Must match FVirtualTextureWriteArgs in VirtualTextureWrite.slang.
        struct FVirtualTextureWriteArgs
        {
            uint32      Target;
            uint32      Mode;
            uint32      Resolution;
            float       TexelSize;
            FIntVector2 RectMin;
            FUIntVector2 RectSize;
            FVector4    Clear;
            float       Amount;
            uint32      StampTiles;
            FIntVector2 WindowOrigin;
            RHI::TGPUSpan<FVirtualTextureStampGPU> Stamps;
        };
        static_assert(offsetof(FVirtualTextureWriteArgs, Clear) == 32 && offsetof(FVirtualTextureWriteArgs, Stamps) == 64,
                      "FVirtualTextureWriteArgs must match VirtualTextureWrite.slang.");

        // Must match FVirtualTextureCaptureArgs in TerrainVirtualTextureCapture.slang.
        struct FVirtualTextureCaptureArgs
        {
            uint32       Target;
            uint32       Resolution;
            float        TexelSize;
            uint32       TerrainIndex;
            FIntVector2  RectMin;
            FUIntVector2 RectSize;
            uint32       Layout;
            float        Mip;
        };
        static_assert(sizeof(FVirtualTextureCaptureArgs) == 40, "FVirtualTextureCaptureArgs must match TerrainVirtualTextureCapture.slang.");

        enum class EWriteMode : uint32 { Clear = 0, Decay = 1, Stamp = 2 };

        FVector2 WindowCenter(const CRuntimeVirtualTexture& Texture, const FFrameData& Frame)
        {
            if (Texture.Placement == EVirtualTexturePlacement::Fixed)
            {
                return Texture.FixedCenter;
            }
            const FVector4& Camera = Frame.SceneGlobalData.CameraData.Location;
            return FVector2(Camera.x, Camera.z);
        }

        void AddStamp(FFrameData::FVirtualTextureExtract& Out, const FVector2& From, const FVector2& To, float Radius, const FVector4& Value,
                      EVirtualTextureBlend Blend, float Hardness, float Opacity)
        {
            FVirtualTextureStampGPU& Stamp = Out.Stamps.EmplaceGetRef();
            Stamp.From     = From;
            Stamp.To       = To;
            Stamp.Value    = Value;
            Stamp.Radius   = Radius;
            Stamp.Hardness = Hardness;
            Stamp.Opacity  = Opacity;
            Stamp.Blend    = (uint32)Blend;

            const FVector2 Span  = Math::Abs(To - From) + FVector2(Radius * 2.0f);
            const float    Texels = Math::Max(Span.x, Span.y) / Out.TexelSize + 2.0f;
            Out.StampTiles = Math::Max(Out.StampTiles, Math::Min((uint32)Math::Ceil(Texels / 8.0f), kMaxStampTiles));
        }
    }

    void FDefaultSceneRenderer::ExtractVirtualTextures(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SCOPE();
        using namespace SceneRendererVirtualTextureInternal;

        TVector<FFrameData::FVirtualTextureExtract>& Textures = Frame.Extracts.VirtualTextures;
        Textures.Reset();
        const float Dt = Frame.CachedWorldDeltaTime;

        auto Acquire = [&](CRuntimeVirtualTexture* Texture) -> FFrameData::FVirtualTextureExtract*
        {
            if (Texture == nullptr || !Texture->GetTextureResource().NewTexture.IsValid() || Texture->GetResourceID() < 0)
            {
                return nullptr;
            }
            const uint32 SRV = (uint32)Texture->GetResourceID();
            for (FFrameData::FVirtualTextureExtract& Existing : Textures)
            {
                if (Existing.SRV == SRV)
                {
                    return &Existing;
                }
            }

            const FTextureResource& Resource = Texture->GetTextureResource();
            FFrameData::FVirtualTextureExtract& Out = Textures.EmplaceGetRef();
            Out.Texture    = Resource.NewTexture.Texture;
            Out.UAV        = RHI::Textures::StorageSlot(Resource.NewTexture, 0);
            Out.SRV        = SRV;
            Out.Resolution = Math::Max(Resource.ImageDescription.Extent.x, 1u);
            Out.TexelSize  = Math::Max(Texture->TexelSize, 0.01f);
            Out.Clear      = Texture->ClearColor;
            Out.Origin     = VirtualTextureMath::WindowOrigin(WindowCenter(*Texture, Frame), Out.TexelSize, (int32)Out.Resolution);

            FVirtualTextureState& State = VirtualTextureStates.FindOrAdd(SRV);
            State.Asset = Texture;
            return &Out;
        };

        // Once written, a texture keeps following the view and decaying until it unloads.
        TVector<uint32> Expired;
        for (auto& [SRV, State] : VirtualTextureStates)
        {
            CRuntimeVirtualTexture* Texture = State.Asset.Get();
            if (Texture == nullptr || Acquire(Texture) == nullptr)
            {
                Expired.Add(SRV);
            }
        }
        for (uint32 SRV : Expired)
        {
            VirtualTextureStates.Remove(SRV);
        }

        TVector<FVirtualTextureStamp> Stamps;
        World->DrainVirtualTextureStamps(Stamps);
        for (const FVirtualTextureStamp& Stamp : Stamps)
        {
            if (FFrameData::FVirtualTextureExtract* Out = Acquire(Stamp.Texture.Get()))
            {
                AddStamp(*Out, Stamp.From, Stamp.To, Stamp.Radius, Stamp.Value, Stamp.Blend, Stamp.Hardness, Stamp.Opacity);
            }
        }

        THashMap<ECS::FEntity, FVector2> Positions;
        Registry.View<SVirtualTextureWriterComponent, STransformComponent>().ForEach(
            [&](ECS::FEntity Entity, const SVirtualTextureWriterComponent& Writer, const STransformComponent& Transform)
        {
            FFrameData::FVirtualTextureExtract* Out = Writer.bEnabled ? Acquire(Writer.Texture.Get()) : nullptr;
            if (Out == nullptr)
            {
                return;
            }
            const FVector3 Position = Transform.GetWorldLocation() + Writer.Offset;
            const FVector2 Here(Position.x, Position.z);
            const FVector2* Last = VirtualTextureWriterPositions.Find(Entity);
            const FVector2  From = Last != nullptr && Math::Length(Here - *Last) <= kWriterTeleportDistance ? *Last : Here;
            Positions.Add(Entity, Here);

            const bool  bRate   = Writer.Blend == EVirtualTextureBlend::Add || Writer.Blend == EVirtualTextureBlend::Subtract;
            const float Opacity = bRate ? Writer.Opacity * Dt : Writer.Opacity;
            AddStamp(*Out, From, Here, Writer.Radius, Writer.Value, Writer.Blend, Writer.Hardness, Opacity);
        });
        VirtualTextureWriterPositions = Move(Positions);

        for (int32 Index = 0; Index < Frame.Extracts.TerrainExtracts.Num(); ++Index)
        {
            const FFrameData::FTerrainExtract& Item = Frame.Extracts.TerrainExtracts[Index];
            const STerrainComponent* Terrain = Registry.TryGet<STerrainComponent>(Item.Entity);
            if (Terrain == nullptr || !Item.VirtualTextureShader)
            {
                continue;
            }
            for (const SVirtualTextureOutput& Output : Terrain->VirtualTextureOutputs)
            {
                FFrameData::FVirtualTextureExtract* Out = Acquire(Output.Texture.Get());
                if (Out == nullptr)
                {
                    continue;
                }
                Out->Captures.Add({ Index, (uint32)Output.Layout });
                if (Item.HeightChange != EHeightChange::None || Item.bStructuralChange)
                {
                    VirtualTextureStates[Out->SRV].FastRefreshFrames = kFastRefreshFrames;
                }
            }
        }

        for (FFrameData::FVirtualTextureExtract& Out : Textures)
        {
            FVirtualTextureState& State = VirtualTextureStates[Out.SRV];
            const int32 Res = (int32)Out.Resolution;
            const bool bReplaced = !State.bPlaced || State.Resolution != Out.Resolution || State.TexelSize != Out.TexelSize;
            if (bReplaced)
            {
                Out.ClearRects.Add(FIntVector4(Out.Origin.x, Out.Origin.y, Res, Res));
            }
            else if (State.Origin != Out.Origin)
            {
                VirtualTextureMath::ExposedRects(State.Origin, Out.Origin, Res, Out.ClearRects);
            }
            State.bPlaced    = true;
            State.Origin     = Out.Origin;
            State.Resolution = Out.Resolution;
            State.TexelSize  = Out.TexelSize;

            const CRuntimeVirtualTexture* Texture = State.Asset.Get();
            if (Texture != nullptr && Texture->DecayPerSecond > 0.0f && Dt > 0.0f)
            {
                State.DecayRemaining *= Math::Exp(-Texture->DecayPerSecond * Dt);
                const float Step = Texture->Format == ERenderTargetFormat::RGBA8 ? kDecayStepRGBA8 : 0.0f;
                if (1.0f - State.DecayRemaining >= Step)
                {
                    Out.DecayAmount      = 1.0f - State.DecayRemaining;
                    State.DecayRemaining = 1.0f;
                }
            }

            if (Out.Captures.IsEmpty())
            {
                continue;
            }

            uint64 Key = 0;
            for (const FFrameData::FVirtualTextureExtract::FCapture& Capture : Out.Captures)
            {
                const FFrameData::FTerrainExtract& Item = Frame.Extracts.TerrainExtracts[Capture.TerrainExtract];
                Key = Containers::CombineHash(Key, (uint64)Item.VirtualTextureShader.Handle);
                Key = Containers::CombineHash(Key, ((uint64)(uint32)Item.Entity << 8) | Capture.Layout);
            }
            if (Key != State.CaptureKey || bReplaced)
            {
                State.CaptureKey = Key;
                Out.CaptureRects.Add(FIntVector4(Out.Origin.x, Out.Origin.y, Res, Res));
                continue;
            }

            Out.CaptureRects.Append(Out.ClearRects);
            const uint32 Frames = State.FastRefreshFrames > 0u ? kFastRefreshFrames : kRefreshFrames;
            State.FastRefreshFrames = State.FastRefreshFrames > 0u ? State.FastRefreshFrames - 1u : 0u;
            const int32 Rows = Math::Max(Res / (int32)Frames, 1);
            State.RefreshRow %= (uint32)Res;
            Out.CaptureRects.Add(FIntVector4(Out.Origin.x, Out.Origin.y + (int32)State.RefreshRow, Res, Math::Min(Rows, Res - (int32)State.RefreshRow)));
            State.RefreshRow += (uint32)Rows;
        }
    }

    void FDefaultSceneRenderer::VirtualTexturePass(RHI::FCmdListH CL)
    {
        using namespace SceneRendererVirtualTextureInternal;

        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.VirtualTextures.IsEmpty())
        {
            return;
        }

        static const FShaderH WriteShader = FShaderLibrary::Get("VirtualTextureWrite.slang");
        if (!WriteShader)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Virtual Textures");

        const RHI::FPipelineH WritePipeline = GetOrCreateComputePipeline(WriteShader);
        auto ComputeToCompute = [&CL]()
        {
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ShaderRead,
                            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        };

        // Last frame's materials sampled these, so writes wait for every stage that read them.
        RHI::CmdBarrier(CL, RHI::EStageFlags::PixelShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::MeshShader | RHI::EStageFlags::Compute,
                        RHI::EAccessFlags::ShaderRead,
                        RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        for (const FFrameData::FVirtualTextureExtract& Out : Frame.Extracts.VirtualTextures)
        {
            if (Out.UAV == RHI::kInvalidHeapSlot)
            {
                continue;
            }

            FVirtualTextureWriteArgs Args = {};
            Args.Target       = Out.UAV;
            Args.Resolution   = Out.Resolution;
            Args.TexelSize    = Out.TexelSize;
            Args.Clear        = Out.Clear;
            Args.WindowOrigin = Out.Origin;

            RHI::CmdSetPipeline(CL, WritePipeline);
            for (const FIntVector4& Rect : Out.ClearRects)
            {
                Args.Mode     = (uint32)EWriteMode::Clear;
                Args.RectMin  = FIntVector2(Rect.x, Rect.y);
                Args.RectSize = FUIntVector2((uint32)Rect.z, (uint32)Rect.w);
                RHI::CmdDispatch(CL, MakeArgs(Args), RenderUtils::GetGroupCount(Args.RectSize.x, 8u), RenderUtils::GetGroupCount(Args.RectSize.y, 8u), 1u);
            }
            if (!Out.ClearRects.IsEmpty())
            {
                ComputeToCompute();
            }

            if (Out.DecayAmount > 0.0f)
            {
                Args.Mode   = (uint32)EWriteMode::Decay;
                Args.Amount = Out.DecayAmount;
                RHI::CmdDispatch(CL, MakeArgs(Args), RenderUtils::GetGroupCount(Out.Resolution, 8u), RenderUtils::GetGroupCount(Out.Resolution, 8u), 1u);
                ComputeToCompute();
            }

            for (const FFrameData::FVirtualTextureExtract::FCapture& Capture : Out.Captures)
            {
                const FFrameData::FTerrainExtract& Item = Frame.Extracts.TerrainExtracts[Capture.TerrainExtract];
                if (!Item.VirtualTextureShader || (uint32)Capture.TerrainExtract >= TerrainRecordCount)
                {
                    continue;
                }
                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(Item.VirtualTextureShader));

                // The coarsest page whose texels are no smaller than the virtual texture's, so a wide window does not alias fine pages.
                const float Spacing = Item.TileWorldSize / (float)Math::Max(Item.Resolution - 1, 1);
                const float Mip     = Math::Floor(Math::Max(Math::Log2(Out.TexelSize / Math::Max(Spacing, 1e-4f)), 0.0f));

                FVirtualTextureCaptureArgs CaptureArgs = {};
                CaptureArgs.Target       = Out.UAV;
                CaptureArgs.Resolution   = Out.Resolution;
                CaptureArgs.TexelSize    = Out.TexelSize;
                CaptureArgs.TerrainIndex = (uint32)Capture.TerrainExtract;
                CaptureArgs.Layout       = Capture.Layout;
                CaptureArgs.Mip          = Mip;
                for (const FIntVector4& Rect : Out.CaptureRects)
                {
                    CaptureArgs.RectMin  = FIntVector2(Rect.x, Rect.y);
                    CaptureArgs.RectSize = FUIntVector2((uint32)Rect.z, (uint32)Rect.w);
                    RHI::CmdDispatch(CL, MakeArgs(CaptureArgs), RenderUtils::GetGroupCount(CaptureArgs.RectSize.x, 8u), RenderUtils::GetGroupCount(CaptureArgs.RectSize.y, 8u), 1u);
                }
            }
            if (!Out.Captures.IsEmpty())
            {
                ComputeToCompute();
            }

            if (!Out.Stamps.IsEmpty())
            {
                RHI::CmdSetPipeline(CL, WritePipeline);
                Args.Mode       = (uint32)EWriteMode::Stamp;
                Args.StampTiles = Out.StampTiles;
                Args.Stamps     = RHI::TGPUSpan<FVirtualTextureStampGPU>(RHI::CopyTransientArray(Out.Stamps.GetData(), Out.Stamps.Num()));
                RHI::CmdDispatch(CL, MakeArgs(Args), Out.StampTiles, Out.StampTiles, (uint32)Out.Stamps.Num());
            }
        }

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::PixelShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::MeshShader | RHI::EStageFlags::Compute,
                        RHI::EAccessFlags::ShaderRead);
    }

    void FDefaultSceneRenderer::PublishVirtualTextureGlobals(FSceneGlobalData& Globals)
    {
        const FFrameData& Frame = *RenderFrame;
        Globals.VirtualTextures = {};
        if (Frame.Extracts.VirtualTextures.IsEmpty())
        {
            return;
        }

        TInlineVector<FVirtualTextureGPU, 8> Published;
        for (const FFrameData::FVirtualTextureExtract& Out : Frame.Extracts.VirtualTextures)
        {
            FVirtualTextureGPU& GPU = Published.EmplaceGetRef();
            GPU.Texture    = Out.SRV;
            GPU.Resolution = Out.Resolution;
            GPU.TexelSize  = Out.TexelSize;
            GPU.Origin     = Out.Origin;
            GPU.Clear      = Out.Clear;
        }
        Globals.VirtualTextures = RHI::CopyTransientArray(Published.GetData(), Published.Num());
    }
}
