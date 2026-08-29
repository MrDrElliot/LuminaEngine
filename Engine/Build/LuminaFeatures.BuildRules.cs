using System.Collections.Generic;
using LuminaBuildTool.Configuration;

/// <summary>Resolves the engine's optional features for a build.</summary>
public static class LuminaFeatures
{
    public const string Tracy = "Tracy";

    public const string GpuProfiling = "GpuProfiling";

    public const string Validation = "Validation";

    public const string GpuValidation = "GpuValidation";

    public const string Aftermath = "Aftermath";

    public const string RadeonGpuDetective = "RadeonGpuDetective";

    public const string VerboseLogging = "VerboseLogging";

    public const string BugSplat = "BugSplat";

    public const string Box3DDebugChecks = "Box3DDebugChecks";

    public const string ForceInlineHint = "ForceInlineHint";

    public static bool IsActive(TargetInfo Target, string Feature)
    {
        switch (Target.Options.GetMode(Feature))
        {
            case FeatureMode.On:
                return true;

            case FeatureMode.Off:
                return false;

            default:
                return IsActiveByDefault(Target, Feature);
        }
    }

    private static bool IsActiveByDefault(TargetInfo Target, string Feature)
    {
        bool bNonShipping = Target.Configuration != BuildConfiguration.Shipping;

        return Feature switch
        {
            // Profiler instrumentation is not worth its overhead in a shipping build.
            Tracy => bNonShipping,

            // Query pools and their readback are dead weight in a shipping build.
            GpuProfiling => bNonShipping,

            // Debug only. On in Development these instrumented every physics measurement taken there.
            Box3DDebugChecks => Target.Configuration == BuildConfiguration.Debug,

            // Debug only; too expensive elsewhere. Debug also enables robustBufferAccess (see CreateDevice).
            Validation => Target.Configuration == BuildConfiguration.Debug,

            // Never on by default: GPU-AV instruments every shader. --gpuvalidation toggles it at runtime.
            // On NVIDIA 610.88 the instrumented shaders fault inside the driver (2026-08-04).
            GpuValidation => false,

            // Vendor specific: pointless on a machine that cannot produce the crash dumps.
            Aftermath => bNonShipping && Target.bHostHasNvidiaGpu,

            // No SDK to link; the driver writes the .rgd dump. This controls object names and fault reporting.
            RadeonGpuDetective => bNonShipping && Target.bHostHasAmdGpu,

            // TRACE, DEBUG and INFO compile to nothing when off; WARN and above always stay.
            VerboseLogging => bNonShipping,

            // Editor only, never Game: a game built on Lumina must not report to the engine's database.
            BugSplat => Target.Type == TargetType.Editor && Target.Platform == BuildPlatform.Windows64,

            _ => false,
        };
    }

    /// <summary>Adds the definitions a feature implies when it is active.</summary>
    public static void ApplyDefinitions(TargetInfo Target, List<string> Definitions)
    {
        if (IsActive(Target, Tracy))
        {
            Definitions.AddRange(new[]
            {
                "LUMINA_WITH_TRACY",
                "TRACY_ENABLE",

                // TRACY_CALLSTACK is absent on purpose; a valueless /D is 1, which stack-walks every zone.
                "TRACY_ON_DEMAND",

                // The scheduler brackets fiber switches with TracyFiberEnter and Leave; without
                // this the zones double-end.
                "TRACY_FIBERS",
                "TRACY_ALLOW_SHADOW_WARNING",
                "RMLUI_TRACY_PROFILING",
            });
        }

        if (IsActive(Target, GpuProfiling))
        {
            Definitions.Add("LUMINA_WITH_GPU_PROFILING");
        }

        if (IsActive(Target, Validation))
        {
            Definitions.Add("LUMINA_WITH_VALIDATION");
        }

        // Startup default only. Deliberately not LUMINA_WITH_VALIDATION, which would drag the layer in everywhere.
        if (IsActive(Target, GpuValidation))
        {
            Definitions.Add("LUMINA_WITH_GPU_VALIDATION");
        }

        if (IsActive(Target, Aftermath))
        {
            Definitions.Add("WITH_AFTERMATH");
        }

        if (IsActive(Target, RadeonGpuDetective))
        {
            Definitions.Add("WITH_RGD");
        }

        if (IsActive(Target, VerboseLogging))
        {
            Definitions.Add("LUMINA_VERBOSE_LOGGING");
        }

        if (IsActive(Target, BugSplat))
        {
            Definitions.Add("WITH_BUGSPLAT");
        }
    }
}
