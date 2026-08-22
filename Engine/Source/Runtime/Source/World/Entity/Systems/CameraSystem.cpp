#include "RuntimePCH.h"
#include "CameraSystem.h"
#include "SystemSingletons.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/PostProcessComponent.h"
#include "World/Entity/Components/EntityTags.h"

namespace Lumina
{
    FSystemAccess SCameraSystem::Access = FSystemAccess{}
        .Write<FResolvedSceneView, FCameraGlobalState, SCameraComponent>()
        .Read<STransformComponent, SPostProcessComponent>();

    float EvaluateCameraBlend(ECameraBlendFunction Function, float Alpha)
    {
        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);
        switch (Function)
        {
        case ECameraBlendFunction::EaseIn:    return Alpha * Alpha;
        case ECameraBlendFunction::EaseOut:   return Alpha * (2.0f - Alpha);
        case ECameraBlendFunction::EaseInOut: return Alpha * Alpha * (3.0f - 2.0f * Alpha); // smoothstep
        case ECameraBlendFunction::Linear:
        default:                              return Alpha;
        }
    }

    namespace Detail
    {
        static void NewCameraConstructed(entt::registry& Registry, entt::entity Entity)
        {
            // Play-mode only, since auto-activate in the editor hijacks the viewport with no way back.
            const CWorld* World = Registry.ctx().get<CWorld*>();
            if (World == nullptr || World->GetWorldType() == EWorldType::Editor)
            {
                return;
            }

            SCameraComponent& Camera = Registry.get<SCameraComponent>(Entity);
            if (Camera.bAutoActivate)
            {
                Registry.ctx().get<entt::dispatcher&>().trigger<FSwitchActiveCameraEvent>(FSwitchActiveCameraEvent{Entity});
            }
        }

        // OutLocation is a local-space offset in world units; OutRotationDeg is pitch, yaw and roll.
        static void EvaluateCameraShakes(FCameraGlobalState& State, float Dt, FVector3& OutLocation, FVector3& OutRotationDeg)
        {
            OutLocation    = FVector3(0.0f);
            OutRotationDeg = FVector3(0.0f);

            constexpr float TwoPi = Math::TwoPi<float>();
            for (int32 i = 0; i < (int32)State.Shakes.size(); )
            {
                FCameraShakeInstance& S = State.Shakes[i];
                S.Elapsed += Dt;

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

                if (S.Duration > 0.0f && S.Elapsed >= S.Duration)
                {
                    State.Shakes.erase(State.Shakes.begin() + i);
                }
                else
                {
                    ++i;
                }
            }
        }
    }

    void SCameraSystem::Startup(const FSystemContext& Context) noexcept
    {
        Context.GetRegistry().on_construct<SCameraComponent>().connect<&Detail::NewCameraConstructed>();
    }

    void SCameraSystem::Teardown(const FSystemContext& Context) noexcept
    {
    }

    void SCameraSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        entt::registry& Registry = Context.GetRegistry();
        FResolvedSceneView& Resolved = Registry.ctx().get<FResolvedSceneView>();

        Resolved.bHasView = false;
        Resolved.bHasPostProcess = false;
        Resolved.PostProcessMaterials.clear();

        FCameraGlobalState& CameraState = Registry.ctx().get<FCameraGlobalState>();
        const entt::entity CameraEntity = CameraState.ActiveCameraEntity;
        if (!Registry.valid(CameraEntity) || !Registry.all_of<SCameraComponent, STransformComponent>(CameraEntity))
        {
            return;
        }

        const STransformComponent& CameraTransform = Registry.get<STransformComponent>(CameraEntity);
        (void)CameraTransform.GetWorldMatrix();

        SCameraComponent& Camera = Registry.get<SCameraComponent>(CameraEntity);

        // Live pose of the active camera; the blend (if any) eases toward this.
        const FVector3 TargetPosition   = CameraTransform.GetWorldLocation();
        const FQuat TargetRotation      = CameraTransform.GetWorldRotation();
        const float TargetFOV           = Camera.FOV;

        const FVector3 CameraWorldPos = TargetPosition;
        SPostProcessSettings ResolvedPostProcess = Camera.PostProcess;

        struct FVolumeContribution { float Weight; const SPostProcessSettings* Settings; int32 Priority; };
        TVector<FVolumeContribution> Contributions;

        auto VolumeView = Registry.view<const SPostProcessComponent, const STransformComponent>(entt::exclude<SDisabledTag>);
        for (entt::entity VolEntity : VolumeView)
        {
            const SPostProcessComponent& Volume = VolumeView.get<const SPostProcessComponent>(VolEntity);
            if (!Volume.bEnabled || Volume.BlendWeight <= 0.0f)
            {
                continue;
            }

            float Weight = Volume.BlendWeight;

            if (!Volume.bInfiniteExtent)
            {
                const STransformComponent& VolXform = VolumeView.get<const STransformComponent>(VolEntity);
                const FMatrix4 InvWorld = Math::Inverse(VolXform.GetWorldMatrix());
                const FVector3 LocalCam = FVector3(InvWorld * FVector4(CameraWorldPos, 1.0f));
                const FVector3 D = Math::Abs(LocalCam) - Volume.BoxExtent;
                const float Outside = Math::Max(D.x, Math::Max(D.y, D.z));

                if (Outside > Volume.BlendDistance)
                {
                    continue;
                }
                if (Outside > 0.0f && Volume.BlendDistance > 0.0001f)
                {
                    Weight *= 1.0f - (Outside / Volume.BlendDistance);
                }
            }

            if (Weight > 0.0f)
            {
                Contributions.push_back({Weight, &Volume.Settings, Volume.Priority});
            }
        }

        Algo::Sort(Contributions.begin(), Contributions.end(), [](const FVolumeContribution& A, const FVolumeContribution& B)
        {
            return A.Priority < B.Priority;
        });

        for (const FVolumeContribution& Contribution : Contributions)
        {
            BlendPostProcessSettings(ResolvedPostProcess, *Contribution.Settings, Contribution.Weight);
        }

        TVector<CMaterialInterface*>& PostProcessMaterials = Resolved.PostProcessMaterials;
        for (const TObjectPtr<CMaterialInterface>& M : Camera.PostProcessMaterials)
        {
            if (M.IsValid())
            {
                PostProcessMaterials.push_back(M.Get());
            }
        }

        struct FMaterialVolumeRef { entt::entity Entity; int32 Priority; };
        TVector<FMaterialVolumeRef> MaterialVolumes;
        for (entt::entity VolEntity : VolumeView)
        {
            const SPostProcessComponent& Volume = VolumeView.get<const SPostProcessComponent>(VolEntity);
            if (!Volume.bEnabled || Volume.PostProcessMaterials.empty())
            {
                continue;
            }
            if (!Volume.bInfiniteExtent)
            {
                const STransformComponent& VolXform = VolumeView.get<const STransformComponent>(VolEntity);
                const FMatrix4 InvWorld = Math::Inverse(VolXform.GetWorldMatrix());
                const FVector3 LocalCam = FVector3(InvWorld * FVector4(CameraWorldPos, 1.0f));
                const FVector3 D = Math::Abs(LocalCam) - Volume.BoxExtent;
                const float Outside = Math::Max(D.x, Math::Max(D.y, D.z));
                if (Outside > Volume.BlendDistance)
                {
                    continue;
                }
            }
            MaterialVolumes.push_back({VolEntity, Volume.Priority});
        }
        
        Algo::Sort(MaterialVolumes.begin(), MaterialVolumes.end(), [](const FMaterialVolumeRef& A, const FMaterialVolumeRef& B)
        {
            return A.Priority < B.Priority;
        });
        
        for (const FMaterialVolumeRef& Ref : MaterialVolumes)
        {
            const SPostProcessComponent& Volume = VolumeView.get<const SPostProcessComponent>(Ref.Entity);
            for (const TObjectPtr<CMaterialInterface>& M : Volume.PostProcessMaterials)
            {
                if (M.IsValid())
                {
                    PostProcessMaterials.push_back(M.Get());
                }
            }
        }

        // Drive the camera-to-camera blend, easing from the snapshot toward the live target.
        FCameraGlobalState::FBlendState& Blend = CameraState.Blend;
        if (Blend.bActive)
        {
            Blend.Elapsed += (float)Context.GetDeltaTime();
            if (Blend.Elapsed >= Blend.Duration)
            {
                Blend.bActive = false;
            }
        }

        FVector3            FinalPosition    = TargetPosition;
        FQuat            FinalRotation    = TargetRotation;
        float                FinalFOV         = TargetFOV;
        SPostProcessSettings FinalPostProcess = ResolvedPostProcess;

        if (Blend.bActive)
        {
            const FCameraGlobalState::FBlendState& B = Blend;
            const float Alpha = EvaluateCameraBlend(B.Function, B.Duration > 0.0f ? B.Elapsed / B.Duration : 1.0f);

            FQuat To = TargetRotation;
            if (Math::Dot(B.FromRotation, To) < 0.0f)
            {
                To = -To; // Shortest-arc slerp.
            }

            FinalPosition = Math::Mix(B.FromPosition, TargetPosition, Alpha);
            FinalRotation = Math::Slerp(B.FromRotation, To, Alpha);
            FinalFOV      = Math::Mix(B.FromFOV, TargetFOV, Alpha);

            FinalPostProcess = B.FromPostProcess;
            BlendPostProcessSettings(FinalPostProcess, ResolvedPostProcess, Alpha);
        }
        
        FVector3 ShakeLocation(0.0f);
        FVector3 ShakeRotationDeg(0.0f);
        Detail::EvaluateCameraShakes(CameraState, (float)Context.GetDeltaTime(), ShakeLocation, ShakeRotationDeg);

        const FVector3 ShakenPosition = FinalPosition + FinalRotation * ShakeLocation;
        const FQuat    ShakenRotation = FinalRotation * FQuat(FVector3(
            Math::Radians(ShakeRotationDeg.x),
            Math::Radians(ShakeRotationDeg.y),
            Math::Radians(ShakeRotationDeg.z)));

        // Baked so direct matrix consumers match the rendered view; the authored FOV stays intact.
        Camera.SetResolvedView(
            ShakenPosition,
            ShakenRotation * FVector3(0.0f, 0.0f, 1.0f),
            ShakenRotation * FVector3(0.0f, 1.0f, 0.0f),
            FinalFOV);

        Resolved.ViewVolume      = Camera.GetViewVolume();
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

    void SCameraSystem::SetActiveCamera(entt::registry& Registry, entt::entity Entity, float BlendTime, ECameraBlendFunction Function)
    {
        FCameraGlobalState& State = Registry.ctx().get<FCameraGlobalState>();

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

    entt::entity SCameraSystem::GetActiveCameraEntity(entt::registry& Registry)
    {
        return Registry.ctx().get<FCameraGlobalState>().ActiveCameraEntity;
    }

    SCameraComponent* SCameraSystem::GetActiveCamera(entt::registry& Registry)
    {
        return Registry.try_get<SCameraComponent>(GetActiveCameraEntity(Registry));
    }

    uint32 SCameraSystem::PlayCameraShake(entt::registry& Registry, const FCameraShakeParams& Params)
    {
        FCameraGlobalState& State = Registry.ctx().get<FCameraGlobalState>();

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

        State.Shakes.push_back(S);
        return S.Handle;
    }

    void SCameraSystem::StopCameraShake(entt::registry& Registry, uint32 Handle)
    {
        if (Handle == 0)
        {
            return;
        }
        FCameraGlobalState& State = Registry.ctx().get<FCameraGlobalState>();
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

    void SCameraSystem::StopAllCameraShakes(entt::registry& Registry)
    {
        Registry.ctx().get<FCameraGlobalState>().Shakes.clear();
    }
}
