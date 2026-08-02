namespace LuminaBuildTool.Configuration;

/// <summary>
/// Optimization and debug level. Mirrors the engine's historical Premake configurations.
/// </summary>
public enum BuildConfiguration
{
    Debug,
    Development,
    Shipping,
}

/// <summary>
/// Target platform. Values here are the platform identity used for output directories and
/// toolchain selection; adding a platform means adding an IBuildPlatform implementation.
/// </summary>
public enum BuildPlatform
{
    Windows64,
    Linux64,
    Mac64,
}

/// <summary>
/// What a target produces. Editor and Game differ by WITH_EDITOR and by which modules link;
/// Program covers standalone tools such as the Reflector.
/// </summary>
public enum TargetType
{
    Editor,
    Game,
    Program,
}

/// <summary>
/// Link-time role of a module's own output.
/// </summary>
public enum ModuleBinaryType
{
    /// <summary>Headers, defines and prebuilt libraries only. Compiles nothing.</summary>
    HeaderOnly,

    StaticLibrary,

    SharedLibrary,

    ConsoleApplication,

    WindowedApplication,
}

/// <summary>
/// Which target types a module is allowed to appear in.
/// </summary>
public enum ModuleHostType
{
    /// <summary>Available to every target type.</summary>
    Runtime,

    /// <summary>Editor targets only. Excluded from Game and Program targets.</summary>
    Editor,

    /// <summary>Tools and non-shipping targets.</summary>
    Developer,

    /// <summary>Belongs to a single Program target.</summary>
    Program,
}

/// <summary>
/// How a module participates in precompiled headers.
/// </summary>
public enum PrecompiledHeaderMode
{
    None,

    /// <summary>Module declares and compiles its own PCH.</summary>
    Create,
}

public static class BuildEnumExtensions
{
    /// <summary>
    /// Operating system identity, without architecture. Baked into the engine as
    /// LUMINA_SYSTEM_NAME and matched against a .lplugin's SupportedPlatforms.
    /// </summary>
    public static string GetSystemName(this BuildPlatform Platform)
    {
        return Platform switch
        {
            BuildPlatform.Windows64 => "Windows",
            BuildPlatform.Linux64 => "Linux",
            BuildPlatform.Mac64 => "Macosx",
            _ => Platform.ToString(),
        };
    }

    public static string GetArchitectureName(this BuildPlatform Platform)
    {
        return Platform switch
        {
            BuildPlatform.Windows64 or BuildPlatform.Linux64 => "64",
            BuildPlatform.Mac64 => "64",
            _ => "64",
        };
    }

    /// <summary>
    /// Directory name under Binaries, for example "Windows64". Baked into the engine as
    /// LUMINA_PLATFORM_NAME, which is what resolves plugin and game module DLL paths at runtime,
    /// so this string cannot change without breaking module loading.
    /// </summary>
    public static string GetOutputDirectoryName(this BuildPlatform Platform)
    {
        return Platform.GetSystemName() + Platform.GetArchitectureName();
    }

    public static string GetExecutableExtension(this BuildPlatform Platform)
    {
        return Platform == BuildPlatform.Windows64 ? ".exe" : string.Empty;
    }

    public static string GetSharedLibraryExtension(this BuildPlatform Platform)
    {
        return Platform switch
        {
            BuildPlatform.Windows64 => ".dll",
            BuildPlatform.Linux64 => ".so",
            BuildPlatform.Mac64 => ".dylib",
            _ => string.Empty,
        };
    }

    public static bool ProducesExecutable(this ModuleBinaryType Type)
    {
        return Type is ModuleBinaryType.ConsoleApplication or ModuleBinaryType.WindowedApplication;
    }

    public static bool ProducesCompiledOutput(this ModuleBinaryType Type)
    {
        return Type != ModuleBinaryType.HeaderOnly;
    }

    /// <summary>
    /// True when the module is linked into dependents rather than loaded, which is what decides
    /// whether dependents see it on their link line.
    /// </summary>
    public static bool IsLinkable(this ModuleBinaryType Type)
    {
        return Type is ModuleBinaryType.StaticLibrary or ModuleBinaryType.SharedLibrary;
    }

    /// <summary>
    /// True when the output is a loadable image in its own right rather than something absorbed
    /// into one. Decides where definitions that must exist once per image belong.
    /// </summary>
    public static bool IsLoadableImage(this ModuleBinaryType Type)
    {
        return Type is ModuleBinaryType.SharedLibrary
            or ModuleBinaryType.ConsoleApplication
            or ModuleBinaryType.WindowedApplication;
    }

    public static bool IsAvailableIn(this ModuleHostType Host, TargetType Target)
    {
        return Host switch
        {
            ModuleHostType.Runtime => true,
            ModuleHostType.Editor => Target == TargetType.Editor,
            ModuleHostType.Developer => Target != TargetType.Game,
            ModuleHostType.Program => Target == TargetType.Program,
            _ => true,
        };
    }
}
