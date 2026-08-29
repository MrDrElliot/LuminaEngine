namespace LuminaBuildTool.Configuration;

/// <summary>Optimization and debug level. Mirrors the engine's historical Premake configurations.</summary>
public enum BuildConfiguration
{
    Debug,
    Development,
    Shipping,
}

/// <summary>Target platform.</summary>
public enum BuildPlatform
{
    Windows64,
    Linux64,
    Mac64,
}

/// <summary>What a target produces.</summary>
public enum TargetType
{
    Editor,
    Game,
    Program,
}

/// <summary>Link-time role of a module's own output.</summary>
public enum ModuleBinaryType
{
    /// <summary>Headers, defines and prebuilt libraries only. Compiles nothing.</summary>
    HeaderOnly,

    StaticLibrary,

    SharedLibrary,

    ConsoleApplication,

    WindowedApplication,
}

/// <summary>Which target types a module is allowed to appear in.</summary>
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

/// <summary>Which half of a profile guided optimization cycle a build is.</summary>
public enum PgoMode
{
    Off,

    /// <summary>Instrumented, so running the binary writes a profile. Slower than an ordinary build.</summary>
    Instrument,

    /// <summary>Optimized against a profile an instrumented run already collected.</summary>
    Optimize,
}

/// <summary>What LUMINA_FORCEINLINE_HINT expands to for a target.</summary>
public enum ForceInlineHintPolicy
{
    /// <summary>Decided by configuration in the engine's target rules.</summary>
    Default,

    /// <summary>Forced, matching plain FORCEINLINE.</summary>
    Force,

    /// <summary>Demoted to inline, leaving the choice to the optimizer and to PGO.</summary>
    Hint,
}

/// <summary>How a module participates in precompiled headers.</summary>
public enum PrecompiledHeaderMode
{
    None,

    /// <summary>Module declares and compiles its own PCH.</summary>
    Create,
}

public static class BuildEnumExtensions
{
    /// <summary>Operating system identity, without architecture.</summary>
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

    /// <summary>Directory name under Binaries, for example "Windows64".</summary>
    public static string GetOutputDirectoryName(this BuildPlatform Platform)
    {
        return Platform.GetSystemName() + Platform.GetArchitectureName();
    }

    public static string GetExecutableExtension(this BuildPlatform Platform)
    {
        return Platform == BuildPlatform.Windows64 ? ".exe" : string.Empty;
    }

    public static bool ProducesExecutable(this ModuleBinaryType Type)
    {
        return Type is ModuleBinaryType.ConsoleApplication or ModuleBinaryType.WindowedApplication;
    }

    public static bool ProducesCompiledOutput(this ModuleBinaryType Type)
    {
        return Type != ModuleBinaryType.HeaderOnly;
    }

    /// <summary>True when the module links into dependents rather than being loaded at run time.</summary>
    public static bool IsLinkable(this ModuleBinaryType Type)
    {
        return Type is ModuleBinaryType.StaticLibrary or ModuleBinaryType.SharedLibrary;
    }

    /// <summary>True when the output is a loadable image rather than something absorbed into one.</summary>
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
