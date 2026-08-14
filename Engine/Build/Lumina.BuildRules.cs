using System.IO;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Platform;

/// <summary>Engine-wide target defaults.</summary>
public abstract class LuminaTargetRules : TargetRules
{
    protected LuminaTargetRules(TargetInfo Target)
        : base(Target)
    {
        CppStandard = "c++latest";

        // AVX, not AVX2: AVX2 raises an invalid-instruction fault on CPUs without it.
        VectorExtensions = "AVX";

        bEnableExceptions = true;
        bEnableRtti = false;
        bUseDynamicCrt = true;

        OutputSuffix = Target.Configuration switch
        {
            BuildConfiguration.Debug => "-Debug",
            BuildConfiguration.Development => "-Development",
            _ => "-Shipping",
        };

        bDebugSymbols = Target.Configuration != BuildConfiguration.Shipping;
        bLinkTimeCodeGeneration = Target.Configuration == BuildConfiguration.Shipping;
        bIncrementalLinking = Target.Configuration != BuildConfiguration.Shipping;

        GlobalDefinitions.AddRange(new[]
        {
            "EASTL_USER_DEFINED_ALLOCATOR=1",
            "_SILENCE_CXX23_ALIGNED_UNION_DEPRECATION_WARNING",
            "_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING",
            "IMGUI_DEFINE_MATH_OPERATORS",
            "IMGUI_IMPL_VULKAN_USE_VOLK",
            "RMLUI_STATIC_LIB",
            "__AVX__",

            // The runtime resolves plugin and project binaries from these, so they must stay
            // exactly "Windows" / "64" / "Windows64" / ".dll" on Windows.
            $"LUMINA_SYSTEM_NAME=\"{Target.SystemName}\"",
            $"LUMINA_ARCH_NAME=\"{Target.ArchitectureName}\"",
            $"LUMINA_PLATFORM_NAME=\"{Target.PlatformName}\"",
            $"LUMINA_CONFIGURATION_NAME=\"{Target.Configuration}\"",
            $"LUMINA_SHAREDLIB_EXT_NAME=\"{BuildPlatformRegistry.Get(Target.Platform).SharedLibraryExtension}\"",
            $"LUMINA_SHAREDLIB_PREFIX_NAME=\"{BuildPlatformRegistry.Get(Target.Platform).SharedLibraryPrefix}\"",
        });

        LuminaFeatures.ApplyDefinitions(Target, GlobalDefinitions);

        GlobalDefinitions.Add(Target.Configuration switch
        {
            BuildConfiguration.Debug => "LE_DEBUG",
            BuildConfiguration.Development => "LE_DEVELOPMENT",
            _ => "LE_SHIPPING",
        });

        // Must agree with the CRT the toolchain selects. A target that links prebuilt release
        // libraries pins itself to a non-debug configuration rather than overriding this.
        GlobalDefinitions.Add(Target.Configuration == BuildConfiguration.Debug ? "_DEBUG" : "NDEBUG");

        // Strips the API export and import macros and makes IMPLEMENT_MODULE register into a
        // static table instead of exporting an entry point.
        if (Target.bMonolithic)
        {
            GlobalDefinitions.Add("LUMINA_MONOLITHIC");
        }

        if (Target.Platform == BuildPlatform.Windows64)
        {
            GlobalDisabledWarnings.AddRange(new[]
            {
                "4251", // DLL interface on an exported type
                "4275", // non-DLL-interface base class
                "4244", // precision loss
                "4267", // precision loss
            });

            GlobalCompilerOptions.Add("/bigobj");
        }

        // Layering the module graph cannot state for itself. Checked across the whole closure, so
        // routing one of these through an intermediate module does not get past it.
        ForbidDependency(
            "Runtime",
            "Editor",
            "The runtime is what ships. An editor dependency here is not only a layering inversion, "
            + "it cannot link at all in a Game target, where the editor module does not exist.");

        // Vendored SDKs whose reach is meant to stop at the plugin wrapping them. Left contained,
        // the plugin can be disabled and the SDK goes with it.
        ForbidDependency(
            "Runtime",
            "NsightPerf",
            "The Nsight SDK belongs to the NsightPerf plugin. Nothing in the runtime should be built "
            + "against a vendor profiler that a project is free to disable.");
    }
}

/// <summary>Defaults shared by every engine module.</summary>
public abstract class LuminaModuleRules : ModuleRules
{
    protected LuminaModuleRules(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        bEnableReflection = true;

        // Force-included ahead of everything so the module's export macros are defined in every
        // translation unit without each source having to include the header itself.
        ForceIncludeFiles.Add("ModuleAPI.h");

        // Every engine image links the dynamic CRT; the static one arrives through some vendored
        if (Target.Platform == BuildPlatform.Windows64)
        {
            PrivateLinkerOptions.Add("/NODEFAULTLIB:LIBCMT");
        }

        // EASTL resolves its allocator per image, so each image needs exactly one compiled copy.
        PerImageSourceFiles.Add(
            Path.Combine(Target.EngineSourceDirectory, "Runtime", "Source", "Memory", "EASTLImpl.cpp"));

        // Replacing global new/delete is a per-binary link decision; an image without its own definition
        // binds to the CRT, and one image out of step hands rpmalloc a CRT block.
        PerImageSourceFiles.Add(
            Path.Combine(Target.EngineSourceDirectory, "Runtime", "Source", "Memory", "GlobalAllocatorOverrides.cpp"));
    }
}

/// <summary>Defaults for third-party code the engine vendors but does not own.</summary>
public abstract class LuminaThirdPartyModuleRules : ModuleRules
{
    protected LuminaThirdPartyModuleRules(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.StaticLibrary;
        bIsThirdParty = true;
        bEnableReflection = false;
        bRootSourceFiles = true;
        bUseUnityBuild = true;
    }
}
