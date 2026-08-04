using System;
using LuminaBuildTool.Configuration;

public class BugSplat : LuminaThirdPartyModuleRules
{
    // The BugSplat database is the account subdomain, not a secret: it ships inside the binary and
    // identifies where reports land. The environment variable lets a fork or a CI job report into a
    // different database without editing this file.
    private const string DefaultDatabase = "Lumina-Game-Engine";

    public BugSplat(TargetInfo Target)
        : base(Target)
    {
        // Prebuilt import library plus DLLs copied beside the executable.
        BinaryType = ModuleBinaryType.HeaderOnly;

        // The SDK ships its headers in inc/ and includes them as <BugSplat.h>.
        PublicIncludePaths.Add(ModulePath("inc"));

        // The import library for BugSplat.dll, paired with the flat C API in BugSplatC.h. Only the
        // C ABI crosses the DLL boundary; the C++ BugSplat class lives in the md/mt static libs and
        // linking those would pin this to one CRT and need a separate lib per /MD, /MDd, /MT, /MTd.
        PublicLibraryPaths.Add(ModulePath("lib"));
        PublicSystemLibraries.Add("BugSplat");

        string Database = Environment.GetEnvironmentVariable("BUGSPLAT_DATABASE");
        if (string.IsNullOrEmpty(Database))
        {
            Database = DefaultDatabase;
        }

        PublicDefinitions.Add($"BUGSPLAT_DATABASE=\"{Database}\"");

        // All four have to sit beside the executable. BugSplatMonitor.exe is the out-of-process
        // sender that survives the crash and does the upload; without it a crash writes the local
        // dump and then silently fails to report, which looks exactly like "no crashes happening".
        //
        // These live in lib/ next to the import library rather than the SDK's own bin/, because the
        // repo's .gitignore drops any bin/ as .NET build output and would leave them uncommitted.
        AddRuntimeDependency("lib/BugSplat.dll");
        AddRuntimeDependency("lib/BugSplatMonitor.exe");
        AddRuntimeDependency("lib/BugSplatRc.dll");
        AddRuntimeDependency("lib/BugSplatWer.dll");
    }
}
