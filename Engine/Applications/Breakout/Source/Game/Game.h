#pragma once

#include "GameTypes.h"
#include "Events/KeyCodes.h"

namespace Breakout
{
    // The whole simulation. Owns one ECS registry and steps it at a fixed rate.
    class FGame
    {
    public:

        void Initialize();

        void Advance(float DeltaSeconds);

        void OnKey(Lumina::EKey Key, bool bPressed);
        void OnMouseMoved(float FieldX);

        NODISCARD FFrameStats& GetStats() { return Stats; }
        NODISCARD const FFrameStats& GetStats() const { return Stats; }
        NODISCARD bool ShowsStats() const { return bShowStats; }

        NODISCARD ECS::FRegistry& GetRegistry() { return Registry; }
        NODISCARD const ECS::FRegistry& GetRegistry() const { return Registry; }

        NODISCARD const FGameState& GetState() const { return Registry.GetSingleton<FGameState>(); }
        NODISCARD const FCameraShake& GetShake() const { return Registry.GetSingleton<FCameraShake>(); }

        NODISCARD TVector<FSoundRequest>& GetPendingSounds() { return Registry.GetSingleton<FSoundQueue>().Pending; }

        // Null for a procedural stage.
        NODISCARD const char* GetLevelName() const;

        NODISCARD bool WantsQuit() const { return bQuitRequested; }
        NODISCARD bool IsPaused() const { return bPaused; }
        NODISCARD bool IsMuted() const { return bMuted; }

    private:

        void StepFixed(float Delta);
        void StepVisual(float Delta);

        ECS::FRegistry Registry;
        FFrameStats Stats;
        float Accumulator    = 0.0f;
        bool  bPaused        = false;
        bool  bQuitRequested = false;
        bool  bShowStats     = false;
        bool  bMuted         = false;
    };
}
