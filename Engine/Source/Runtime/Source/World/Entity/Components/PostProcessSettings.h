#pragma once
#include "Core/Object/ObjectMacros.h"
#include "PostProcessSettings.generated.h"

namespace Lumina
{
    // Tone-mapping operator applied after color grading, in linear HDR.
    REFLECT()
    enum class EToneMapper : uint8
    {
        None        = 0,    // pure clamp (diagnostic / display-ready)
        ACES        = 1,    // Narkowicz ACES Film fit; saturated, hue-shifts reds
        AGX         = 2,    // Sobotka AGX (bwrensch fit); neutral highlights
        AGXPunchy   = 3,    // AGX with extra slope/sat; graded-print look
        AGXGolden   = 4,    // AGX biased warm; sunset/candlelit
    };

    // Per-camera color grading + tone mapping. Grading is linear HDR before the tone mapper; vignette is
    // display-space after. Defaults to an identity grade with AGX.
    REFLECT()
    struct RUNTIME_API SPostProcessSettings
    {
        GENERATED_BODY()


        /** When false, pass becomes passthrough copy + tone map; grading knobs ignored. */
        PROPERTY(Editable, Category = "Post Process")
        bool bEnabled = true;

        /** Tone mapper applied after grading. */
        PROPERTY(Editable, Category = "Post Process")
        EToneMapper ToneMapper = EToneMapper::ACES;


        /** Exposure compensation in stops (EV); +1 doubles, -1 halves. With auto-exposure on, this biases the adapted result. */
        PROPERTY(Editable, Category = "Post Process|Exposure", ClampMin = -8.0f, ClampMax = 8.0f)
        float ExposureCompensation = 0.0f;

        /** When true, exposure adapts to scene luminance over time; ExposureCompensation then acts as a bias on top. */
        PROPERTY(Editable, Category = "Post Process|Exposure")
        bool bAutoExposure = false;

        /** Lower bound on the auto-exposure adjustment, in stops (EV). */
        PROPERTY(Editable, Category = "Post Process|Exposure", ClampMin = -16.0f, ClampMax = 16.0f)
        float AutoExposureMinEV = -8.0f;

        /** Upper bound on the auto-exposure adjustment, in stops (EV). */
        PROPERTY(Editable, Category = "Post Process|Exposure", ClampMin = -16.0f, ClampMax = 16.0f)
        float AutoExposureMaxEV = 8.0f;

        /** Eye-adaptation rate per second; higher snaps to new lighting faster. */
        PROPERTY(Editable, Category = "Post Process|Exposure", ClampMin = 0.1f, ClampMax = 16.0f)
        float AutoExposureSpeed = 2.0f;


        /** Color temperature [-1, 1]; negative cools, positive warms. LMS chromatic adaptation keeps neutrals neutral. */
        PROPERTY(Editable, Category = "Post Process|White Balance", ClampMin = -1.0f, ClampMax = 1.0f)
        float Temperature = 0.0f;

        /** Green/magenta shift [-1, 1]; pairs with Temperature. */
        PROPERTY(Editable, Category = "Post Process|White Balance", ClampMin = -1.0f, ClampMax = 1.0f)
        float Tint = 0.0f;

        /** Contrast around mid-grey (0.18); 1.0 = no change. */
        PROPERTY(Editable, Category = "Post Process|Tone", ClampMin = 0.0f, ClampMax = 2.0f)
        float Contrast = 1.0f;

        /** 0 = greyscale, 1 = unchanged. Rec.709 luma. */
        PROPERTY(Editable, Category = "Post Process|Tone", ClampMin = 0.0f, ClampMax = 4.0f)
        float Saturation = 1.0f;

        /** Display-space gamma; 1.0 = no change. */
        PROPERTY(Editable, Category = "Post Process|Tone", ClampMin = 0.1f, ClampMax = 4.0f)
        float Gamma = 1.0f;


        /** Multiplicative color filter in linear space; (1,1,1) is no-op. */
        PROPERTY(Editable, Color, Category = "Post Process|Color Filter")
        FVector3 ColorFilter = FVector3(1.0f);

        /** Color filter strength; 0 disables. */
        PROPERTY(Editable, Category = "Post Process|Color Filter", ClampMin = 0.0f, ClampMax = 2.0f)
        float ColorFilterIntensity = 1.0f;

        /** Shadow tint (lift). */
        PROPERTY(Editable, Color, Category = "Post Process|Lift Gamma Gain")
        FVector3 Shadows = FVector3(1.0f);

        /** Midtone tint (power-style). */
        PROPERTY(Editable, Color, Category = "Post Process|Lift Gamma Gain")
        FVector3 Midtones = FVector3(1.0f);

        /** Highlight tint (gain). */
        PROPERTY(Editable, Color, Category = "Post Process|Lift Gamma Gain")
        FVector3 Highlights = FVector3(1.0f);


        /** Corner darkening; 0 = off. */
        PROPERTY(Editable, Category = "Post Process|Vignette", ClampMin = 0.0f, ClampMax = 1.0f)
        float VignetteIntensity = 0.0f;

        /** 0 = elliptical (matches aspect), 1 = circular. */
        PROPERTY(Editable, Category = "Post Process|Vignette", ClampMin = 0.0f, ClampMax = 1.0f)
        float VignetteRoundness = 1.0f;

        /** Falloff width; 0 = hard ring, 1 = soft. */
        PROPERTY(Editable, Category = "Post Process|Vignette", ClampMin = 0.01f, ClampMax = 1.0f)
        float VignetteSmoothness = 0.5f;

        /** Vignette tint. */
        PROPERTY(Editable, Color, Category = "Post Process|Vignette")
        FVector3 VignetteColor = FVector3(0.0f);

        /** Bloom strength; 0 skips bloom passes entirely. 0.04-0.12 cinematic, >0.3 stylized. */
        PROPERTY(Editable, Category = "Post Process|Bloom", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.005f)
        float BloomIntensity = 0.0f;

        /** Brightness threshold in linear scene units (pre-tone-map). ~1.0 catches sun/lights without leaking mids. */
        PROPERTY(Editable, Category = "Post Process|Bloom", ClampMin = 0.0f, ClampMax = 16.0f, Delta = 0.05f)
        float BloomThreshold = 1.0f;

        /** Soft-knee width; 0 = hard cutoff, 0.5 = standard cinematic soft knee. */
        PROPERTY(Editable, Category = "Post Process|Bloom", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f)
        float BloomSoftKnee = 0.5f;

        /** How far the wide halo spreads: each coarser bloom octave contributes Scatter x the
        previous one. ~0.85 cinematic, 1 = flat sum of all octaves, low = tight glow only. */
        PROPERTY(Editable, Category = "Post Process|Bloom", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f)
        float BloomScatter = 0.85f;

        /** Bloom tint. */
        PROPERTY(Editable, Color, Category = "Post Process|Bloom")
        FVector3 BloomTint = FVector3(1.0f);

        /** Radial RGB split; 0 = off. 0.001-0.005 subtle anamorphic, >0.01 stylized/VHS. */
        PROPERTY(Editable, Category = "Post Process|Chromatic Aberration", ClampMin = 0.0f, ClampMax = 0.05f, Delta = 0.0005f)
        float ChromaticAberration = 0.0f;

        /** Film grain strength; 0 = off. 0.05-0.15 reads as film stock. Animated per-frame. */
        PROPERTY(Editable, Category = "Post Process|Film Grain", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.005f)
        float FilmGrainIntensity = 0.0f;

        /** Grain cell size in pixels. 1 = digital, 2-4 = 35mm, >6 = Super 8. */
        PROPERTY(Editable, Category = "Post Process|Film Grain", ClampMin = 0.5f, ClampMax = 8.0f, Delta = 0.05f)
        float FilmGrainSize = 1.5f;

        /** Shadow bias; 0 = uniform, 1 = grain in dark regions only. */
        PROPERTY(Editable, Category = "Post Process|Film Grain", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f)
        float FilmGrainResponse = 0.6f;
    };

    // Blend In onto InOut by Weight; ToneMapper snaps at Weight >= 0.5. bEnabled untouched (not blendable).
    inline void BlendPostProcessSettings(SPostProcessSettings& InOut, const SPostProcessSettings& In, float Weight)
    {
        if (Weight <= 0.0f || !In.bEnabled)
        {
            return;
        }
        Weight = (Weight < 1.0f) ? Weight : 1.0f;

        const auto LerpF  = [Weight](float A, float B)         { return A + (B - A) * Weight; };
        const auto LerpV3 = [Weight](FVector3 A, FVector3 B) { return A + (B - A) * Weight; };

        InOut.ExposureCompensation = LerpF (InOut.ExposureCompensation, In.ExposureCompensation);
        InOut.AutoExposureMinEV    = LerpF (InOut.AutoExposureMinEV,    In.AutoExposureMinEV);
        InOut.AutoExposureMaxEV    = LerpF (InOut.AutoExposureMaxEV,    In.AutoExposureMaxEV);
        InOut.AutoExposureSpeed    = LerpF (InOut.AutoExposureSpeed,    In.AutoExposureSpeed);
        InOut.Temperature          = LerpF (InOut.Temperature,          In.Temperature);
        InOut.Tint                 = LerpF (InOut.Tint,                 In.Tint);
        InOut.Contrast             = LerpF (InOut.Contrast,             In.Contrast);
        InOut.Saturation           = LerpF (InOut.Saturation,           In.Saturation);
        InOut.Gamma                = LerpF (InOut.Gamma,                In.Gamma);
        InOut.ColorFilter          = LerpV3(InOut.ColorFilter,          In.ColorFilter);
        InOut.ColorFilterIntensity = LerpF (InOut.ColorFilterIntensity, In.ColorFilterIntensity);
        InOut.Shadows              = LerpV3(InOut.Shadows,              In.Shadows);
        InOut.Midtones             = LerpV3(InOut.Midtones,             In.Midtones);
        InOut.Highlights           = LerpV3(InOut.Highlights,           In.Highlights);
        InOut.VignetteIntensity    = LerpF (InOut.VignetteIntensity,    In.VignetteIntensity);
        InOut.VignetteRoundness    = LerpF (InOut.VignetteRoundness,    In.VignetteRoundness);
        InOut.VignetteSmoothness   = LerpF (InOut.VignetteSmoothness,   In.VignetteSmoothness);
        InOut.VignetteColor        = LerpV3(InOut.VignetteColor,        In.VignetteColor);
        InOut.BloomIntensity       = LerpF (InOut.BloomIntensity,       In.BloomIntensity);
        InOut.BloomThreshold       = LerpF (InOut.BloomThreshold,       In.BloomThreshold);
        InOut.BloomSoftKnee        = LerpF (InOut.BloomSoftKnee,        In.BloomSoftKnee);
        InOut.BloomScatter         = LerpF (InOut.BloomScatter,         In.BloomScatter);
        InOut.BloomTint            = LerpV3(InOut.BloomTint,            In.BloomTint);
        InOut.ChromaticAberration  = LerpF (InOut.ChromaticAberration,  In.ChromaticAberration);
        InOut.FilmGrainIntensity   = LerpF (InOut.FilmGrainIntensity,   In.FilmGrainIntensity);
        InOut.FilmGrainSize        = LerpF (InOut.FilmGrainSize,        In.FilmGrainSize);
        InOut.FilmGrainResponse    = LerpF (InOut.FilmGrainResponse,    In.FilmGrainResponse);

        if (Weight >= 0.5f)
        {
            InOut.ToneMapper    = In.ToneMapper;
            InOut.bAutoExposure = In.bAutoExposure;
        }
    }
}
