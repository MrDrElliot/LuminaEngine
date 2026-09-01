#pragma once

#include "GameTypes.h"
#include "Swarm.h"
#include "Events/KeyCodes.h"

namespace Umbral
{
    const char* WeaponName(EWeapon Weapon);
    const char* WeaponTagline(EWeapon Weapon);
    const char* WeaponUpgrade(EWeapon Weapon, int32 NextLevel);

    class FGame
    {
    public:

        void Initialize();
        void Advance(float DeltaSeconds);

        void OnKey(Lumina::EKey Key, bool bPressed);
        void OnMouseMoved(const FVector2& ViewPosition);
        void OnMouseClick();
        void SetViewSize(const FVector2& ViewSize);

        NODISCARD ECS::FRegistry& GetRegistry() { return Registry; }
        NODISCARD FSwarm& GetSwarm() { return Swarm; }
        NODISCARD const FSwarm& GetSwarm() const { return Swarm; }

        NODISCARD const FRunState& GetRun() const { return Registry.GetSingleton<FRunState>(); }
        NODISCARD const FPlayerState& GetPlayer() const { return Registry.GetSingleton<FPlayerState>(); }
        NODISCARD const FWeaponState& GetWeapons() const { return Registry.GetSingleton<FWeaponState>(); }

        NODISCARD TVector<FSoundRequest>& GetPendingSounds() { return Registry.GetSingleton<FSoundQueue>().Pending; }
        NODISCARD FFrameStats& GetStats() { return Stats; }
        NODISCARD const FFrameStats& GetStats() const { return Stats; }

        NODISCARD bool WantsQuit() const { return bQuitRequested; }
        NODISCARD bool IsPaused() const { return bPaused; }
        NODISCARD bool ShowsStats() const { return bShowStats; }

    private:

        void StepFixed(float Delta);
        void StepVisual(float Delta);

        ECS::FRegistry Registry;
        FSwarm Swarm;
        FFrameStats Stats;

        TVector<FDamageVolume> Volumes;
        TVector<FVector2> Deaths;
        TVector<FVector4> DeathColors;

        float Accumulator    = 0.0f;
        bool  bPaused        = false;
        bool  bQuitRequested = false;
        bool  bShowStats     = false;
    };
}
