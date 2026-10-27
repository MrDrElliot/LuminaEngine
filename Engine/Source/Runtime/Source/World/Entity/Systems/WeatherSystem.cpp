#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "WeatherSystem.h"

#include "World/World.h"
#include "World/Entity/Components/WeatherComponent.h"
#include "World/Entity/Components/EntityTags.h"

namespace Lumina
{
    namespace
    {
        constexpr float kMaxStepSeconds = 0.25f;
        constexpr float kSnowingThreshold = 0.01f;

        // Smooth, aperiodic swing in [-1, 1] from three detuned sines.
        float GustNoise(double T)
        {
            constexpr double TwoPi = 6.283185307179586;
            const double N = 0.6 * Math::Sin(TwoPi * T) + 0.3 * Math::Sin(TwoPi * 2.71 * T + 1.3) + 0.1 * Math::Sin(TwoPi * 6.13 * T + 0.4);
            return (float)N;
        }

        float MoveToward(float Value, float Target, float MaxStep)
        {
            return Value < Target ? Math::Min(Value + MaxStep, Target) : Math::Max(Value - MaxStep, Target);
        }
    }

    void SWeatherSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
        RequireUpdate(EUpdateStage::Paused);
        Reads<SWeatherComponent>();
    }

    void SWeatherSystem::OnStartup(const SSystemContext& Context)
    {
        GetWorld()->GetOrEmplaceSingleton<FWeatherState>();
    }

    bool SWeatherSystem::HasWork(const SSystemContext& Context)
    {
        CWorld* World = GetWorld();
        const ECS::TComponentStorage<SWeatherComponent> Weather = World != nullptr
            ? World->GetEntityRegistry().FindStorage<SWeatherComponent>()
            : ECS::TComponentStorage<SWeatherComponent>();
        return Weather && !Weather.IsEmpty();
    }

    void SWeatherSystem::OnUpdate(const SSystemContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* World = Context.GetWorld();
        FWeatherState* State = World != nullptr ? World->TryGetSingleton<FWeatherState>() : nullptr;
        if (State == nullptr)
        {
            return;
        }

        const SWeatherComponent* Weather = nullptr;
        auto View = Context.CreateView<SWeatherComponent>(ECS::TExclude<SDisabledTag>{});
        View.ForEach([&Weather] (const SWeatherComponent& Component)
        {
            if (Weather == nullptr && Component.bEnabled)
            {
                Weather = &Component;
            }
        });

        if (Weather == nullptr)
        {
            State->bActive = false;
            bHasActivePreset = false;
            return;
        }

        const float DeltaTime = Math::Min((float)World->GetRealDeltaTime(), kMaxStepSeconds);

        const CWeatherPreset* Preset = Weather->Preset.Get();
        if (Preset != nullptr && Preset->HasAnyFlag(OF_NeedsLoad))
        {
            return;
        }

        FWeatherParams Target = Preset != nullptr ? Preset->Resolve() : FWeatherParams();
        Target.Rain.Intensity *= Weather->Intensity;

        if (!bHasActivePreset || !State->bActive)
        {
            TransitionAlpha = 1.0f;
        }
        else if (Preset != ActivePreset)
        {
            TransitionFrom  = State->Params;
            TransitionAlpha = 0.0f;
        }
        ActivePreset     = Preset;
        bHasActivePreset = true;

        TransitionAlpha = Weather->TransitionTime > 0.0f ? Math::Min(TransitionAlpha + DeltaTime / Weather->TransitionTime, 1.0f) : 1.0f;
        const float Blend = Math::SmoothStep(0.0f, 1.0f, TransitionAlpha);

        State->Params  = TransitionAlpha >= 1.0f ? Target : FWeatherParams::Lerp(TransitionFrom, Target, Blend);
        State->bActive = true;

        const FWindParams& Wind = State->Params.Wind;
        GustTime += DeltaTime * Wind.GustFrequency;
        const float Gust      = GustNoise(GustTime);
        const float GustSpeed = Math::Max(Wind.Speed * (1.0f + Wind.Gustiness * Gust), 0.0f);
        const float Veer      = Wind.Gustiness * 0.3f * GustNoise(GustTime * 0.37 + 11.0);
        const FVector2 Direction(Wind.Direction.x * Math::Cos(Veer) - Wind.Direction.y * Math::Sin(Veer),
                                 Wind.Direction.x * Math::Sin(Veer) + Wind.Direction.y * Math::Cos(Veer));
        State->Wind = Direction * GustSpeed;
        State->CloudWindOffset += State->Wind * (Wind.CloudSpeedScale * DeltaTime);
        State->Time         += DeltaTime;
        State->FallDistance += (double)State->Params.Rain.FallSpeed * DeltaTime;
        State->RainWindX    += (double)State->Wind.x * DeltaTime;
        State->RainWindZ    += (double)State->Wind.y * DeltaTime;
        State->SnowFallDistance += (double)State->Params.Snow.FallSpeed * DeltaTime;

        const FSnowParams& Snow = State->Params.Snow;
        if (Weather->bOverrideSnowCover)
        {
            State->SnowCover = Weather->SnowCover;
        }
        else if (Snow.Intensity > kSnowingThreshold)
        {
            State->SnowCover = MoveToward(State->SnowCover, Snow.MaxCover, Snow.AccumulationRate * Snow.Intensity * DeltaTime);
        }
        else
        {
            State->SnowCover = MoveToward(State->SnowCover, 0.0f, Snow.MeltRate * DeltaTime);
        }

        const FRainParams& Rain = State->Params.Rain;
        if (Weather->bOverrideWetness)
        {
            State->Wetness     = Weather->Wetness;
            State->PuddleLevel = Weather->Wetness;
            return;
        }

        constexpr float kRainingThreshold = 0.01f;
        constexpr float kPuddleFillShare  = 0.35f;
        constexpr float kPuddleDrainShare = 0.5f;
        if (Rain.Intensity > kRainingThreshold)
        {
            const float Soak = Rain.WettingRate * Rain.Intensity * DeltaTime;
            State->Wetness     = MoveToward(State->Wetness, Rain.MaxWetness, Soak);
            State->PuddleLevel = MoveToward(State->PuddleLevel, Rain.MaxWetness * Rain.Intensity, Soak * kPuddleFillShare);
        }
        else
        {
            State->Wetness     = MoveToward(State->Wetness, 0.0f, Rain.DryingRate * DeltaTime);
            State->PuddleLevel = MoveToward(State->PuddleLevel, 0.0f, Rain.DryingRate * kPuddleDrainShare * DeltaTime);
        }
    }
}
