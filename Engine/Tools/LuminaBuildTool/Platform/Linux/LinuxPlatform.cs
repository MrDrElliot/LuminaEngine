using LuminaBuildTool.Configuration;
using LuminaBuildTool.Toolchain;
using LuminaBuildTool.Toolchain.Linux;

namespace LuminaBuildTool.Platform.Linux;

public sealed class LinuxPlatform : IBuildPlatform
{
    private ClangToolchain? CachedToolchain;

    public BuildPlatform Platform => BuildPlatform.Linux64;

    public string SharedLibraryPrefix => "lib";

    public string SharedLibraryExtension => ".so";

    public string StaticLibraryPrefix => "lib";

    public string StaticLibraryExtension => ".a";

    public string ExecutableExtension => string.Empty;

    public string ObjectFileExtension => ".o";

    public string? ImportLibraryExtension => null;

    public IEnumerable<string> GetPlatformDefinitions(TargetInfo Info)
    {
        yield return "LE_PLATFORM_LINUX";
        yield return "LUMINA_PLATFORM_CPU_X86_64";

        yield return "PLATFORM_UNIX=1";

        yield return "DLL_EXPORT=__attribute__((visibility(\"default\")))";
        yield return "DLL_IMPORT=";

        yield return "_GNU_SOURCE";
    }

    public IToolchain CreateToolchain(TargetInfo Info)
    {
        return CachedToolchain ??= new ClangToolchain(LinuxToolchainLocator.Locate());
    }
}
