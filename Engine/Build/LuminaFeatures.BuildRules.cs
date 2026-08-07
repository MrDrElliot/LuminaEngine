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

    public const string GpuValidation = "GpuValidation";

    public const string Aftermath = "Aftermath";

    public const string RadeonGpuDetective = "RadeonGpuDetective";

    public const string VerboseLogging = "VerboseLogging";

    public const string BugSplat = "BugSplat";

    public const string JoltDebugChecks = "JoltDebugChecks";

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

            // Jolt's own asserts, FP-exception scopes and profile instrumentation. Debug only: these
            // were on in Development, which made every physics measurement taken there an instrumented
            // one. Turn back on with -JoltDebugChecks=On when chasing a Jolt-side assert.
            JoltDebugChecks => Target.Configuration == BuildConfiguration.Debug,

            // Debug only. The layer costs far too much to leave on anywhere else, and this one flag is
            // what gates all of it -- core checks and sync validation both. Debug also turns on
            // robustBufferAccess (see CreateDevice), so a Debug run is the configuration where an
            // out-of-bounds access is bounded rather than undefined; the two belong to the same
            // decision even though only one of them is a layer.
            Validation => Target.Configuration == BuildConfiguration.Debug,

            // Never on by default, in any configuration -- including Debug. GPU-assisted validation
            // instruments every shader, so it is a deliberate hunting mode rather than something to
            // leave running.
            //
            // Note what this feature does and does not control. GPU-AV is instance-creation data, not
            // code, so it is always compiled in and the define here only picks the startup default:
            // --gpuvalidation and --nogpuvalidation are what turn it on and off. Leaving it a rebuild
            // away would be the wrong shape for it, because separating an engine fault from an
            // instrumentation fault means running the same build both ways.
            //
            // Worth knowing before reaching for it: on NVIDIA 610.88 the instrumented shaders fault
            // inside the driver rather than reporting anything (2026-08-04). The faulting module is the
            // layer's rewrite of ours -- 113 KB against a few KB -- and Aftermath cannot name it,
            // because the hash the driver reports is not one the engine ever registered. Treat a
            // device loss under it as the instrumentation's until proven otherwise.
            GpuValidation => false,

            // Vendor specific: pointless on a machine that cannot produce the crash dumps.
            Aftermath => bNonShipping && Target.bHostHasNvidiaGpu,

            // AMD's counterpart. Unlike Aftermath there is no SDK to link: the Adrenalin driver
            // writes the .rgd dump and the offline rgd CLI parses it. What the build controls is
            // the in-process side that decides whether that dump is readable -- debug-utils object
            // names and markers, and the device-fault reporting on a lost device.
            RadeonGpuDetective => bNonShipping && Target.bHostHasAmdGpu,

            // TRACE, DEBUG and INFO compile to nothing when off; WARN and above always stay.
            VerboseLogging => bNonShipping,

            // Editor targets only, and on by default there: the point is crash reports from people
            // running the editor, and they build it from source, so anything defaulting to off
            // reaches nobody. It stays gated on consent at runtime, and the database compiled in
            // here is the engine's.
            //
            // Deliberately off for Game targets. A game built on Lumina belongs to whoever built it;
            // shipping it with the engine's reporter would send their players' logs to this database
            // without either party agreeing to it.
            BugSplat => Target.Type == TargetType.Editor,

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

        // Only the startup default; the runtime flags decide. The layer is turned on with it at
        // runtime too, because GPU-AV is a feature OF the validation layer and does nothing without
        // it -- so this deliberately does not add LUMINA_WITH_VALIDATION, which would drag the layer
        // into every build whose default is on.
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
