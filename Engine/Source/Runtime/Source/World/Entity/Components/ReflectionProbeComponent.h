#pragma once

#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "ReflectionProbeComponent.generated.h"

namespace Lumina
{
    /** Proxy volume the reflection ray is intersected against for parallax correction. */
    REFLECT()
    enum class EReflectionProbeShape : uint8
    {
        // Axis-aligned-in-local-space box. The workhorse for interiors: rooms, corridors, courtyards.
        Box,

        // Sphere of radius = Extent.x. Cheaper intersection, no orientation to get wrong; suits
        // open areas and props where a box would be arbitrary.
        Sphere,
    };

    /** When a probe recaptures. */
    REFLECT()
    enum class EReflectionProbeUpdateMode : uint8
    {
        // Capture once, then only when the probe itself changes or a rebuild is requested. Free after
        // the initial bake. Correct for anything whose surroundings are static.
        Once,

        // Recapture continuously. Costs six scene renders every time this probe's turn comes up, so it
        // is a per-probe opt-in rather than a global mode. Multiple Always probes take turns, one per
        // frame, so ten of them each refresh every tenth frame.
        Always,
    };

    /** What fills the parts of a probe capture that no geometry covers. */
    REFLECT()
    enum class EReflectionProbeClearMode : uint8
    {
        // Render the sky. Correct outdoors, and the reason an interior probe whose walls have gaps
        // (or single-sided walls the capture sees straight through) reflects bright sky.
        Sky,

        // Fill with BackgroundColor instead. For sealed interiors, where any sky reaching the capture
        // is by definition a leak. Also the fastest way to tell whether a capture saw ANY geometry:
        // clear to black and whatever is left is real captured surface.
        SolidColor,
    };

    /** Per-probe capture face size. Higher tiers cost VRAM in the shared cube array (every probe
        allocates at the array's face size, which is the max over all probes) and bake time. */
    REFLECT()
    enum class EReflectionProbeResolution : uint8
    {
        // 64 px faces. For small props and fillers.
        Low,

        // 128 px faces. Default; adequate for anything rougher than a polished floor.
        Medium,

        // 256 px faces.
        High,

        // 512 px faces. Near-mirror surfaces only; 8x the memory of Medium per probe.
        Ultra,
    };

    /**
     * Captures the scene into a cubemap from its origin and supplies parallax-corrected specular
     * reflections to surfaces inside its influence volume, replacing the infinitely-distant sky
     * cube that SSkyLightComponent provides.
     *
     * Captures are on-demand: probes bake on level load and when explicitly rebuilt, not per frame.
     * A probe whose surroundings changed (geometry moved, sun rotated) shows stale reflections
     * until rebuilt.
     */
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SReflectionProbeComponent
    {
        GENERATED_BODY()

        /** When false, the probe contributes nothing and is skipped by the bake. */
        PROPERTY(Editable, Category = "Reflection Probe")
        bool bEnabled = true;

        /** Proxy shape used both for the influence test and for parallax correction. */
        PROPERTY(Editable, Category = "Reflection Probe")
        EReflectionProbeShape Shape = EReflectionProbeShape::Box;

        /** Half-extents in local units before the entity transform scale. Sphere mode uses X as radius. */
        PROPERTY(Editable, Category = "Reflection Probe", Units = "m")
        FVector3 Extent = FVector3(5.0f, 5.0f, 5.0f);

        /** Fraction of the volume over which influence ramps from 0 at the boundary to 1 inside.
            Larger values cross-fade more smoothly between overlapping probes at more overlap cost. */
        PROPERTY(Editable, Category = "Reflection Probe", ClampMin = 0.0f, ClampMax = 1.0f)
        float BlendDistance = 0.25f;

        /** Ties break toward the higher value where volumes overlap, letting a small interior probe
            win against the large one that encloses it. */
        PROPERTY(Editable, Category = "Reflection Probe")
        int32 Priority = 0;

        /** Scales captured radiance on sampling. Use to trim a probe that reads too hot without rebaking. */
        PROPERTY(Editable, Category = "Reflection Probe", ClampMin = 0.0f, ClampMax = 10.0f)
        float Brightness = 1.0f;

        /** Once = bake and hold. Always = recapture continuously, for probes with moving surroundings. */
        PROPERTY(Editable, Category = "Reflection Probe|Capture")
        EReflectionProbeUpdateMode UpdateMode = EReflectionProbeUpdateMode::Once;

        /** Sky or a flat fill behind the captured geometry. */
        PROPERTY(Editable, Category = "Reflection Probe|Capture")
        EReflectionProbeClearMode ClearMode = EReflectionProbeClearMode::Sky;

        /** Fill color for SolidColor mode. HDR-range; this is captured radiance, not an LDR swatch. */
        PROPERTY(Editable, Color, Category = "Reflection Probe|Capture")
        FVector3 BackgroundColor = FVector3(0.0f, 0.0f, 0.0f);

        /** Cube face size for this probe's capture. */
        PROPERTY(Editable, Category = "Reflection Probe|Capture")
        EReflectionProbeResolution Resolution = EReflectionProbeResolution::Medium;

        /** Capture-camera near plane. Raise to clip geometry hugging the probe origin out of the bake. */
        PROPERTY(Editable, Category = "Reflection Probe|Capture", ClampMin = 0.001f, Units = "m")
        float CaptureNearPlane = 0.1f;

        /** Capture-camera far plane. Geometry past this is missing from the probe and falls back to sky. */
        PROPERTY(Editable, Category = "Reflection Probe|Capture", ClampMin = 1.0f, Units = "m")
        float CaptureFarPlane = 500.0f;

        /** Offset from the entity origin for the capture viewpoint only; the influence volume stays
            centered on the entity. Lets a probe sit at eye height in a room whose volume is floor-aligned. */
        PROPERTY(Editable, Category = "Reflection Probe|Capture", Units = "m")
        FVector3 CaptureOffset = FVector3(0.0f);
    };

    /**
     * Forces every reflection probe in every scene to recapture.
     *
     * Probes rebake automatically when the probe set itself changes (added, removed, moved, resized,
     * re-prioritized), but NOT when the world around them changes: moving a wall next to a baked probe
     * leaves its reflection showing the old wall position. Call this after editing level geometry.
     * Also exposed as the console command `r.ReflectionProbes.Rebake`.
     */
    RUNTIME_API void RequestReflectionProbeRebake();

    /** Face size in texels for each resolution tier. */
    inline uint32 GetReflectionProbeFaceSize(EReflectionProbeResolution Resolution)
    {
        switch (Resolution)
        {
        case EReflectionProbeResolution::Low:    return 64u;
        case EReflectionProbeResolution::Medium: return 128u;
        case EReflectionProbeResolution::High:   return 256u;
        case EReflectionProbeResolution::Ultra:  return 512u;
        }
        return 128u;
    }
}
