using LuminaBuildTool.Configuration;
using LuminaBuildTool.Toolchain;

namespace LuminaBuildTool.Platform;

/// <summary>What the core build system needs about a platform, without knowing its toolchain.</summary>
public interface IBuildPlatform
{
    BuildPlatform Platform { get; }

    string SharedLibraryPrefix { get; }

    string SharedLibraryExtension { get; }

    string StaticLibraryPrefix { get; }

    string StaticLibraryExtension { get; }

    string ExecutableExtension { get; }

    string ObjectFileExtension { get; }

    /// <summary>Import library beside a shared library, or null when the platform links them directly.</summary>
    string? ImportLibraryExtension { get; }

    /// <summary>Definitions every module on this platform receives.</summary>
    IEnumerable<string> GetPlatformDefinitions(TargetInfo Info);

    /// <summary>Resolves the toolchain used to compile and link for this platform.</summary>
    IToolchain CreateToolchain(TargetInfo Info);
}
