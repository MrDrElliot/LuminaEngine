#include "RuntimePCH.h"
#include "WaterComponent.h"

namespace Lumina
{
    FMatrix4 WaterToWorld(const SWaterComponent& Water, const FMatrix4& EntityWorld)
    {
        return Math::Scale(EntityWorld, FVector3(Math::Max(Water.Extent.x, 0.01f), 1.0f, Math::Max(Water.Extent.y, 0.01f)));
    }

    float WaterWaveHeight(const SWaterComponent& Water, float WorldX, float WorldZ, float Time)
    {
        const float WindLength = Math::Sqrt(Water.WindDirection.x * Water.WindDirection.x + Water.WindDirection.y * Water.WindDirection.y);
        const FVector2 Wind = WindLength > 1e-4f
            ? FVector2(Water.WindDirection.x / WindLength, Water.WindDirection.y / WindLength)
            : FVector2(1.0f, 0.0f);

        // The geometry waves of GetWaterWave in Includes/Water.slang, or anything resting on the water drifts off the drawn surface.
        constexpr float MaxSteepness   = 0.42f;
        constexpr float LengthRatio    = 0.86f;
        constexpr float AmplitudePower = 1.25f;
        const int       Count          = Math::Clamp(Water.WaveCount, 1, 8) * 3;
        const float     WindFactor     = 0.5f + 0.5f * Math::Clamp(Water.WindSpeed * 0.1f, 0.0f, 1.0f);

        float Height = 0.0f;
        for (int i = 0; i < Count; ++i)
        {
            const float Ratio  = Math::Pow(LengthRatio, (float)i);
            const float Spread = 1.0f + 1.6f * Math::Clamp((float)i / 15.0f, 0.0f, 1.0f);
            const float Hashed = (float)i * 0.6180339887f + 0.13f;
            const float Angle  = (Hashed - Math::Floor(Hashed) - 0.5f) * Spread;
            const float Ca = Math::Cos(Angle);
            const float Sa = Math::Sin(Angle);
            const float Dx = Wind.x * Ca - Wind.y * Sa;
            const float Dz = Wind.x * Sa + Wind.y * Ca;

            const float K      = Math::TwoPi<float>() / (Math::Max(Water.WaveLength, 0.5f) * Ratio);
            const float Omega  = Math::Sqrt(9.81f * K) * WindFactor;
            const float PhaseH = (float)i * 0.7548776662f + 0.31f;
            const float Phase  = (PhaseH - Math::Floor(PhaseH)) * Math::TwoPi<float>();
            const float Phi    = K * (Dx * WorldX + Dz * WorldZ) - Omega * Time + Phase;
            Height += Math::Min(Water.WaveAmplitude * Math::Pow(Ratio, AmplitudePower), MaxSteepness / K) * Math::Sin(Phi);
        }
        return Height;
    }

    FVector3 WaterWaveNormal(const SWaterComponent& Water, float WorldX, float WorldZ, float Time)
    {
        constexpr float Eps = 0.5f;
        const float HxL = WaterWaveHeight(Water, WorldX - Eps, WorldZ, Time);
        const float HxR = WaterWaveHeight(Water, WorldX + Eps, WorldZ, Time);
        const float HzL = WaterWaveHeight(Water, WorldX, WorldZ - Eps, Time);
        const float HzR = WaterWaveHeight(Water, WorldX, WorldZ + Eps, Time);
        return Math::Normalize(FVector3(-(HxR - HxL) / (2.0f * Eps), 1.0f, -(HzR - HzL) / (2.0f * Eps)));
    }
}
