using System.Diagnostics;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Toolchain.Linux;

/// <summary>Which compiler family a located installation belongs to.</summary>
public enum CompilerFamily
{
    Clang,

    Gcc,
}

public sealed class UnixToolchainInstallation
{
    public required CompilerFamily Family { get; init; }

    /// <summary>Compiler driver, used for both compiling and linking.</summary>
    public required string CompilerPath { get; init; }

    /// <summary>Driver used for C sources. Same installation, different front end default.</summary>
    public required string CCompilerPath { get; init; }

    public required string ArchiverPath { get; init; }

    public required string? LinkerName { get; init; }

    /// <summary>
    /// Library carrying the backtrace implementation behind std::stacktrace, without the "lib"
    /// prefix, or null when this toolchain ships no such archive.
    /// </summary>
    /// <remarks>
    /// libstdc++ declares std::stacktrace in the header but leaves the implementation in a separate
    /// archive, and which archive that is has moved: GCC 13 shipped libstdc++_libbacktrace.a, and
    /// from GCC 14 the implementation lives in libstdc++exp.a with the old archive gone entirely.
    /// Hardcoding either name breaks every toolchain that uses the other, which is what a link
    /// against a GCC 16 installation used to fail on.
    ///
    /// Null is a normal answer, not a failure: a libc++ toolchain leaves __cpp_lib_stacktrace
    /// undefined, so Assert.h compiles its non-stacktrace path and there is nothing to link.
    /// </remarks>
    public required string? StacktraceLibrary { get; init; }

    /// <summary>Version string as the compiler reports it, for example "18.1.3".</summary>
    public required string Version { get; init; }

    public string Name => Family == CompilerFamily.Clang ? "Clang" : "GCC";

    public override string ToString()
    {
        string Linker = LinkerName is null ? "default linker" : LinkerName;
        return $"{Name} {Version} ({Linker})";
    }
}

public static class LinuxToolchainLocator
{
    private static UnixToolchainInstallation? Cached;

    private static readonly (CompilerFamily Family, string Cxx, string Cc)[] Candidates =
    {
        (CompilerFamily.Clang, "clang++", "clang"),
        (CompilerFamily.Gcc, "g++", "gcc"),
    };

    public static UnixToolchainInstallation Locate()
    {
        if (Cached is not null)
        {
            return Cached;
        }

        UnixToolchainInstallation? Found = LocateFromEnvironment() ?? LocateFromPath();

        if (Found is null)
        {
            throw new BuildException(
                "No C++ toolchain was found. Install clang or gcc, or point CXX at a compiler driver.");
        }

        Log.Verbose("Using {0} at '{1}'", Found, Found.CompilerPath);
        Cached = Found;

        return Found;
    }

    private static UnixToolchainInstallation? LocateFromEnvironment()
    {
        string? Cxx = Environment.GetEnvironmentVariable("CXX");

        if (string.IsNullOrWhiteSpace(Cxx))
        {
            return null;
        }

        string? ResolvedCxx = ResolveTool(Cxx);

        if (ResolvedCxx is null)
        {
            throw new BuildException($"CXX is set to '{Cxx}', which is not an executable this tool can find.");
        }

        string? Cc = Environment.GetEnvironmentVariable("CC");
        string ResolvedCc = (string.IsNullOrWhiteSpace(Cc) ? null : ResolveTool(Cc)) ?? DeriveCCompiler(ResolvedCxx);

        return Describe(ResolvedCxx, ResolvedCc);
    }

    private static UnixToolchainInstallation? LocateFromPath()
    {
        foreach ((CompilerFamily _, string Cxx, string Cc) in Candidates)
        {
            string? ResolvedCxx = ResolveTool(Cxx);

            if (ResolvedCxx is null)
            {
                continue;
            }

            UnixToolchainInstallation? Installation = Describe(ResolvedCxx, ResolveTool(Cc) ?? ResolvedCxx);

            if (Installation is not null)
            {
                return Installation;
            }
        }

        return null;
    }

    private static UnixToolchainInstallation? Describe(string CompilerPath, string CCompilerPath)
    {
        string? Banner = RunAndCaptureFirstLine(CompilerPath, "--version");

        if (Banner is null)
        {
            return null;
        }

        CompilerFamily Family;

        if (Banner.Contains("clang", StringComparison.OrdinalIgnoreCase))
        {
            Family = CompilerFamily.Clang;
        }
        else if (Banner.Contains("gcc", StringComparison.OrdinalIgnoreCase)
            || Banner.Contains("g++", StringComparison.OrdinalIgnoreCase)
            || Banner.Contains("Free Software Foundation", StringComparison.OrdinalIgnoreCase))
        {
            Family = CompilerFamily.Gcc;
        }
        else
        {
            Log.Verbose("Ignoring '{0}': unrecognized compiler banner '{1}'", CompilerPath, Banner);
            return null;
        }

        return new UnixToolchainInstallation
        {
            Family = Family,
            CompilerPath = CompilerPath,
            CCompilerPath = CCompilerPath,
            ArchiverPath = LocateArchiver(Family),
            LinkerName = LocateFastLinker(),
            StacktraceLibrary = LocateStacktraceLibrary(CompilerPath),
            Version = ExtractVersion(Banner),
        };
    }

    /// <summary>
    /// Asks the compiler which of the known std::stacktrace support archives it actually ships.
    /// </summary>
    /// <remarks>
    /// Asked of the driver rather than worked out from the version number, because the archive that
    /// exists is a property of the installation and not of the release it claims to be: a distro
    /// snapshot can ship either one, and the driver is the only thing that knows where its own
    /// library directory is anyway.
    ///
    /// libstdc++exp.a comes first because it is the superset. GCC 13 ships both, and its exp
    /// archive already contains the same backtrace objects as libstdc++_libbacktrace.a, so
    /// preferring it is correct on the versions that offer a choice and is the only answer from
    /// GCC 14 on.
    ///
    /// -print-file-name echoes the name back unchanged when it resolves nothing, so the result only
    /// counts as a hit if it came back as a path that exists.
    /// </remarks>
    private static string? LocateStacktraceLibrary(string CompilerPath)
    {
        foreach (string Candidate in new[] { "stdc++exp", "stdc++_libbacktrace" })
        {
            string? Resolved = RunAndCaptureFirstLine(CompilerPath, $"-print-file-name=lib{Candidate}.a");

            if (!string.IsNullOrWhiteSpace(Resolved) && File.Exists(Resolved.Trim()))
            {
                Log.Verbose("std::stacktrace support library: {0} ({1})", Candidate, Resolved.Trim());
                return Candidate;
            }
        }

        Log.Verbose("No std::stacktrace support library found for '{0}'.", CompilerPath);
        return null;
    }

    private static string LocateArchiver(CompilerFamily Family)
    {
        string? Preferred = Environment.GetEnvironmentVariable("AR");

        if (!string.IsNullOrWhiteSpace(Preferred) && ResolveTool(Preferred) is string Resolved)
        {
            return Resolved;
        }

        if (Family == CompilerFamily.Clang && ResolveTool("llvm-ar") is string LlvmAr)
        {
            return LlvmAr;
        }

        if (Family == CompilerFamily.Gcc && ResolveTool("gcc-ar") is string GccAr)
        {
            return GccAr;
        }

        return ResolveTool("ar") ?? "ar";
    }

    private static string? LocateFastLinker()
    {
        string? Preferred = Environment.GetEnvironmentVariable("LUMINA_LINKER");

        if (!string.IsNullOrWhiteSpace(Preferred))
        {
            return Preferred;
        }

        if (ResolveTool("ld.lld") is not null)
        {
            return "lld";
        }

        if (ResolveTool("ld.mold") is not null || ResolveTool("mold") is not null)
        {
            return "mold";
        }

        return ResolveTool("ld.gold") is not null ? "gold" : null;
    }

    private static string DeriveCCompiler(string CxxPath)
    {
        string Directory = Path.GetDirectoryName(CxxPath) ?? string.Empty;
        string Name = Path.GetFileName(CxxPath);

        string? CName = Name switch
        {
            _ when Name.Contains("clang++", StringComparison.Ordinal) => Name.Replace("clang++", "clang", StringComparison.Ordinal),
            _ when Name.Contains("g++", StringComparison.Ordinal) => Name.Replace("g++", "gcc", StringComparison.Ordinal),
            _ => null,
        };

        if (CName is null)
        {
            return CxxPath;
        }

        string Candidate = Path.Combine(Directory, CName);

        return File.Exists(Candidate) ? Candidate : CxxPath;
    }

    private static string? ResolveTool(string NameOrPath)
    {
        string[] Suffixes = OperatingSystem.IsWindows() ? new[] { string.Empty, ".exe" } : new[] { string.Empty };

        if (NameOrPath.Contains(Path.DirectorySeparatorChar) || NameOrPath.Contains('/'))
        {
            foreach (string Suffix in Suffixes)
            {
                if (File.Exists(NameOrPath + Suffix))
                {
                    return Path.GetFullPath(NameOrPath + Suffix);
                }
            }

            return null;
        }

        string? PathVariable = Environment.GetEnvironmentVariable("PATH");

        if (PathVariable is null)
        {
            return null;
        }

        foreach (string Directory in PathVariable.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            foreach (string Suffix in Suffixes)
            {
                string Candidate = Path.Combine(Directory, NameOrPath + Suffix);

                if (File.Exists(Candidate))
                {
                    return Candidate;
                }
            }
        }

        return null;
    }

    /// <summary>First dotted version number in the banner, or the whole banner when there is none.</summary>
    private static string ExtractVersion(string Banner)
    {
        foreach (string Token in Banner.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            string Trimmed = Token.Trim('(', ')', ',');

            if (Trimmed.Length > 0 && char.IsDigit(Trimmed[0]) && Trimmed.Contains('.'))
            {
                return Trimmed;
            }
        }

        return Banner;
    }

    private static string? RunAndCaptureFirstLine(string Executable, string Arguments)
    {
        try
        {
            ProcessStartInfo StartInfo = new(Executable, Arguments)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };

            using Process? Running = Process.Start(StartInfo);

            if (Running is null)
            {
                return null;
            }

            string Output = Running.StandardOutput.ReadToEnd();
            Running.WaitForExit(10_000);

            if (Running.ExitCode != 0)
            {
                return null;
            }

            return Output.Split('\n').FirstOrDefault()?.Trim();
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            Log.Verbose("Could not run '{0} {1}': {2}", Executable, Arguments, Ex.Message);
            return null;
        }
    }
}
