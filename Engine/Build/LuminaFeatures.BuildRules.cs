using System.Collections.Generic;
using LuminaBuildTool.Configuration;

/// <summary>
/// Resolves the engine's optional features for a build. The mode comes from
/// Engine/Build/BuildConfiguration.json and the command line; what "auto" means for each feature
/// is engine policy and lives here.
/// </summary>
public static class LuminaFeatures
{
    public const string Tracy = "Tracy";

    public const string Validation = "Validation";

    public const string Aftermath = "Aftermath";

    public const string VerboseLogging = "VerboseLogging";

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

            // Validation layers cost far too much to leave on outside Debug.
            Validation => Target.Configuration == BuildConfiguration.Debug,

            // Vendor specific: pointless on a machine that cannot produce the crash dumps.
            Aftermath => bNonShipping && Target.bHostHasNvidiaGpu,

            // TRACE, DEBUG and INFO compile to nothing when off; WARN and above always stay.
            VerboseLogging => bNonShipping,

            _ => false,
        };
    }

    /// <summary>
    /// Adds the definitions a feature implies when it is active. Called once per target from the
    /// shared target rules.
    /// </summary>
    public static void ApplyDefinitions(TargetInfo Target, List<string> Definitions)
    {
        if (IsActive(Target, Tracy))
        {
            Definitions.AddRange(new[]
            {
                "LUMINA_WITH_TRACY",
                "TRACY_ENABLE",
                "TRACY_CALLSTACK",
                "TRACY_ON_DEMAND",

                // The scheduler brackets fiber switches with TracyFiberEnter and Leave; without
                // this the zones double-end.
                "TRACY_FIBERS",
                "TRACY_ALLOW_SHADOW_WARNING",
                "RMLUI_TRACY_PROFILING",
            });
        }

        if (IsActive(Target, Validation))
        {
            Definitions.Add("LUMINA_WITH_VALIDATION");
        }

        if (IsActive(Target, Aftermath))
        {
            Definitions.Add("WITH_AFTERMATH");
        }

        if (IsActive(Target, VerboseLogging))
        {
            Definitions.Add("LUMINA_VERBOSE_LOGGING");
        }
    }
}
