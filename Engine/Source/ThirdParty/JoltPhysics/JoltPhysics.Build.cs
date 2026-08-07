using LuminaBuildTool.Configuration;

public class JoltPhysics : LuminaThirdPartyModuleRules
{
    public JoltPhysics(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        // Every define here changes Jolt's struct layout, alignment or ABI, so it has to reach every
        // translation unit that includes a Jolt header -- Public, never Private. Runtime's
        // Physics/API/Jolt is the only consumer in the tree; nothing else includes <Jolt/...>, which is
        // why none of this belongs in the engine's GlobalDefinitions.

        // AVX, not AVX2: this changes Jolt's struct layout and alignment, so it must match the
        // engine baseline (VectorExtensions in Lumina.BuildRules.cs). Do not bump one without the other.
        PublicDefinitions.Add("__AVX__");

        // Single precision, deliberately: JPH_DOUBLE_PRECISION widens RVec3/RMat44 to doubles for worlds
        // past ~5 km, costing memory and throughput on every body. Do not define it without that need.
        // Cross-platform determinism is off for the same reason -- JPH_CROSS_PLATFORM_DETERMINISTIC
        // forces precise FP and contact sorting, and nothing here needs lockstep reproducibility.

        // The collision profile packs a layer plus a mask into one object layer, which does not fit
        // Jolt's default 16 bits.
        PublicDefinitions.Add("JPH_OBJECT_LAYER_BITS=32");

        if (Target.Configuration != BuildConfiguration.Shipping)
        {
            PublicDefinitions.Add("JPH_DEBUG_RENDERER");
        }

        // Jolt's own asserts, FP-exception scopes and profile zones. Debug only by default: these used
        // to ride on "non-Shipping", which quietly made every Development physics measurement an
        // instrumented one. Turn on with -JoltDebugChecks=On to chase a Jolt-side assert.
        if (LuminaFeatures.IsActive(Target, LuminaFeatures.JoltDebugChecks))
        {
            PublicDefinitions.Add("JPH_FLOATING_POINT_EXCEPTIONS_ENABLED");
            PublicDefinitions.Add("JPH_EXTERNAL_PROFILE");
            PublicDefinitions.Add("JPH_ENABLE_ASSERTS");
        }
    }
}
