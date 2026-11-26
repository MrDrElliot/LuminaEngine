#include "RuntimePCH.h"
#include "SkyAtmosphere.h"

#include "Shared/SharedConstants.h"
#include "World/Entity/Components/EnvironmentComponent.h"

#include <cmath>

namespace Lumina::SkyAtmosphere
{
    namespace
    {
        constexpr int32 kTransmittanceSteps = 64;

        // Double, since a planet-sized radius squared leaves float nothing for the meters that matter here.
        double RayToSphere(double Radius, double CosZenith, double SphereRadius)
        {
            const double B = Radius * CosZenith;
            const double D = B * B - (Radius - SphereRadius) * (Radius + SphereRadius);
            return D < 0.0 ? -1.0 : -B + std::sqrt(D);
        }
    }

    FVector3 Transmittance(float CosZenith, float Altitude, float Aerosol)
    {
        const double Radius = SKY_PLANET_RADIUS + SKY_GROUND_ALTITUDE + Math::Clamp((double)Altitude, 0.0, SKY_ATMOSPHERE_RADIUS - SKY_PLANET_RADIUS - SKY_GROUND_ALTITUDE - 100.0);
        const double Cos    = Math::Clamp((double)CosZenith, -1.0, 1.0);

        // Half the sun's width either side of the horizon, so sunset dims over the disc instead of switching off.
        constexpr double SunAngularRadius = 0.0047;
        const double CosHorizon = -std::sqrt(Math::Max(0.0, 1.0 - (SKY_PLANET_RADIUS / Radius) * (SKY_PLANET_RADIUS / Radius)));
        const double Visible    = Math::SmoothStep(CosHorizon - SunAngularRadius, CosHorizon + SunAngularRadius, Cos);
        if (Visible <= 0.0)
        {
            return FVector3(0.0f);
        }

        const double March = Math::Max(Cos, CosHorizon);
        const double Sin   = std::sqrt(Math::Max(0.0, 1.0 - March * March));
        const double Dt    = RayToSphere(Radius, March, SKY_ATMOSPHERE_RADIUS) / kTransmittanceSteps;

        double DepthR = 0.0;
        double DepthM = 0.0;
        double DepthO = 0.0;
        for (int32 i = 0; i < kTransmittanceSteps; ++i)
        {
            const double T = (i + 0.5) * Dt;
            const double X = Sin * T;
            const double Y = Radius + March * T;
            const double Height = Math::Max(std::sqrt(X * X + Y * Y) - SKY_PLANET_RADIUS, 0.0);
            DepthR += std::exp(-Height / SKY_RAYLEIGH_SCALE_HEIGHT) * Dt;
            DepthM += std::exp(-Height / SKY_MIE_SCALE_HEIGHT) * Dt;
            DepthO += Math::Clamp(1.0 - std::abs(Height - SKY_OZONE_CENTER) / SKY_OZONE_HALF_WIDTH, 0.0, 1.0) * Dt;
        }

        const double Mie = SKY_MIE_SCATTERING * SKY_MIE_EXTINCTION_RATIO * Math::Max((double)Aerosol, 0.0) * DepthM;
        return FVector3(
            (float)(std::exp(-(SKY_RAYLEIGH_R * DepthR + Mie + SKY_OZONE_R * DepthO)) * Visible),
            (float)(std::exp(-(SKY_RAYLEIGH_G * DepthR + Mie + SKY_OZONE_G * DepthO)) * Visible),
            (float)(std::exp(-(SKY_RAYLEIGH_B * DepthR + Mie + SKY_OZONE_B * DepthO)) * Visible));
    }

    FVector3 SunlightTint(float CosZenith, float Altitude, float Aerosol)
    {
        const FVector3 Overhead = Transmittance(1.0f, Altitude, Aerosol);
        return Transmittance(CosZenith, Altitude, Aerosol) / Math::Max(Overhead, FVector3(1e-6f));
    }

    FVector3 MoonDirection(const SEnvironmentComponent& Env, const FVector3& SunDirection)
    {
        if (!Env.bMoonOpposeSun)
        {
            const float Len = Math::Length(Env.MoonDirection);
            return Len > 1e-6f ? Env.MoonDirection / Len : FVector3(0.0f, -1.0f, 0.0f);
        }

        const FVector3 Sun        = Math::Normalize(SunDirection);
        const float    Elongation = Env.MoonPhase * Math::TwoPi<float>();
        FVector3 Perp = FVector3(0.0f, 1.0f, 0.0f) - Sun * Sun.y;
        const float PerpLen = Math::Length(Perp);
        Perp = PerpLen > 1e-4f ? Perp / PerpLen : FVector3(1.0f, 0.0f, 0.0f);
        return Math::Normalize(Sun * std::cos(Elongation) + Perp * std::sin(Elongation));
    }

    float MoonPhaseBrightness(const SEnvironmentComponent& Env)
    {
        // Degrees between the sun and the viewer as seen from the moon, zero at full.
        const float Phase      = Env.MoonPhase - std::floor(Env.MoonPhase);
        const float PhaseAngle = std::abs(180.0f - Phase * 360.0f);
        return std::pow(10.0f, -0.4f * (0.026f * PhaseAngle + 4e-9f * PhaseAngle * PhaseAngle * PhaseAngle * PhaseAngle));
    }
}
