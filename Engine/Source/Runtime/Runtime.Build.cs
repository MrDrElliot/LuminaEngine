using LuminaBuildTool.Configuration;

public class Runtime : LuminaModuleRules
{
    public Runtime(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;

        PrecompiledHeader = new PrecompiledHeaderRules("RuntimePCH.h", "Source/RuntimePCH.cpp");

        // The test suite declares reflected types of its own, which is how delegate reflection is covered.
        bEnableTestReflection = true;

        PublicIncludePaths.Add("Source");

        PrivateDefinitions.AddRange(new[]
        {
            "GLFW_INCLUDE_NONE",
            "GLFW_STATIC",
            "LUMINA_RENDERER_VULKAN",
            "VK_NO_PROTOTYPES",
            "LUMINA_RPMALLOC",
            "LUMINA_HAS_RECAST",
        });

        // Public because a dependent compiling against these headers needs the same third-party declarations.
        PublicDependencyModuleNames.AddRange(new[]
        {
            "StbImage",
            "RPMalloc",
            "Miniz",
            "Zstd",

            "GLFW",
            "ImGui",
            "FreeType",
            "RmlUi",

            "Vulkan",
            "Volk",
            "VMA",
            "SLang",

            "Recast",
            "DotNetHost",

            "MeshOptimizer",
            "BasicUniversal",
            "MSDFGen",

            // Public because Runtime's own public headers (MeshData.h, SceneRenderTypes.h) include Shared/SharedConstants.h.
            "Shaders",

            // The header path is always needed, and the module becomes header only when profiling is off.
            "Tracy",
        });

        // Private because only Runtime's Physics/API/Box3D includes it, so the test target takes its own Box3D dependency.
        PrivateDependencyModuleNames.Add("Box3D");

        if (LuminaFeatures.IsActive(Target, LuminaFeatures.Aftermath))
        {
            // Runtime owns the DLL copy because it builds into the executable's Binaries directory.
            PrivateDependencyModuleNames.Add("NvidiaAftermath");
        }

        if (LuminaFeatures.IsActive(Target, LuminaFeatures.BugSplat))
        {
            PrivateDependencyModuleNames.Add("BugSplat");
        }

        // The ImGui widgets, renderer and backends only serve the developer UI, and the stubs that replace ImGui there cover its public API alone.
        if (!LuminaFeatures.IsActive(Target, LuminaFeatures.DevUI))
        {
            ExcludedSourceDirectories.Add(System.IO.Path.Combine(ModuleDirectory, "Source", "Tools", "UI", "ImGui"));
        }

        // Shadowed locals and unreferenced formal parameters have caused real bugs in this module.
        Warnings.Set(WarningSeverity.Fatal,
            CompilerWarning.ShadowLocal,
            CompilerWarning.ShadowParameter,
            CompilerWarning.ShadowMember,
            CompilerWarning.NonstandardExtension);

        if (Target.Platform != BuildPlatform.Windows64)
        {
            // The fork-join hot path reads its thread context on every fork, which the default dynamic TLS model makes a call.
            AddPerFileOption("ForkRuntime.cpp", "-ftls-model=initial-exec");

            // Their AVX2 paths repeat the scalar math op for op, so the scalar side must not fuse into FMA (MSVC never does by default).
            AddPerFileOption("AnimCompression.cpp", "-ffp-contract=off");
            AddPerFileOption("AnimTaskExecutor.cpp", "-ffp-contract=off");

            // The name is a request, since the Unix toolchain substitutes libstdc++_libbacktrace.a or libstdc++exp.a by GCC version.
            PublicSystemLibraries.Add("stdc++_libbacktrace");
        }

        // Defines stb's implementation macros, so it must be the only translation unit that does.
        ExcludeFromUnity.Add("StbImageImpl.cpp");

        // Reached directly by the Tests directory rather than through Runtime's own headers.
        TestDependencyModuleNames.AddRange(new[]
        {
            "Box3D",
            "ImGui",
            "RPMalloc",
        });
    }
}
