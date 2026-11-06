#include "RuntimePCH.h"
#include "MovementComponentSystem.h"
#include "World/ECS/Registry.h"
#include "SystemContext.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    namespace MovementComponentSystemInternal
    {
        constexpr float kSettledAmplitude = 1e-4f;

        float EvaluateWave(ESineWaveType Type, float Phase)
        {
            const float S = Math::Sin(Phase);
            switch (Type)
            {
            case ESineWaveType::Cosine:     return Math::Cos(Phase);
            case ESineWaveType::Triangle:   return Math::Asin(S) * (2.0f / Math::Pi<float>());
            case ESineWaveType::Square:     return Math::Sign(S);
            case ESineWaveType::Sawtooth:   return Math::Fract(Phase / Math::TwoPi<float>()) * 2.0f - 1.0f;
            case ESineWaveType::Bounce:     return Math::Abs(S);
            case ESineWaveType::SmoothStep: return Math::SmoothStep(-1.0f, 1.0f, S) * 2.0f - 1.0f;
            default:                        return S;
            }
        }
    }

    void SMovementComponentSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
    }

    void SMovementComponentSystem::OnUpdate(const SSystemContext& Context)
    {
        using namespace MovementComponentSystemInternal;

        const float DeltaTime = Context.GetDeltaTime();
        if (Context.GetWorldType() == EWorldType::Editor || DeltaTime <= 0.0f)
        {
            return;
        }

        Interpolators.ParallelForEach([&](ECS::TMut<SInterpolatingMovementComponent> MoveMut, ECS::TMut<STransformComponent> TransformMut)
        {
            if (MoveMut.Get().Speed <= 0.0f)
            {
                return;
            }
            STransformComponent& Transform = TransformMut.GetMut();
            SInterpolatingMovementComponent& Move = MoveMut.GetMut();

            const float Step = Move.Speed * DeltaTime;
            Move.Alpha = Math::Clamp(Move.Alpha + (Move.bForward ? Step : -Step), 0.0f, 1.0f);
            if (Move.Alpha == 1.0f || Move.Alpha == 0.0f)
            {
                Move.bForward = Move.Alpha == 0.0f;
            }

            FTransform Pose;
            Pose.SetLocation(Math::Mix(Move.Start.GetLocation(), Move.End.GetLocation(), Move.Alpha));
            Pose.SetRotation(Math::Slerp(Move.Start.GetRotation(), Move.End.GetRotation(), Move.Alpha));
            Pose.SetScale(Math::Mix(Move.Start.GetScale(), Move.End.GetScale(), Move.Alpha));
            Transform.SetLocalTransform(Pose);
        }, 256);

        Oscillators.ParallelForEach([&](ECS::TMut<SSineWaveMovementComponent> WaveMut, ECS::TMut<STransformComponent> TransformMut)
        {
            if (!WaveMut.Get().bInitialized)
            {
                WaveMut->InitialPosition = TransformMut.Get().GetLocalLocation();
                WaveMut->bIsActive       = WaveMut.Get().bAutoStart;
                WaveMut->bInitialized    = true;
            }
            if (!WaveMut.Get().bIsActive)
            {
                return;
            }

            SSineWaveMovementComponent& Wave = WaveMut.GetMut();
            Wave.CurrentTime += DeltaTime;
            const float T = Wave.CurrentTime - Wave.StartDelay;
            if (T < 0.0f)
            {
                return;
            }

            float Envelope = Wave.bDampingEnabled ? Math::Exp(-Wave.DampingFactor * T) : 1.0f;
            if (Wave.bAmplitudeModulation)
            {
                Envelope *= 0.5f + 0.5f * Math::Sin(Math::TwoPi<float>() * Wave.AmplitudeModulationFrequency * T);
            }
            Wave.CurrentAmplitude = Wave.Amplitude * Envelope;

            FVector3 Offset = Wave.Axis * (Wave.CurrentAmplitude * EvaluateWave(Wave.WaveType, Math::TwoPi<float>() * Wave.Frequency * T + Wave.PhaseOffset));
            if (Wave.bEnableSecondaryAxis)
            {
                const float Secondary = Wave.SecondaryAmplitude * Envelope;
                Offset += Wave.SecondaryAxis * (Secondary * EvaluateWave(Wave.WaveType, Math::TwoPi<float>() * Wave.SecondaryFrequency * T + Wave.SecondaryPhaseOffset));
            }

            // A fully damped wave rests at its origin and stops, instead of rewriting the same transform forever.
            const bool bSettled = Wave.bDampingEnabled && Math::Max(Wave.Amplitude, Wave.SecondaryAmplitude) * Envelope < kSettledAmplitude;
            if (bSettled)
            {
                Offset         = FVector3(0.0f);
                Wave.bIsActive = false;
            }

            const FVector3 Base = Wave.bUseRelativeMovement ? Wave.InitialPosition : FVector3(0.0f);
            TransformMut->SetLocalLocation(Base + Offset);
        }, 256);
    }
}
