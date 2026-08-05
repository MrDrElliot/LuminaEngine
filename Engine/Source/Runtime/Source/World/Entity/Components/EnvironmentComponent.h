#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "EnvironmentComponent.generated.h"

namespace Lumina
{
    class CTexture;

    /** Sky background mode. */
    REFLECT()
    enum class ESkyMode : uint8
    {
        // Flat color fill; cheapest path, no atmospherics.
        SolidColor,

        // Zenith/horizon/ground gradient + procedural sun disc.
        Gradient,

        // Full atmospheric scattering (Rayleigh + Mie) with TOD sun color.
        Dynamic,

        // Imported HDR equirect panorama; same texture drives IBL ambient + reflections.
        HDRI,
    };

    /** IBL bake resolution tier. Sets sky-cube + specular-prefilter face sizes; higher = sharper
        reflections at more VRAM. Cost is paid only when the sky changes (the bake is gated), so this
        adds no per-frame overhead -- just a one-time bake spike on change. */
    REFLECT()
    enum class EIBLQuality : uint8
    {
        // 256 cube / 128 prefilter. Softest reflections, lowest VRAM.
        Low,

        // 512 cube / 256 prefilter.
        Medium,

        // 1024 cube / 256 prefilter. Crisp reflections that read as the source HDRI.
        High,

        // 2048 cube / 512 prefilter. Near-mirror; highest VRAM + bake cost.
        Ultra,
    };

    /** Singleton-style environment component; only one enabled instance per frame is read. */
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SEnvironmentComponent
    {
        GENERATED_BODY()

        /** When false, EnvironmentPass is skipped; HDR target stays at its clear value. */
        PROPERTY(Editable, Category = "Sky")
        bool bRenderSky = true;

        /** Selects sky background path; each mode reads a different subset of properties below. */
        PROPERTY(Editable, Category = "Sky")
        ESkyMode SkyMode = ESkyMode::Dynamic;

        /** SolidColor mode output; HDR-range, values >1 feed bloom. */
        PROPERTY(Editable, Color, Category = "Sky|Solid")
        FVector3 SolidSkyColor = FVector3(0.45f, 0.65f, 1.0f);


        /** Color directly overhead. */
        PROPERTY(Editable, Color, Category = "Sky|Gradient")
        FVector3 ZenithColor = FVector3(0.05f, 0.1f, 0.4f);

        /** Color at the horizon (Y == 0). */
        PROPERTY(Editable, Color, Category = "Sky|Gradient")
        FVector3 HorizonColor = FVector3(0.6f, 0.8f, 1.0f);

        /** Color directly below the horizon. */
        PROPERTY(Editable, Color, Category = "Sky|Gradient")
        FVector3 GroundColor = FVector3(0.2f, 0.18f, 0.15f);

        /** Horizon-to-zenith curvature; 1.0 = clean cosine, smaller widens horizon band. */
        PROPERTY(Editable, Category = "Sky|Gradient", ClampMin = 0.05f, ClampMax = 4.0f)
        float HorizonExponent = 0.7f;

        /** Sun disc visual scale (cosmetic; doesn't affect lighting). */
        PROPERTY(Editable, Category = "Sky|Sun", ClampMin = 0.0f, ClampMax = 8.0f)
        float SunDiscScale = 1.0f;

        /** Sun disc + glow brightness on the sky. */
        PROPERTY(Editable, Category = "Sky|Sun", ClampMin = 0.0f)
        float SunIntensity = 20.0f;

        /** Tint on TOD sun color (Dynamic) and sun disc (Gradient). */
        PROPERTY(Editable, Color, Category = "Sky|Sun")
        FVector3 SunColorTint = FVector3(1.0f);

        /** Exposure on dynamic sky scattering before write. */
        PROPERTY(Editable, Category = "Sky|Dynamic", ClampMin = 0.05f, ClampMax = 8.0f)
        float SkyExposure = 0.5f;

        /** Mie phase asymmetry (0 = isotropic, ~0.76 = strong forward scatter). */
        PROPERTY(Editable, Category = "Sky|Dynamic", ClampMin = -0.99f, ClampMax = 0.99f)
        float MieAnisotropy = 0.76f;

        /** Night sky base color (multiplied by NightBrightness). */
        PROPERTY(Editable, Color, Category = "Sky|Night")
        FVector3 NightSkyColor = FVector3(0.012f, 0.018f, 0.04f);

        /** Night sky brightness; independent of star/moon brightness. */
        PROPERTY(Editable, Category = "Sky|Night", ClampMin = 0.0f, ClampMax = 8.0f)
        float NightBrightness = 0.01f;

        /** Star cell density [0, 1]. */
        PROPERTY(Editable, Category = "Sky|Stars", ClampMin = 0.0f, ClampMax = 1.0f)
        float StarDensity = 0.55f;

        /** Star brightness; HDR-friendly. */
        PROPERTY(Editable, Category = "Sky|Stars", ClampMin = 0.0f, ClampMax = 8.0f)
        float StarBrightness = 0.76f;

        /** Twinkle angular speed (rad/s); 0 freezes the field. */
        PROPERTY(Editable, Category = "Sky|Stars", ClampMin = 0.0f, ClampMax = 16.0f)
        float StarTwinkleSpeed = 2.5f;

        /** Star size within its cell. */
        PROPERTY(Editable, Category = "Sky|Stars", ClampMin = 0.05f, ClampMax = 1.0f)
        float StarSize = 0.5f;

        /** Milky-Way band brightness; 0 disables. */
        PROPERTY(Editable, Category = "Sky|Stars", ClampMin = 0.0f, ClampMax = 4.0f)
        float GalaxyIntensity = 0.002f;

        /** Galactic plane tilt vs world XZ (radians). */
        PROPERTY(Editable, Category = "Sky|Stars", ClampMin = 0.0f, ClampMax = 6.2832f)
        float GalaxyTilt = 0.45f;

        /** Moon disc scale (multiples of real angular size ~0.5 deg). */
        PROPERTY(Editable, Category = "Sky|Moon", ClampMin = 0.0f, ClampMax = 16.0f)
        float MoonSize = 1.5f;

        /** Moon glow halo multiplier; 0 disables. */
        PROPERTY(Editable, Category = "Sky|Moon", ClampMin = 0.0f, ClampMax = 4.0f)
        float MoonGlowSize = 0.3f;

        /** Moon disc + glow brightness. */
        PROPERTY(Editable, Category = "Sky|Moon", ClampMin = 0.0f, ClampMax = 8.0f)
        float MoonBrightness = 0.6f;

        /** When true, moon anchored opposite sun; phase auto-shaded from sun. */
        PROPERTY(Editable, Category = "Sky|Moon")
        bool bMoonOpposeSun = true;

        /** Manual moon direction (FROM viewer TO moon) used when bMoonOpposeSun is false. */
        PROPERTY(Editable, Category = "Sky|Moon")
        FVector3 MoonDirection = FVector3(0.0f, -1.0f, 0.0f);

        /** HDR equirect for IBL irradiance/prefilter. Texture must use ColorSpace = Environment. */
        PROPERTY(Editable, Category = "Sky|HDRI")
        TObjectPtr<CTexture> EnvironmentMap;

        /** Linear multiplier on the panorama's radiance. Applied to the visible sky AND the IBL bake, so
            ambient and reflections track it. 1.0 = the HDRI's authored values. */
        PROPERTY(Editable, Category = "Sky|HDRI", ClampMin = 0.0f, ClampMax = 64.0f)
        float HDRIIntensity = 1.0f;

        /** Yaw of the panorama about world Y, in degrees; turns the HDRI, not the camera. Lines the
            source's sun/skyline up with the scene. Applied to the IBL bake as well. */
        PROPERTY(Editable, Category = "Sky|HDRI", ClampMin = 0.0f, ClampMax = 360.0f)
        float HDRIRotation = 0.0f;

        /** Reflection/IBL bake resolution. Higher tiers sharpen reflections at more VRAM; only re-baked
            on sky change, so no per-frame cost. */
        PROPERTY(Editable, Category = "Environment|Quality")
        EIBLQuality IBLQuality = EIBLQuality::High;
    };
}
