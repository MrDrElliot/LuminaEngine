#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "Containers/Algorithm.h"
#include "Containers/HashPrimitives.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/ViewCaptureComponent.h"

namespace Lumina
{
    namespace SceneRendererViewCaptureInternal
    {
        using Containers::CombineHash;
        using Containers::GetTypeHash;
        using Containers::HashBytes;
        using FFrameData      = FDefaultSceneRenderer::FFrameData;
        using ECaptureFeature = FDefaultSceneRenderer::FFrameData::ECaptureFeature;

        // A pooled view no capture used for this many extracts gives its images back.
        constexpr uint32 kIdleViewExtracts = 120u;

        // Filters are bits of one instance word, so at most this many distinct shown or hidden sets are live at once.
        constexpr uint32 kMaxFilterSlots = 32u;

        // Each capture is a cull view, and these leave the shadows of a busy scene the rest.
        constexpr uint32 kMaxComponentCaptures = 64u;

        // Must match FViewCaptureResolveArgs in ViewCaptureResolve.slang.
        struct FViewCaptureResolveArgs
        {
            uint32       Color;
            uint32       Depth;
            uint32       Target;
            uint32       bComposite;
            FUIntVector2 Size;
            uint32       Pad[2];
            FVector4     BackgroundColor;
        };
        static_assert(sizeof(FViewCaptureResolveArgs) == 48, "FViewCaptureResolveArgs must match ViewCaptureResolve.slang.");

        FViewVolume MakeCaptureVolume(const SViewCaptureComponent& Capture, const STransformComponent& Transform, const FUIntVector2& Size)
        {
            const float Aspect = (float)Size.x / (float)Math::Max(Size.y, 1u);
            const float Near   = Math::Max(Capture.NearPlane, 0.001f);
            const float Far    = Math::Max(Capture.FarPlane, Near + 0.01f);

            FViewVolume Volume(Math::Clamp(Capture.FieldOfView, 1.0f, 170.0f), Aspect, Near, Far);
            if (Capture.Projection == EViewCaptureProjection::Orthographic)
            {
                Volume.SetOrthographic(Math::Max(Capture.OrthoWidth, 0.01f), Aspect);
            }
            const FQuat Rotation = Transform.GetWorldRotation();
            Volume.SetView(Transform.GetWorldLocation(), Rotation * FViewVolume::ForwardAxis, Rotation * FViewVolume::UpAxis);
            return Volume;
        }

        ECaptureFeature MakeFeatures(const SViewCaptureComponent& Capture, bool bDrawsTerrain)
        {
            uint32 Features = 0u;
            Features |= Capture.bAntiAliasing ? (uint32)ECaptureFeature::AntiAliasing : 0u;
            Features |= Capture.bPostProcess  ? (uint32)ECaptureFeature::PostProcess  : 0u;
            Features |= Capture.bVolumetrics  ? (uint32)ECaptureFeature::Volumetrics  : 0u;
            Features |= Capture.bReflections  ? (uint32)ECaptureFeature::Reflections  : 0u;
            Features |= Capture.bTranslucency ? (uint32)ECaptureFeature::Translucency : 0u;
            Features |= Capture.bDecals       ? (uint32)ECaptureFeature::Decals       : 0u;
            Features |= Capture.Background == EViewCaptureBackground::Scene ? (uint32)ECaptureFeature::Sky : 0u;
            Features |= bDrawsTerrain ? (uint32)ECaptureFeature::Terrain : 0u;
            return (ECaptureFeature)Features;
        }

        // The world's grading for a capture that asks for post-process, otherwise the tone mapper alone, and a fixed exposure either way.
        SPostProcessSettings MakeCapturePostProcess(const SViewCaptureComponent& Capture, const FFrameData& Frame)
        {
            const SPostProcessSettings* World = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;

            SPostProcessSettings Settings = Capture.bPostProcess && World != nullptr ? *World : SPostProcessSettings{};
            if (!Capture.bPostProcess)
            {
                Settings.ToneMapper          = World != nullptr ? World->ToneMapper : Settings.ToneMapper;
                Settings.Contrast            = 1.0f;
                Settings.VignetteIntensity   = 0.0f;
                Settings.BloomIntensity      = 0.0f;
                Settings.ChromaticAberration = 0.0f;
                Settings.FilmGrainIntensity  = 0.0f;
                Settings.DepthOfFieldFStop   = 0.0f;
                Settings.FadeAmount          = 0.0f;
                Settings.LetterboxAspect     = 0.0f;
            }
            Settings.bEnabled             = true;
            Settings.bAutoExposure        = false;
            Settings.ExposureCompensation = Capture.ExposureCompensation;
            return Settings;
        }

        uint64 HashCaptureInputs(const SViewCaptureComponent& Capture, const STransformComponent& Transform, const FUIntVector2& Size)
        {
            const FMatrix4 World = Transform.GetWorldMatrix();
            uint64 Hash = HashBytes(&World, sizeof(World));
            Hash = CombineHash(Hash, GetTypeHash(Size.x) ^ (GetTypeHash(Size.y) << 1));
            Hash = CombineHash(Hash, GetTypeHash((uint64)(uintptr_t)Capture.Target.Get()));
            const float Floats[] = { Capture.FieldOfView, Capture.OrthoWidth, Capture.NearPlane, Capture.FarPlane, Capture.AmbientScale,
                                     Capture.ExposureCompensation, Capture.BackgroundColor.x, Capture.BackgroundColor.y,
                                     Capture.BackgroundColor.z, Capture.BackgroundColor.w };
            Hash = CombineHash(Hash, HashBytes(Floats, sizeof(Floats)));
            const uint32 Words[] = { (uint32)Capture.Projection, (uint32)Capture.Background, Capture.CullMask.Bits,
                                     (uint32)MakeFeatures(Capture, true), Capture.bIncludeChildren ? 1u : 0u };
            return CombineHash(Hash, HashBytes(Words, sizeof(Words)));
        }
    }

    void FDefaultSceneRenderer::ExtractViewCaptures(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SCOPE();
        using namespace SceneRendererViewCaptureInternal;

        Frame.Views.CaptureLightLayers = 0u;

        const double           Now       = World->GetTimeSinceWorldCreation();
        const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();

        THashMap<ECS::FEntity, uint32> FilterMasks;
        THashMap<uint64, uint32>       FilterSlots;
        TVector<ECS::FEntity>          Members;
        uint32                         NumCaptures = 0u;

        // Sorted, so the same set listed in another order shares a slot.
        auto Gather = [&](const TVector<ECS::FEntity>& Roots, bool bChildren)
        {
            Members.Reset();
            for (ECS::FEntity Root : Roots)
            {
                if (!Registry.IsValid(Root))
                {
                    continue;
                }
                Members.Add(Root);
                if (bChildren)
                {
                    Hierarchy.ForEachDescendant(Root, [&](ECS::FEntity Child) { Members.Add(Child); });
                }
            }
            Algo::Sort(Members, [](ECS::FEntity A, ECS::FEntity B) { return A.Value < B.Value; });
            Members.SetNum((int32)(Algo::Unique(Members) - Members.begin()));
        };

        // A ShowOnly list whose entities are all gone still claims a slot, one nobody holds, so the capture draws nothing rather than everything.
        auto AssignSlot = [&](bool bNamedAny) -> uint32
        {
            if (Members.IsEmpty() && !bNamedAny)
            {
                return 0u;
            }
            const uint64 Key = Members.IsEmpty() ? 0u : HashBytes(Members.GetData(), sizeof(ECS::FEntity) * (size_t)Members.Num());
            if (const uint32* Existing = FilterSlots.Find(Key))
            {
                return *Existing;
            }
            if ((uint32)FilterSlots.Num() >= kMaxFilterSlots)
            {
                if (!bWarnedCaptureFilterSlots)
                {
                    LOG_WARN("View capture: more than {} distinct ShowOnly and Hidden sets, so the rest draw unfiltered.", kMaxFilterSlots);
                    bWarnedCaptureFilterSlots = true;
                }
                return 0u;
            }
            const uint32 Bit = 1u << (uint32)FilterSlots.Num();
            FilterSlots.Add(Key, Bit);
            for (ECS::FEntity Member : Members)
            {
                FilterMasks.FindOrAdd(Member) |= Bit;
            }
            return Bit;
        };

        auto CapturesView = Registry.View<ECS::Read<SViewCaptureComponent>, ECS::Read<STransformComponent>, ECS::Without<SDisabledTag>>();
        CapturesView.ForEach([&](ECS::FEntity Entity, const SViewCaptureComponent& Capture, const STransformComponent& Transform)
        {
            FViewCaptureState& State = ViewCaptureStates.FindOrAdd(Entity);
            State.LastSeenSerial = ExtractSerial;

            CTextureRenderTarget* Target = Capture.Target.Get();
            if (!Capture.bEnabled || Target == nullptr || Target->GetResourceID() < 0 || !Target->GetTextureResource().NewTexture.IsValid())
            {
                return;
            }

            const FTextureResource& Resource = Target->GetTextureResource();
            const FUIntVector2 Size(Math::Max(Resource.ImageDescription.Extent.x, 1u), Math::Max(Resource.ImageDescription.Extent.y, 1u));

            // Every frame, due or not, so an entity's slots stay put and its instances are not re-uploaded on the off frames.
            Gather(Capture.ShowOnly, Capture.bIncludeChildren);
            const uint32 Include = AssignSlot(!Capture.ShowOnly.IsEmpty());
            bool bDrawsTerrain = Capture.ShowOnly.IsEmpty();
            uint64 ChangeHash = HashCaptureInputs(Capture, Transform, Size);
            for (ECS::FEntity Member : Members)
            {
                bDrawsTerrain = bDrawsTerrain || Registry.TryGet<STerrainComponent>(Member) != nullptr;
                if (const STransformComponent* MemberTransform = Registry.TryGet<STransformComponent>(Member))
                {
                    const FMatrix4 MemberWorld = MemberTransform->GetWorldMatrix();
                    ChangeHash = CombineHash(ChangeHash, HashBytes(&MemberWorld, sizeof(MemberWorld)));
                }
            }
            Gather(Capture.Hidden, Capture.bIncludeChildren);
            const uint32 Exclude = AssignSlot(false);

            const bool bRequested = Capture.CaptureRequests != State.CaptureRequests;
            bool bDue = !State.bCaptured || bRequested;
            switch (Capture.UpdateMode)
            {
            case EViewCaptureUpdate::EveryFrame: bDue = true; break;
            case EViewCaptureUpdate::Interval:   bDue = bDue || Now - State.LastCaptureTime >= (double)Capture.UpdateInterval; break;
            case EViewCaptureUpdate::OnDemand:   break;
            case EViewCaptureUpdate::OnChange:   bDue = bDue || ChangeHash != State.ChangeHash; break;
            }
            if (!bDue || NumCaptures >= kMaxComponentCaptures)
            {
                return;
            }

            const int32 ViewIndex = AcquireCaptureView(Size);
            if (ViewIndex < 0)
            {
                if (!bWarnedCaptureViewsFull)
                {
                    LOG_WARN("View capture: no scene view left for a {}x{} capture, since captures share one view per resolution and at most {} exist.",
                             Size.x, Size.y, MaxSceneViews - 1u);
                    bWarnedCaptureViewsFull = true;
                }
                return;
            }

            FFrameData::FCaptureViewData& Out = Frame.Views.CaptureViews.EmplaceGetRef();
            Out.ViewVolume      = MakeCaptureVolume(Capture, Transform, Size);
            Out.SceneViewIndex  = ViewIndex;
            Out.Features        = MakeFeatures(Capture, bDrawsTerrain);
            Out.LayerMask       = Capture.CullMask.Bits;
            Out.CaptureInclude  = Include;
            Out.CaptureExclude  = Exclude;
            Out.TargetUAV       = RHI::Textures::StorageSlot(Resource.NewTexture, 0);
            Out.Background      = Capture.Background;
            Out.BackgroundColor = Capture.Background == EViewCaptureBackground::Color ? Capture.BackgroundColor : FVector4(0.0f);
            Out.AmbientScale    = Math::Max(Capture.AmbientScale, 0.0f);
            Out.PostProcess     = MakeCapturePostProcess(Capture, Frame);
            Out.Entity          = Entity;
            Out.ChangeHash      = ChangeHash;
            Out.CaptureRequests = Capture.CaptureRequests;
            Out.CaptureTime     = Now;

            Frame.Views.CaptureLightLayers |= Capture.CullMask.Bits;
            ++NumCaptures;
        });

        ViewCaptureStates.RemoveAll([this](const auto& Pair) { return Pair.Value.LastSeenSerial != ExtractSerial; });
        ScenePrimitives.SetCaptureFilterMasks(Move(FilterMasks), Registry);
        ReleaseIdleCaptureViews();

        LUMINA_PROFILE_VALUE("ViewCapture/Captures", NumCaptures);
    }

    int32 FDefaultSceneRenderer::AcquireCaptureView(const FUIntVector2& Size)
    {
        int32 Released = -1;
        for (int32 i = 1; i < SceneViews.Num(); ++i)
        {
            FSceneView& View = SceneViews[i];
            if (!View.bCapturePool)
            {
                continue;
            }
            if (View.Size == Size)
            {
                View.CapturePoolLastUsed = ExtractSerial;
                return i;
            }
            if (Released < 0 && View.Size == FUIntVector2(0u))
            {
                Released = i;
            }
        }

        if (Released >= 0)
        {
            FSceneView& View = SceneViews[Released];
            View.Size              = Size;
            View.DisplaySize       = Size;
            InitViewImages(View);
        }
        else
        {
            if ((uint32)SceneViews.Num() >= MaxSceneViews)
            {
                return -1;
            }
            Released = SceneViews.Num();
            AddSceneView(Size, /*bPrimary*/ false);
            SceneViews[Released].bCapturePool = true;
        }

        SceneViews[Released].CapturePoolLastUsed = ExtractSerial;
        return Released;
    }

    void FDefaultSceneRenderer::ReleaseIdleCaptureViews()
    {
        using namespace SceneRendererViewCaptureInternal;

        for (int32 i = 1; i < SceneViews.Num(); ++i)
        {
            FSceneView& View = SceneViews[i];
            if (View.bCapturePool && View.Size != FUIntVector2(0u) && ExtractSerial - View.CapturePoolLastUsed > kIdleViewExtracts)
            {
                ReleaseViewImages(View);
                View.Size        = FUIntVector2(0u);
                View.DisplaySize = FUIntVector2(0u);
                bWarnedCaptureViewsFull = false;
            }
        }
    }

    void FDefaultSceneRenderer::RenderViewCaptures(RHI::FCmdListH CL)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Views.CaptureViews.IsEmpty())
        {
            return;
        }

        const uint64 PrimaryLights    = SceneBindings.Lights;
        const uint64 PrimarySceneData = SceneBindings.SceneData;
        bool         bRendered        = false;
        for (const FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            if (Capture.SceneViewIndex <= 0 || Capture.SceneViewIndex >= SceneViews.Num() || Capture.CameraViewIndex == Constants::kIndexNoneU32)
            {
                continue;
            }

            SCENE_GPU_SCOPE(CL, "Capture View");

            FSceneView& View = SceneViews[Capture.SceneViewIndex];
            PointAtView(View);
            CurrentCameraEarlyView = Capture.CameraViewIndex;

            const bool bComponent = Capture.TargetUAV != RHI::kInvalidHeapSlot;
            CurrentCapture                 = bComponent ? &Capture : nullptr;
            Frame.PostProcess.ViewOverride = bComponent ? &Capture.PostProcess : nullptr;
            if (bComponent)
            {
                // The pooled view's last clouds were marched for another capture.
                View.bCloudHistoryValid = false;

                FSceneLightData Lights = Frame.Lighting.LightData;
                Lights.ViewLayerMask   = Capture.LayerMask;
                const bool bSunlit     = !Frame.Lighting.LightLayers.IsEmpty() && (Frame.Lighting.LightLayers[0] & Capture.LayerMask) != 0u;
                Lights.bHasSun         = Lights.bHasSun != 0u && bSunlit ? 1u : 0u;
                Lights.AmbientLight.w *= Capture.AmbientScale;
                SceneBindings.Lights   = RHI::CopyTransient(Lights);
            }

            SetSceneRoot(CL, View, RHI::CopyTransient(MakeSecondaryViewGlobals(Capture.SceneGlobalData)));
            RenderCaptureView(CL);
            bRendered = true;

            // Committed only once the image landed, so an on-demand or on-change capture the frame could not draw is tried again.
            if (bComponent && ResolveViewCapture(CL, Capture))
            {
                FViewCaptureState& State = ViewCaptureStates.FindOrAdd(Capture.Entity);
                State.bCaptured       = true;
                State.LastCaptureTime = Capture.CaptureTime;
                State.ChangeHash      = Capture.ChangeHash;
                State.CaptureRequests = Capture.CaptureRequests;
            }
            SceneBindings.Lights = PrimaryLights;
        }

        CurrentCapture                 = nullptr;
        Frame.PostProcess.ViewOverride = nullptr;

        // The passes after the loop read the camera's view, and its visibility IDs resolve only under its own root.
        if (bRendered)
        {
            PointAtView(SceneViews[0]);
            CurrentCameraEarlyView = 0u;
            SetSceneRoot(CL, SceneViews[0], PrimarySceneData);
        }
    }

    bool FDefaultSceneRenderer::ResolveViewCapture(RHI::FCmdListH CL, const FFrameData::FCaptureViewData& Capture)
    {
        using namespace SceneRendererViewCaptureInternal;

        static const FShaderH ResolveShader = FShaderLibrary::Get("ViewCaptureResolve.slang");
        if (!ResolveShader)
        {
            return false;
        }

        SCENE_GPU_SCOPE(CL, "Capture Resolve");

        // Waits for the capture's own passes and for last frame's readers of the target alike.
        RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EAccessFlags::Any,
                        RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        const FSceneView& View = *CurrentView;
        FViewCaptureResolveArgs Args = {};
        Args.Color           = (uint32)View.Output.GetResourceID();
        Args.Depth           = (uint32)View.Images[(int)ENamedImage::DepthAttachment].GetResourceID();
        Args.Target          = Capture.TargetUAV;
        Args.bComposite      = Capture.Background != EViewCaptureBackground::Scene ? 1u : 0u;
        Args.Size            = View.Size;
        Args.BackgroundColor = Capture.BackgroundColor;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ResolveShader));
        RHI::CmdDispatch(CL, MakeArgs(Args), RenderUtils::GetGroupCount(View.Size.x, 8u), RenderUtils::GetGroupCount(View.Size.y, 8u), 1u);

        // The next capture reuses this view, and the UI samples the target later in the frame.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::Any, RHI::EStageFlags::AllCommands, RHI::EAccessFlags::Any);
        return true;
    }
}
