using LuminaBuildTool.Configuration;

public class GLFW : LuminaThirdPartyModuleRules
{
    public GLFW(TargetInfo Target)
        : base(Target)
    {
        bCompileAsC = true;
        PublicIncludePaths.Add("include");
        PrivateIncludePaths.Add("src");

        // GLFW ships every platform back end in one tree, so the sources are listed rather than
        // globbed: compiling another platform's files would not even parse.
        bUseExplicitSourceList = true;
        ExtraSourceFiles.AddRange(new[]
        {
            "src/context.c",
            "src/init.c",
            "src/input.c",
            "src/monitor.c",
            "src/platform.c",
            "src/vulkan.c",
            "src/window.c",

            "src/null_init.c",
            "src/null_joystick.c",
            "src/null_monitor.c",
            "src/null_window.c",
        });

        if (Target.Platform == BuildPlatform.Windows64)
        {
            PublicDefinitions.Add("_GLFW_WIN32");
            ExtraSourceFiles.AddRange(new[]
            {
                "src/win32_init.c",
                "src/win32_joystick.c",
                "src/win32_module.c",
                "src/win32_monitor.c",
                "src/win32_thread.c",
                "src/win32_time.c",
                "src/win32_window.c",
                "src/wgl_context.c",
                "src/egl_context.c",
                "src/osmesa_context.c",
            });

            PublicSystemLibraries.Add("gdi32");
        }
        else if (Target.Platform == BuildPlatform.Linux64)
        {
            PublicDefinitions.Add("_GLFW_X11");
            ExtraSourceFiles.AddRange(new[]
            {
                "src/x11_init.c",
                "src/x11_monitor.c",
                "src/x11_window.c",
                "src/xkb_unicode.c",
                "src/posix_module.c",
                "src/posix_time.c",
                "src/posix_thread.c",
                "src/glx_context.c",
                "src/egl_context.c",
                "src/osmesa_context.c",
                "src/linux_joystick.c",
            });
        }
    }
}
