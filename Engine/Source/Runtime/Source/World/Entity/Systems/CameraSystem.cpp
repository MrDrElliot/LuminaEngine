#include "RuntimePCH.h"
#include "CameraSystem.h"
#include "CameraRigSystem.h"
#include "CoroutineSystem.h"
#include "SocketAttachmentSystem.h"
#include "TweenSystem.h"
#include "Core/Math/Easing.h"
#include "World/ECS/Registry.h"
#include "SystemSingletons.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/PostProcessComponent.h"
#include "World/Entity/Components/EntityTags.h"

namespace Lumina
{
    void CCameraSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
        RequireUpdate(EUpdateStage::Paused);
        // Everything that moves a camera in the same stage goes first, so the resolved view is this frame's.
        RunsAfter<CCameraRigSystem, CSocketAttachmentSystem, CTweenSystem, CCoroutineSystem>();
    }

    float EvaluateCameraBlend(ECameraBlendFunction Function, float Alpha)
    {
        switch (Function)
        {
        case ECameraBlendFunction::EaseIn:  return Easing::Evaluate(EEaseTransition::Quad, EEaseType::In, Alpha);
        case ECameraBlendFunction::EaseOut: return Easing::Evaluate(EEaseTransition::Quad, EEaseType::Out, Alpha);

        // Hermite, which is a gentler curve than Quad InOut and is what this blend has always been.
        case ECameraBlendFunction::EaseInOut: return Math::SmoothStep(0.0f, 1.0f, Alpha);

        case ECameraBlendFunction::Linear:
        default:                            return Math::Clamp(Alpha, 0.0f, 1.0f);
        }
    }

    FQuat AddMouseLook(const FQuat& Rotation, FVector2 YawPitchDegrees)
    {
        constexpr float PitchLimit = 89.0f;

        FTransform Look;
        Look.SetRotation(Rotation);
        Look.AddYawRadians(Math::Radians(YawPitchDegrees.x));

        // Clamps the accumulated pitch rather than the delta, so the camera cannot flip past vertical.
        const float Elevation = Math::Degrees(Math::Asin(Math::Clamp(Look.GetForward().y, -1.0f, 1.0f)));
        Look.AddPitchRadians(Math::Radians(Math::Clamp(YawPitchDegrees.y, Elevation - PitchLimit, Elevation + PitchLimit)));
        return Look.GetRotation();
    }

    FViewVolume AddMouseLook(const FViewVolume& View, FVector2 YawPitchDegrees)
    {
        const FQuat Rotation = AddMouseLook(Math::QuatLookAt(View.GetForwardVector(), View.GetUpVector()), YawPitchDegrees);

        FViewVolume Turned = View;
        Turned.SetView(View.GetViewPosition(), Rotation * FVector3(0.0f, 0.0f, 1.0f), Rotation * FVector3(0.0f, 1.0f, 0.0f));
        return Turned;
    }

    namespace CameraSystemInternal
    {
        static void NewCameraConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            // Play-mode only, since auto-activate in the editor hijacks the viewport with no way back.
            CWorld* World = Registry.Ctx().Get<CWorld*>();
            if (World == nullptr || World->GetWorldType() == EWorldType::Editor)
            {
                return;
            }

            const SCameraComponent& Camera = Registry.Get<ECS::Read<SCameraComponent>>(Entity);
            if (Camera.bAutoActivate)
            {
                World->GetCamera()->SetActiveCamera(Entity);
            }
        }

        // Split from the sample below so a same-frame re-resolve can read the shakes without advancing them.
        static void AdvanceCameraShakes(SCameraGlobalState& State, float Dt)
        {
            for (int32 i = 0; i < State.Shakes.Num(); )
            {
                FCameraShakeInstance& S = State.Shakes[i];
                S.Elapsed += Dt;

                if (S.Duration > 0.0f && S.Elapsed >= S.Duration)
                {
                    State.Shakes.RemoveAt(i);
                }
                else
                {
                    ++i;
                }
            }
        }

        // OutLocation is a local-space offset in world units; OutRotationDeg is pitch, yaw and roll.
        static void SampleCameraShakes(const SCameraGlobalState& State, FVector3& OutLocation, FVector3& OutRotationDeg)
        {
            OutLocation    = FVector3(0.0f);
            OutRotationDeg = FVector3(0.0f);

            constexpr float TwoPi = Math::TwoPi<float>();
            for (const FCameraShakeInstance& S : State.Shakes)
            {
                // Envelope ramps up over BlendInTime and down over BlendOutTime before Duration ends.
                float Env = 1.0f;
                if (S.BlendInTime > 0.0f && S.Elapsed < S.BlendInTime)
                {
                    Env = S.Elapsed / S.BlendInTime;
                }
                if (S.Duration > 0.0f && S.BlendOutTime > 0.0f)
                {
                    const float Remaining = S.Duration - S.Elapsed;
                    if (Remaining < S.BlendOutTime)
                    {
                        Env = Math::Min(Env, Math::Max(0.0f, Remaining / S.BlendOutTime));
                    }
                }

                auto Osc = [&](float Phase, float FreqMul)
                {
                    return Math::Sin((S.Elapsed * S.Frequency * FreqMul + Phase) * TwoPi);
                };

                OutLocation.x    += S.LocationAmplitude.x * Osc(S.LocPhase[0], S.LocFreqMul[0]) * Env;
                OutLocation.y    += S.LocationAmplitude.y * Osc(S.LocPhase[1], S.LocFreqMul[1]) * Env;
                OutLocation.z    += S.LocationAmplitude.z * Osc(S.LocPhase[2], S.LocFreqMul[2]) * Env;
                OutRotationDeg.x += S.RotationAmplitude.x * Osc(S.RotPhase[0], S.RotFreqMul[0]) * Env;
                OutRotationDeg.y += S.RotationAmplitude.y * Osc(S.RotPhase[1], S.RotFreqMul[1]) * Env;
                OutRotationDeg.z += S.RotationAmplitude.z * Osc(S.RotPhase[2], S.RotFreqMul[2]) * Env;
            }
        }

        // Advances no clock, so calling it a second time inside one frame is free of side effects.
        static void StampResolvedView(const SCameraGlobalState& CameraState, SCameraComponent& Camera, const STransformComponent& CameraTransform,
                                      FVector3& OutPosition, FQuat& OutRotation, float& OutFOV)
        {
            OutPosition = CameraTransform.GetWorldLocation();
            OutRotation = CameraTransform.GetWorldRotation();
            OutFOV      = Camera.FOV;

            const SCameraGlobalState::FBlendState& Blend = CameraState.Blend;
            if (Blend.bActive)
            {
                const float Alpha = EvaluateCameraBlend(Blend.Function, Blend.Duration > 0.0f ? Blend.Elapsed / Blend.Duration : 1.0f);

                FQuat To = OutRotation;
                if (Math::Dot(Blend.FromRotation, To) < 0.0f)
                {
                    To = -To; // Shortest-arc slerp.
                }

                OutPosition = Math::Mix(Blend.FromPosition, OutPosition, Alpha);
                OutRotation = Math::Slerp(Blend.FromRotation, To, Alpha);
                OutFOV      = Math::Mix(Blend.FromFOV, OutFOV, Alpha);
            }

            FVector3 ShakeLocation(0.0f);
            FVector3 ShakeRotationDeg(0.0f);
            SampleCameraShakes(CameraState, ShakeLocation, ShakeRotationDeg);

            const FVector3 ShakenPosition = OutPosition + OutRotation * ShakeLocation;
            const FQuat    ShakenRotation = OutRotation * FQuat(FVector3(
                Math::Radians(ShakeRotationDeg.x),
                Math::Radians(ShakeRotationDeg.y),
                Math::Radians(ShakeRotationDeg.z)));

            // Baked so direct matrix consumers match the rendered view; the authored FOV stays intact.
            Camera.SetResolvedView(
                ShakenPosition,
                ShakenRotation * FVector3(0.0f, 0.0f, 1.0f),
                ShakenRotation * FVector3(0.0f, 1.0f, 0.0f),
                OutFOV);
        }
    }

    void CCameraSystem::OnStartup(const SExclusiveContext& Context)
    {
        Context.GetRegistry().GetSignals<SCameraComponent>().OnConstruct.Connect<&CameraSystemInternal::NewCameraConstructed>();
    }

    void CCameraSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace CameraSystemInternal;

        FResolvedSceneView& Resolved = *ResolvedView;
        Resolved.bHasView = false;
        Resolved.bHasPostProcess = false;
        Resolved.PostProcessMaterials.Reset();

        SCameraGlobalState& CameraState = *GlobalState;
        const ECS::FEntity CameraEntity = CameraState.ActiveCameraEntity;
        SCameraComponent* CameraPtr = Cameras.TryGet<SCameraComponent>(CameraEntity);
        const STransformComponent* CameraTransformPtr = Cameras.TryGet<STransformComponent>(CameraEntity);
        if (CameraPtr == nullptr || CameraTransformPtr == nullptr)
        {
            return;
        }

        const STransformComponent& CameraTransform = *CameraTransformPtr;
        (void)CameraTransform.GetWorldMatrix();

        SCameraComponent& Camera = *CameraPtr;

        // Live pose of the active camera; the blend (if any) eases toward this.
        FVector3 TargetPosition;
        FQuat TargetRotation;
        TargetPosition = CameraTransform.GetWorldLocation();
        TargetRotation = CameraTransform.GetWorldRotation();
        const float TargetFOV           = Camera.FOV;

        const FVector3 CameraWorldPos = TargetPosition;
        SPostProcessSettings ResolvedPostProcess = Camera.PostProcess;

        struct FVolumeContribution { float Weight; const SPostProcessSettings* Settings; int32 Priority; };
        static thread_local TVector<FVolumeContribution> Contributions;
        Contributions.Reset();

        struct FMaterialVolumeRef { const SPostProcessComponent* Volume; int32 Priority; };
        static thread_local TVector<FMaterialVolumeRef> MaterialVolumes;
        MaterialVolumes.Reset();

        Volumes.ForEach([&](const SPostProcessComponent& Volume, const STransformComponent& VolXform)
        {
            if (!Volume.bEnabled)
            {
                return;
            }

            const bool bBlends = Volume.BlendWeight > 0.0f;
            const bool bHasMaterials = !Volume.PostProcessMaterials.IsEmpty();
            if (!bBlends && !bHasMaterials)
            {
                return;
            }

            float Outside = 0.0f;
            if (!Volume.bInfiniteExtent)
            {
                const FMatrix4 InvWorld = Math::Inverse(VolXform.GetWorldMatrix());
                const FVector3 LocalCam = FVector3(InvWorld * FVector4(CameraWorldPos, 1.0f));
                const FVector3 D = Math::Abs(LocalCam) - Volume.BoxExtent;
                Outside = Math::Max(D.x, Math::Max(D.y, D.z));

                if (Outside > Volume.BlendDistance)
                {
                    return;
                }
            }

            if (bBlends)
            {
                float Weight = Volume.BlendWeight;
                if (Outside > 0.0f && Volume.BlendDistance > 0.0001f)
                {
                    Weight *= 1.0f - (Outside / Volume.BlendDistance);
                }
                if (Weight > 0.0f)
                {
                    Contributions.Emplace(Weight, &Volume.Settings, Volume.Priority);
                }
            }

            if (bHasMaterials)
            {
                MaterialVolumes.Emplace(&Volume, Volume.Priority);
            }
        });

        Algo::Sort(Contributions, [](const FVolumeContribution& A, const FVolumeContribution& B)
        {
            return A.Priority < B.Priority;
        });

        for (const FVolumeContribution& Contribution : Contributions)
        {
            BlendPostProcessSettings(ResolvedPostProcess, *Contribution.Settings, Contribution.Weight);
        }

        // Depth of field belongs to the lens, so a camera that sets an aperture keeps it inside any volume.
        if (Camera.PostProcess.DepthOfFieldFStop > 0.0f)
        {
            ResolvedPostProcess.DepthOfFieldFStop         = Camera.PostProcess.DepthOfFieldFStop;
            ResolvedPostProcess.DepthOfFieldMinFStop      = Camera.PostProcess.DepthOfFieldMinFStop;
            ResolvedPostProcess.DepthOfFieldMaxFStop      = Camera.PostProcess.DepthOfFieldMaxFStop;
            ResolvedPostProcess.DepthOfFieldBladeCount    = Camera.PostProcess.DepthOfFieldBladeCount;
            ResolvedPostProcess.DepthOfFieldFocusDistance = Camera.PostProcess.DepthOfFieldFocusDistance;
            ResolvedPostProcess.DepthOfFieldSensorHeight  = Camera.PostProcess.DepthOfFieldSensorHeight;
            ResolvedPostProcess.DepthOfFieldSensorWidth   = Camera.PostProcess.DepthOfFieldSensorWidth;
            ResolvedPostProcess.DepthOfFieldMaxBlur       = Camera.PostProcess.DepthOfFieldMaxBlur;
            ResolvedPostProcess.DepthOfFieldBandBlur       = Camera.PostProcess.DepthOfFieldBandBlur;
            ResolvedPostProcess.DepthOfFieldBandCenter     = Camera.PostProcess.DepthOfFieldBandCenter;
            ResolvedPostProcess.DepthOfFieldBandHalfHeight = Camera.PostProcess.DepthOfFieldBandHalfHeight;
            ResolvedPostProcess.DepthOfFieldBandFalloff    = Camera.PostProcess.DepthOfFieldBandFalloff;
            ResolvedPostProcess.DepthOfFieldTiltPitch      = Camera.PostProcess.DepthOfFieldTiltPitch;
            ResolvedPostProcess.DepthOfFieldTiltYaw        = Camera.PostProcess.DepthOfFieldTiltYaw;
        }

        TVector<CMaterialInterface*>& PostProcessMaterials = Resolved.PostProcessMaterials;
        for (const TStrongObjectPtr<CMaterialInterface>& M : Camera.PostProcessMaterials)
        {
            if (M.IsValid())
            {
                PostProcessMaterials.Add(M.Get());
            }
        }

        Algo::Sort(MaterialVolumes, [](const FMaterialVolumeRef& A, const FMaterialVolumeRef& B)
        {
            return A.Priority < B.Priority;
        });
        
        for (const FMaterialVolumeRef& Ref : MaterialVolumes)
        {
            for (const TStrongObjectPtr<CMaterialInterface>& M : Ref.Volume->PostProcessMaterials)
            {
                if (M.IsValid())
                {
                    PostProcessMaterials.Add(M.Get());
                }
            }
        }

        // Drive the camera-to-camera blend, easing from the snapshot toward the live target.
        SCameraGlobalState::FBlendState& Blend = CameraState.Blend;
        if (Blend.bActive)
        {
            Blend.Elapsed += Context.GetDeltaTime();
            if (Blend.Elapsed >= Blend.Duration)
            {
                Blend.bActive = false;
            }
        }

        AdvanceCameraShakes(CameraState, Context.GetDeltaTime());

        SPostProcessSettings FinalPostProcess = ResolvedPostProcess;
        if (Blend.bActive)
        {
            const float Alpha = EvaluateCameraBlend(Blend.Function, Blend.Duration > 0.0f ? Blend.Elapsed / Blend.Duration : 1.0f);

            FinalPostProcess = Blend.FromPostProcess;
            BlendPostProcessSettings(FinalPostProcess, ResolvedPostProcess, Alpha);
        }

        FVector3 FinalPosition = TargetPosition;
        FQuat    FinalRotation = TargetRotation;
        float    FinalFOV      = TargetFOV;
        StampResolvedView(CameraState, Camera, CameraTransform, FinalPosition, FinalRotation, FinalFOV);

        if (CameraState.OverlayFade > FinalPostProcess.FadeAmount)
        {
            FinalPostProcess.FadeAmount = CameraState.OverlayFade;
            FinalPostProcess.FadeColor  = CameraState.OverlayFadeColor;
        }
        if (CameraState.OverlayLetterbox > 0.0f)
        {
            FinalPostProcess.LetterboxAspect = CameraState.OverlayLetterbox;
        }

        Resolved.ViewVolume      = Camera.GetViewVolume();
        Resolved.Layers          = Camera.GetViewLayers();
        Resolved.MouseLookDegreesPerCount = Camera.MouseLookDegreesPerCount;
        Resolved.PostProcess     = FinalPostProcess;
        Resolved.bHasView        = true;
        Resolved.bHasPostProcess = true;

        // Record the displayed view so a later switch can blend from it.
        CameraState.LastViewPosition = FinalPosition;
        CameraState.LastViewRotation = FinalRotation;
        CameraState.LastViewFOV      = FinalFOV;
        CameraState.LastPostProcess  = FinalPostProcess;
        CameraState.bHasResolvedView = true;
    }

    void CCameraSystem::ResolveActiveCameraView(ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        const SCameraGlobalState& CameraState = Registry.Ctx().Get<SCameraGlobalState>();
        const ECS::FEntity CameraEntity = CameraState.ActiveCameraEntity;
        if (!Registry.IsValid(CameraEntity) || !Registry.HasAll<SCameraComponent, STransformComponent>(CameraEntity))
        {
            return;
        }

        SCameraComponent& Camera = Registry.Get<ECS::Write<SCameraComponent>>(CameraEntity);
        FVector3 Position(0.0f);
        FQuat    Rotation;
        float    FOV = 0.0f;
        CameraSystemInternal::StampResolvedView(CameraState, Camera, Registry.Get<ECS::Read<STransformComponent>>(CameraEntity), Position, Rotation, FOV);

        // Only refreshed once Update has published a view, so this never invents one before the first tick.
        FResolvedSceneView& Resolved = Registry.Ctx().Get<FResolvedSceneView>();
        if (Resolved.bHasView)
        {
            Resolved.ViewVolume = Camera.GetViewVolume();
        }
    }

    void CCameraSystem::SetActiveCamera(ECS::FRegistry& Registry, ECS::FEntity Entity, float BlendTime, ECameraBlendFunction Function)
    {
        SCameraGlobalState& State = Registry.Ctx().Get<SCameraGlobalState>();

        // No-op switches keep any running blend intact.
        if (Entity == State.ActiveCameraEntity)
        {
            return;
        }

        // With no prior resolved view or a zero duration the camera just snaps.
        if (BlendTime > 0.0f && State.bHasResolvedView)
        {
            State.Blend.bActive        = true;
            State.Blend.Elapsed        = 0.0f;
            State.Blend.Duration       = BlendTime;
            State.Blend.Function       = Function;
            State.Blend.FromPosition   = State.LastViewPosition;
            State.Blend.FromRotation   = State.LastViewRotation;
            State.Blend.FromFOV        = State.LastViewFOV;
            State.Blend.FromPostProcess = State.LastPostProcess;
        }
        else
        {
            State.Blend.bActive = false;
        }

        State.ActiveCameraEntity = Entity;
    }

    void CCameraSystem::SetCinematicOverlay(ECS::FRegistry& Registry, float Fade, FVector3 FadeColor, float LetterboxAspect)
    {
        SCameraGlobalState& State = Registry.Ctx().Get<SCameraGlobalState>();
        State.OverlayFade      = Math::Clamp(Fade, 0.0f, 1.0f);
        State.OverlayFadeColor = FadeColor;
        State.OverlayLetterbox = Math::Max(LetterboxAspect, 0.0f);
    }

    ECS::FEntity CCameraSystem::GetActiveCameraEntity(ECS::FRegistry& Registry)
    {
        return Registry.Ctx().Get<SCameraGlobalState>().ActiveCameraEntity;
    }

    SCameraComponent* CCameraSystem::GetActiveCamera(ECS::FRegistry& Registry)
    {
        return Registry.TryGet<ECS::Write<SCameraComponent>>(GetActiveCameraEntity(Registry));
    }

    uint32 CCameraSystem::PlayCameraShake(ECS::FRegistry& Registry, const SCameraShakeParams& Params)
    {
        SCameraGlobalState& State = Registry.Ctx().Get<SCameraGlobalState>();

        FCameraShakeInstance S;
        S.LocationAmplitude = Params.LocationAmplitude;
        S.RotationAmplitude = Params.RotationAmplitude;
        S.Frequency         = Math::Max(Params.Frequency, 0.01f);
        S.Duration          = Params.Duration;
        S.BlendInTime       = Math::Max(Params.BlendInTime, 0.0f);
        S.BlendOutTime      = Math::Max(Params.BlendOutTime, 0.0f);
        S.Elapsed           = 0.0f;
        S.Handle            = State.NextShakeHandle++;

        // Deterministic per-axis phase from the handle so axes do not oscillate in lockstep.
        auto Frac = [](uint32 X) { X *= 2654435761u; X ^= X >> 15; return (float)(X & 0xFFFFu) / 65535.0f; };
        for (int a = 0; a < 3; ++a)
        {
            S.LocPhase[a]   = Frac(S.Handle * 7u  + (uint32)a * 131u + 1u);
            S.RotPhase[a]   = Frac(S.Handle * 13u + (uint32)a * 197u + 5u);
            S.LocFreqMul[a] = 0.85f + 0.30f * Frac(S.Handle * 17u + (uint32)a * 53u + 9u);
            S.RotFreqMul[a] = 0.85f + 0.30f * Frac(S.Handle * 23u + (uint32)a * 71u + 11u);
        }

        State.Shakes.Add(S);
        return S.Handle;
    }

    void CCameraSystem::StopCameraShake(ECS::FRegistry& Registry, uint32 Handle)
    {
        if (Handle == 0)
        {
            return;
        }
        SCameraGlobalState& State = Registry.Ctx().Get<SCameraGlobalState>();
        for (FCameraShakeInstance& S : State.Shakes)
        {
            if (S.Handle == Handle)
            {
                // Turn a (possibly looping) shake into one that ends after its blend-out, for a smooth stop.
                S.Duration = S.Elapsed + S.BlendOutTime;
                break;
            }
        }
    }

    void CCameraSystem::StopAllCameraShakes(ECS::FRegistry& Registry)
    {
        Registry.Ctx().Get<SCameraGlobalState>().Shakes.Reset();
    }
}
