using System.IO;
using LuminaBuildTool.Configuration;

/// <summary>
/// Engine-wide target defaults. ABI-affecting definitions belong here and nowhere else, so every
/// module in a target agrees on struct layout.
/// </summary>
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
            "JPH_OBJECT_LAYER_BITS=32",
            "JPH_DEBUG_RENDERER",
            "EASTL_USER_DEFINED_ALLOCATOR=1",
            "_SILENCE_CXX23_ALIGNED_UNION_DEPRECATION_WARNING",
            "_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING",
            "IMGUI_DEFINE_MATH_OPERATORS",
            "IMGUI_IMPL_VULKAN_USE_VOLK",
            "RMLUI_STATIC_LIB",
            "LUA_VECTOR_SIZE=4",
            "LUA_UTAG_LIMIT=2000",
            "LUA_LUTAG_LIMIT=2000",
            "__AVX__",

            // The runtime resolves plugin and project binaries from these, so they must stay
            // exactly "Windows" / "64" / "Windows64" / ".dll" on Windows.
            $"LUMINA_SYSTEM_NAME=\"{Target.SystemName}\"",
            $"LUMINA_ARCH_NAME=\"{Target.ArchitectureName}\"",
            $"LUMINA_PLATFORM_NAME=\"{Target.PlatformName}\"",
            $"LUMINA_CONFIGURATION_NAME=\"{Target.Configuration}\"",
            $"LUMINA_SHAREDLIB_EXT_NAME=\"{Target.Platform.GetSharedLibraryExtension()}\"",
        });

        if (Target.Configuration != BuildConfiguration.Shipping)
        {
            GlobalDefinitions.AddRange(new[]
            {
                "JPH_FLOATING_POINT_EXCEPTIONS_ENABLED",
                "JPH_EXTERNAL_PROFILE",
                "JPH_ENABLE_ASSERTS",
                "LUAI_GCMETRICS",
            });
        }
        else
        {
            // Jolt's debug renderer is stripped in Shipping.
            GlobalDefinitions.Remove("JPH_DEBUG_RENDERER");
        }

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

        GlobalDisabledWarnings.AddRange(new[]
        {
            "4251", // DLL interface on an exported type
            "4275", // non-DLL-interface base class
            "4244", // precision loss
            "4267", // precision loss
        });

        GlobalCompilerOptions.Add("/bigobj");
    }
}

/// <summary>
/// Defaults shared by every engine module. Force-includes ModuleAPI.h so the export macros are
/// visible in every translation unit without each source having to include it.
/// </summary>
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
        // libraries' default-lib directives and produces duplicate symbol errors.
        PrivateLinkerOptions.Add("/NODEFAULTLIB:LIBCMT");

        // EASTL resolves its allocator per image, so every loaded image needs exactly one compiled
        // copy of the binding. Declared per image rather than per module: a monolithic link folds
        // these modules into the executable, and then only the executable still needs it.
        PerImageSourceFiles.Add(
            Path.Combine(Target.EngineSourceDirectory, "Runtime", "Source", "Memory", "EASTLImpl.cpp"));
    }
}

/// <summary>
/// Defaults for third-party code the engine vendors but does not own.
/// </summary>
public abstract class LuminaThirdPartyModuleRules : ModuleRules
{
    protected LuminaThirdPartyModuleRules(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.StaticLibrary;
        bIsThirdParty = true;
        bEnableReflection = false;
        bRootSourceFiles = true;

        // Vendored code is merged where it tolerates it. A library that does not is opted out in
        // its own Build.cs with the reason, rather than the whole category being written off:
        // most of these are ordinary C++ that merges fine, and they are a large share of a clean
        // build. When one does not survive, the fix would be upstream's, so we simply leave it
        // compiling file by file.
        bUseUnityBuild = true;
    }
}
