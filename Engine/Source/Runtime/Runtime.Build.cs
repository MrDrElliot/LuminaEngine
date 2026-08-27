using LuminaBuildTool.Configuration;

public class Runtime : LuminaModuleRules
{
    public Runtime(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;

        PrecompiledHeader = new PrecompiledHeaderRules("RuntimePCH.h", "Source/RuntimePCH.cpp");

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

        // Everything Runtime links or exposes through its public headers. Public because a
        // dependent compiling against those headers needs the same third-party declarations.
        PublicDependencyModuleNames.AddRange(new[]
        {
            "NlohmannJson",
            "StbImage",
            "RenderDoc",
            "ConcurrentQueue",
            "RPMalloc",
            "XXHash",
            "Miniz",

            "GLFW",
            "ImGui",
            "FreeType",
            "RmlUi",

            "Vulkan",
            "Volk",
            "VMA",
            "SLang",

            "MiniAudio",
            "Recast",
            "DotNetHost",

            "MeshOptimizer",
            "BasicUniversal",
            "MSDFGen",

            // Header-only. Puts Engine/Resources/Shaders on the include path so C++ can include
            // Shared/SharedConstants.h -- the one definition of the constants both languages use.
            // Public because Runtime's own public headers (MeshData.h, SceneRenderTypes.h) include it.
            "Shaders",

            // Header path is always needed; the module itself becomes header only when
            // profiling is off.
            "Tracy",
        });

        // Private because only Runtime's Physics/API/Box3D includes <box3d/...>. A module that wants the
        // backend headers (the test target) takes its own Box3D dependency rather than inheriting one.
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

        // Shadowed locals and unreferenced formal parameters have caused real bugs in this module.
        Warnings.Set(WarningSeverity.Fatal,
            CompilerWarning.ShadowLocal,
            CompilerWarning.ShadowParameter,
            CompilerWarning.ShadowMember,
            CompilerWarning.NonstandardExtension);

        if (Target.Platform == BuildPlatform.Windows64)
        {
            AddPerFileOption("JobScheduler.cpp", "/GT");
        }
        else
        {
            AddPerFileOption("JobScheduler.cpp", "-ftls-model=initial-exec");

            // libstdc++ declares std::stacktrace but leaves the backtrace implementation in its own
            // archive, so Assert.h's HAS_STD_STACKTRACE path only links with this. The name here is
            // a request rather than a filename: the archive is libstdc++_libbacktrace.a up to GCC 13
            // and libstdc++exp.a from GCC 14, and the Unix toolchain substitutes whichever one the
            // located compiler ships, or drops it where std::stacktrace is unavailable anyway.
            PublicSystemLibraries.Add("stdc++_libbacktrace");
        }

        // Defines stb's implementation macros, so it must be the only translation unit that does.
        // Sharing one with any other source would compile stb twice into the same object.
        ExcludeFromUnity.Add("StbImageImpl.cpp");
    }
}
