#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Platform/Time/PlatformTime.h"
#include "World/ECS/Registry.h"


#include <cstdio>

namespace ECSBench
{
    using namespace Lumina;

    // Components shared by both sides, so the two registries are always measured on identical layouts.

    struct FPosition { float X = 0.0f, Y = 0.0f, Z = 0.0f; };
    struct FVelocity { float X = 1.0f, Y = 1.0f, Z = 1.0f; };
    struct FHealth   { float Value = 100.0f; };

    // A fat component, because a packed pool's advantage over a paged one scales with element size.
    struct FTransformish { float M[16] = {}; };

    struct FDisabledTag {};

    // The same 64 bytes as FTransformish but paged, so growth never relocates.
    struct FFatStable
    {
        static constexpr bool InPlaceDelete = true;
        float M[16] = {};
    };

    // Payload for a named pool, which is how one tag type becomes many independent pools.
    struct FTagLabel { uint32 Value = 0; };

    // Context singletons. Not components, but they share the type-id mechanism.
    struct FCameraState { uint32 ActiveEntity = 0; float Fov = 90.0f; };
    struct FWorldClock  { double Seconds = 0.0; };

    // A non-trivial type, so destruction and relocation are exercised rather than memcpy'd past.
    struct FOwning
    {
        FOwning() : Data(new int(7)) {}
        FOwning(const FOwning& Other) : Data(new int(*Other.Data)) {}
        FOwning(FOwning&& Other) noexcept : Data(Other.Data) { Other.Data = nullptr; }
        FOwning& operator = (const FOwning& Other) { if (this != &Other) { delete Data; Data = new int(*Other.Data); } return *this; }
        FOwning& operator = (FOwning&& Other) noexcept { if (this != &Other) { delete Data; Data = Other.Data; Other.Data = nullptr; } return *this; }
        ~FOwning() { delete Data; ++Destroyed; }

        int* Data = nullptr;
        static inline int Destroyed = 0;
    };

    struct FStablePosition
    {
        static constexpr bool InPlaceDelete = true;
        float X = 0.0f, Y = 0.0f, Z = 0.0f;
    };


    // Timing

    struct FResult
    {
        double Nanos = 0.0;
        uint64 Check = 0;
    };

    // Runs the body until it has spent enough time to be stable, then reports the best pass.
    template<typename TFunc>
    double MeasureNanosPerOp(size_t OpsPerPass, size_t Passes, TFunc&& Body)
    {
        double BestSeconds = 1e30;

        // A random-access case is built once, so a cold TLB would otherwise land entirely in the timed passes.
        Body();

        for (size_t Pass = 0; Pass < Passes; ++Pass)
        {
            const uint64 Start = PlatformTime::Cycles();
            Body();
            const double Seconds = PlatformTime::ToSeconds(PlatformTime::Cycles() - Start);
            if (Seconds < BestSeconds)
            {
                BestSeconds = Seconds;
            }
        }

        return OpsPerPass > 0 ? (BestSeconds * 1e9) / static_cast<double>(OpsPerPass) : 0.0;
    }

    void ReportCase(const char* Name, size_t OpsPerPass, const FResult& Result);
    void ReportHeader();
    void ReportFooter();

    // A cheap deterministic sequence, so both sides see the same access pattern without a shared RNG cost.
    class FSplitMix
    {
    public:

        explicit FSplitMix(uint64 InSeed) : State(InSeed) {}

        uint64 Next()
        {
            State += 0x9E3779B97F4A7C15ull;
            uint64 Z = State;
            Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
            Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
            return Z ^ (Z >> 31);
        }

        uint32 NextBelow(uint32 Bound) { return static_cast<uint32>(Next() % Bound); }

    private:

        uint64 State;
    };
}

