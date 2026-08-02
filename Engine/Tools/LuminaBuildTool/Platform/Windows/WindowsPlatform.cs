using LuminaBuildTool.Configuration;
using LuminaBuildTool.Toolchain;
using LuminaBuildTool.Toolchain.Windows;

namespace LuminaBuildTool.Platform.Windows;

public sealed class WindowsPlatform : IBuildPlatform
{
    private MsvcToolchain? CachedToolchain;

    public BuildPlatform Platform => BuildPlatform.Windows64;

    public string SharedLibraryPrefix => string.Empty;

    public string SharedLibraryExtension => ".dll";

    public string StaticLibraryPrefix => string.Empty;

    public string StaticLibraryExtension => ".lib";

    public string ExecutableExtension => ".exe";

    public string ObjectFileExtension => ".obj";

    public string? ImportLibraryExtension => ".lib";

    public IEnumerable<string> GetPlatformDefinitions(TargetInfo Info)
    {
        yield return "LE_PLATFORM_WINDOWS";
        yield return "LUMINA_PLATFORM_CPU_X86_64";
        yield return "NOMINMAX";
        yield return "WIN32_LEAN_AND_MEAN";
        yield return "_CRT_SECURE_NO_WARNINGS";
        yield return "DLL_EXPORT=__declspec(dllexport)";
        yield return "DLL_IMPORT=__declspec(dllimport)";

        // The engine's TCHAR is wchar_t, which only agrees with winnt.h's TCHAR and makes TEXT()
        // produce wide literals when the Windows headers are in UNICODE mode. Without these the
        // two definitions collide and every TEXT() literal is the wrong character type.
        yield return "UNICODE";
        yield return "_UNICODE";
    }

    public IToolchain CreateToolchain(TargetInfo Info)
    {
        return CachedToolchain ??= new MsvcToolchain(VisualStudioLocator.Locate(), WindowsSdkLocator.Locate());
    }
}
