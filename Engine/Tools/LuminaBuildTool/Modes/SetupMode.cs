using System.Diagnostics;
using System.Formats.Tar;
using System.IO.Compression;
using System.Security.Cryptography;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Platform;

namespace LuminaBuildTool.Modes;

/// <summary>
/// First-time setup: fetch and verify the prebuilt dependency bundle, persist LUMINA_DIR and
/// point git at the repository's hooks.
/// </summary>
public static class SetupMode
{
    private const string ReleaseUrl =
        "https://github.com/MrDrElliot/LuminaEngine/releases/download/external-deps";

    private const string HooksPath = "BuildScripts/Hooks";

    private enum ArchiveFormat
    {
        Zip,

        TarGz,
    }

    /// <summary>One downloadable dependency bundle and the SHA-256 it must match.</summary>
    private sealed record DependencyBundle
    {
        public required BuildPlatform Platform { get; init; }

        public required string AssetName { get; init; }

        public required ArchiveFormat Format { get; init; }

        public required string Sha256 { get; init; }

        public required string SentinelPath { get; init; }

        public string Url => $"{ReleaseUrl}/{AssetName}";

        public bool bIsPublished => Sha256.Length > 0 || Platform == BuildPlatform.Windows64;
    }

    private static readonly DependencyBundle[] Bundles =
    {
        new()
        {
            Platform = BuildPlatform.Windows64,

            AssetName = "External-Win64.zip",
            Format = ArchiveFormat.Zip,
            Sha256 = "6CF9D85C9A52F4663929F33C19FD8242268D55E565C19950696A48D0299D00DD",
            SentinelPath = "LLVM/bin/libclang.dll",
        },
        new()
        {
            Platform = BuildPlatform.Linux64,
            AssetName = "External-Linux64.tar.gz",
            Format = ArchiveFormat.TarGz,

            Sha256 = "1D0232C93DB3445C39BEE4339FDED7C41A8714E8432106F3304F02A3EE4B33FC",
            SentinelPath = "LLVM/lib/libclang.so",
        },
    };

    private static readonly (string Name, string Use)[] DependencyManifest =
    {
        (".NET 10 runtime + hosting headers", "C# scripting host (LuminaSharp)"),
        ("LLVM/Clang 19 (libclang)", "reflection codegen (Reflector)"),
        ("Slang shader compiler", "shader compilation to SPIR-V"),
        ("RenderDoc", "in-app GPU frame capture"),
        ("Tracy", "CPU and GPU profiler"),
    };

    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories Directories, CancellationToken Cancellation)
    {
        Log.Info("==========================================================");
        Log.Info("                 LUMINA ENGINE SETUP                      ");
        Log.Info("==========================================================");
        Log.Info("Engine root: {0}", Directories.EngineRoot);

        Log.Info(string.Empty);
        Log.Info("[1/3] Environment");
        PersistEngineDirectory(Directories.EngineRoot);

        Log.Info(string.Empty);
        Log.Info("[2/3] External dependencies");

        BuildPlatform HostPlatform = BuildPlatformRegistry.HostPlatform;
        DependencyBundle? Bundle = Bundles.FirstOrDefault(B => B.Platform == HostPlatform);

        if (Bundle is null)
        {
            Log.Error("No dependency bundle is published for {0}.", HostPlatform);
            Log.Error("DEPENDENCIES.md lists each upstream if you want to assemble one by hand.");
            return 1;
        }

        string ExternalDirectory = Path.Combine(Directories.EngineRoot, "External");
        string Sentinel = Path.Combine(ExternalDirectory, Bundle.SentinelPath.Replace('/', Path.DirectorySeparatorChar));
        bool bForce = Arguments.HasFlag("Force");

        if (File.Exists(Sentinel) && !bForce)
        {
            Log.Info("{0} dependencies are already installed; skipping the download. Pass -Force to refresh them.", HostPlatform);
        }
        else
        {
            if (Directory.Exists(ExternalDirectory) && !File.Exists(Sentinel))
            {
                Log.Info("External/ exists but holds nothing for {0} ({1} is missing).", HostPlatform, Bundle.SentinelPath);
                Log.Info("Fetching this platform's bundle; the bundles are built to coexist, so anything already there stays.");
            }

            if (!await InstallDependenciesAsync(Arguments, Bundle, Directories.EngineRoot, Cancellation).ConfigureAwait(false))
            {
                return 1;
            }
        }

        Log.Info(string.Empty);
        Log.Info("[3/3] Git hooks");
        ConfigureGitHooks(Directories.EngineRoot);

        Log.Info(string.Empty);
        Log.Info("Setup complete. Next: run GenerateProjectFiles.bat, or build directly with");
        Log.Info("  LuminaBuild.bat Build Lumina -TargetType=Editor");

        return 0;
    }

    private static async Task<bool> InstallDependenciesAsync(
        CommandLine Arguments,
        DependencyBundle Bundle,
        string EngineRoot,
        CancellationToken Cancellation)
    {
        if (!Bundle.bIsPublished)
        {
            Log.Error("No dependency bundle has been published for {0} yet.", Bundle.Platform);
            Log.Error(string.Empty);
            Log.Error("To build one: BuildScripts/MakeLinuxBundle.sh, which fetches each upstream and");
            Log.Error("lays it out to match the paths the build expects. Then upload it as");
            Log.Error("'{0}' on the external-deps release and pin the SHA-256 it prints", Bundle.AssetName);
            Log.Error("in SetupMode.Bundles.");
            return false;
        }

        if (!ConfirmDownload(Arguments, Bundle))
        {
            Log.Warning("Setup cancelled. No files were downloaded.");
            return false;
        }

        string ArchivePath = Path.Combine(EngineRoot, Bundle.AssetName);

        if (!await DownloadAsync(Bundle.Url, ArchivePath, Cancellation).ConfigureAwait(false))
        {
            return false;
        }

        if (!VerifyChecksum(ArchivePath, Bundle.Sha256))
        {
            File.Delete(ArchivePath);
            Log.Error("Deleted the failed download. Setup aborted.");
            return false;
        }

        try
        {
            Log.Info("Extracting into {0}...", EngineRoot);
            Extract(ArchivePath, Bundle.Format, EngineRoot);
        }
        catch (Exception Ex) when (Ex is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            Log.Error("Extraction failed: {0}", Ex.Message);
            Log.Error("Leaving {0} in place for inspection.", Bundle.AssetName);
            return false;
        }

        File.Delete(ArchivePath);

        string Sentinel = Path.Combine(EngineRoot, "External", Bundle.SentinelPath.Replace('/', Path.DirectorySeparatorChar));

        if (!File.Exists(Sentinel))
        {
            Log.Error("The bundle extracted but does not contain External/{0}.", Bundle.SentinelPath);
            Log.Error("It is either the wrong platform's asset or incomplete; the build will fail on it later.");
            return false;
        }

        Log.Info("Dependencies installed for {0}.", Bundle.Platform);

        return true;
    }

    private static bool ConfirmDownload(CommandLine Arguments, DependencyBundle Bundle)
    {
        if (Arguments.HasFlag("Yes") || Environment.GetEnvironmentVariable("LUMINA_SETUP_YES") is not null)
        {
            return true;
        }

        Log.Info("------------------------------------------------------------");
        Log.Info(" External dependencies for {0} (prebuilt bundle, several hundred MB)", Bundle.Platform);
        Log.Info("------------------------------------------------------------");

        foreach ((string Name, string Use) in DependencyManifest)
        {
            Log.Info("   {0,-36} {1}", Name, Use);
        }

        Log.Info(string.Empty);
        Log.Info(" Source:  {0}", Bundle.Url);
        Log.Info(" Details: DEPENDENCIES.md lists upstream sources, versions and licenses.");
        Log.Info(string.Empty);

        Console.Out.Write("Proceed with download? [Y/n] ");
        Console.Out.Flush();

        string? Answer = Console.ReadLine();

        if (Answer is null)
        {
            Log.Warning("No input available. Pass -Yes or set LUMINA_SETUP_YES=1 to confirm non-interactively.");
            return false;
        }

        Answer = Answer.Trim();

        return !Answer.Equals("n", StringComparison.OrdinalIgnoreCase)
            && !Answer.Equals("no", StringComparison.OrdinalIgnoreCase);
    }

    private static async Task<bool> DownloadAsync(string Url, string Destination, CancellationToken Cancellation)
    {
        Log.Info("Downloading {0}", Url);
        Log.Info("    into    {0}", Destination);

        try
        {
            using HttpClient Client = new() { Timeout = TimeSpan.FromMinutes(30) };
            using HttpResponseMessage Response = await Client
                .GetAsync(Url, HttpCompletionOption.ResponseHeadersRead, Cancellation)
                .ConfigureAwait(false);

            Response.EnsureSuccessStatusCode();

            long? Total = Response.Content.Headers.ContentLength;

            await using Stream Source = await Response.Content.ReadAsStreamAsync(Cancellation).ConfigureAwait(false);
            await using FileStream Target = File.Create(Destination);

            byte[] Buffer = new byte[1 << 20];
            long Received = 0;
            int LastPercent = -1;
            int Read;

            while ((Read = await Source.ReadAsync(Buffer, Cancellation).ConfigureAwait(false)) > 0)
            {
                await Target.WriteAsync(Buffer.AsMemory(0, Read), Cancellation).ConfigureAwait(false);
                Received += Read;

                if (Total is > 0)
                {
                    int Percent = (int)(Received * 100 / Total.Value);

                    if (Percent != LastPercent && Percent % 5 == 0)
                    {
                        LastPercent = Percent;
                        Log.Info("  {0,3}%  ({1:F1} MB / {2:F1} MB)", Percent, Received / 1048576.0, Total.Value / 1048576.0);
                    }
                }
            }

            Log.Info("Download complete.");
            return true;
        }
        catch (Exception Ex) when (Ex is HttpRequestException or IOException or TaskCanceledException)
        {
            Log.Error("Download failed: {0}", Ex.Message);
            return false;
        }
    }

    private static void Extract(string ArchivePath, ArchiveFormat Format, string EngineRoot)
    {
        if (Format == ArchiveFormat.Zip)
        {
            ZipFile.ExtractToDirectory(ArchivePath, EngineRoot, overwriteFiles: true);
            return;
        }

        using FileStream Compressed = File.OpenRead(ArchivePath);
        using GZipStream Decompressed = new(Compressed, CompressionMode.Decompress);

        TarFile.ExtractToDirectory(Decompressed, EngineRoot, overwriteFiles: true);
    }

    private static bool VerifyChecksum(string FilePath, string Expected)
    {
        string Actual;

        using (FileStream Stream = File.OpenRead(FilePath))
        {
            Actual = Convert.ToHexString(SHA256.HashData(Stream));
        }

        if (string.IsNullOrEmpty(Expected))
        {
            Log.Warning("No SHA-256 is pinned, so the bundle's integrity was not verified.");
            Log.Warning("Downloaded SHA-256: {0}", Actual);
            Log.Warning("Maintainer: record this in SetupMode.ExpectedSha256 to lock the bundle.");
            return true;
        }

        if (Actual.Equals(Expected, StringComparison.OrdinalIgnoreCase))
        {
            Log.Info("SHA-256 verified: {0}", Actual);
            return true;
        }

        Log.Error("SHA-256 mismatch. Refusing to extract an untrusted bundle.");
        Log.Error("  expected: {0}", Expected);
        Log.Error("  actual:   {0}", Actual);

        return false;
    }

    /// <summary>
    /// Persists LUMINA_DIR for future shells and verifies it took: setx can fail quietly on a
    /// locked or roaming profile, and a missing value breaks every game project built later.
    /// </summary>
    private static void PersistEngineDirectory(string EngineRoot)
    {
        Environment.SetEnvironmentVariable("LUMINA_DIR", EngineRoot);

        if (!OperatingSystem.IsWindows())
        {
            Log.Info("Set LUMINA_DIR for this process. Add it to your shell profile to make it permanent.");
            return;
        }

        try
        {
            Environment.SetEnvironmentVariable("LUMINA_DIR", EngineRoot, EnvironmentVariableTarget.User);

            string? Persisted = Environment.GetEnvironmentVariable("LUMINA_DIR", EnvironmentVariableTarget.User);

            if (Persisted is null || !PathUtils.Normalize(Persisted).Equals(PathUtils.Normalize(EngineRoot), StringComparison.OrdinalIgnoreCase))
            {
                Log.Warning("LUMINA_DIR did not persist to the user environment; it is set for this session only.");
                return;
            }

            Log.Info("LUMINA_DIR persisted as {0}", Persisted);
            Log.Info("Already-running editors and IDEs keep the old value until they restart.");
        }
        catch (Exception Ex) when (Ex is System.Security.SecurityException or UnauthorizedAccessException)
        {
            Log.Warning("Could not persist LUMINA_DIR: {0}", Ex.Message);
        }
    }

    private static void ConfigureGitHooks(string EngineRoot)
    {
        if (!Directory.Exists(Path.Combine(EngineRoot, ".git")))
        {
            Log.Info("Not a git repository; skipping hooks configuration.");
            return;
        }

        if (!Directory.Exists(Path.Combine(EngineRoot, HooksPath)))
        {
            Log.Info("{0} not found; skipping hooks configuration.", HooksPath);
            return;
        }

        try
        {
            ProcessStartInfo StartInfo = new("git")
            {
                WorkingDirectory = EngineRoot,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };

            StartInfo.ArgumentList.Add("config");
            StartInfo.ArgumentList.Add("core.hooksPath");
            StartInfo.ArgumentList.Add(HooksPath);

            using Process? Runner = Process.Start(StartInfo);

            if (Runner is null)
            {
                Log.Warning("Could not run git to configure hooks.");
                return;
            }

            Runner.WaitForExit(30_000);

            if (Runner.ExitCode == 0)
            {
                Log.Info("Git hooks path set to {0}", HooksPath);
            }
            else
            {
                Log.Warning("git config returned {0}; hooks were not configured.", Runner.ExitCode);
            }
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or IOException)
        {
            Log.Warning("Could not configure git hooks: {0}", Ex.Message);
        }
    }
}
