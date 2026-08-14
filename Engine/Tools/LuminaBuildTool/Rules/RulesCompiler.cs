using System.Reflection;
using System.Runtime.Loader;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;

namespace LuminaBuildTool.Rules;

/// <summary>Manifest describing the inputs a cached rules assembly was built from.</summary>
public sealed class RulesCacheManifest
{
    public string ToolVersion { get; set; } = string.Empty;

    public string SourceHash { get; set; } = string.Empty;

    public List<string> SourceFiles { get; set; } = new();
}

/// <summary>Compiles the discovered Target.cs and Build.cs into one cached assembly.</summary>
public static class RulesCompiler
{
    public static Assembly CompileOrLoad(BuildDirectories Directories, IReadOnlyList<RulesFile> RulesFiles, bool bForceRecompile)
    {
        string CacheDirectory = Path.Combine(Directories.BuildToolIntermediatesDirectory, "Rules");
        string AssemblyPath = Path.Combine(CacheDirectory, "LuminaRules.dll");
        string ManifestPath = Path.Combine(CacheDirectory, "LuminaRules.manifest.json");

        List<string> SourcePaths = RulesFiles.Select(F => F.Location).OrderBy(P => P, StringComparer.OrdinalIgnoreCase).ToList();
        string ExpectedHash = ComputeSourceHash(SourcePaths);
        string ToolVersion = GetToolVersion();

        if (!bForceRecompile && IsCacheCurrent(ManifestPath, AssemblyPath, ExpectedHash, ToolVersion))
        {
            Log.Verbose("Reusing cached rules assembly ({0} rules files)", SourcePaths.Count);
            return LoadAssembly(AssemblyPath);
        }

        // Several builds can start at once, and they would all write this assembly. Whoever gets
        // the lock compiles it; the rest find it current and just load it.
        using BuildLock CompileLock = BuildLock.Acquire(
            Directories.OutputRoot,
            "rules|" + CacheDirectory,
            "another build to finish compiling the build rules",
            TimeSpan.FromMinutes(10));

        if (!bForceRecompile && IsCacheCurrent(ManifestPath, AssemblyPath, ExpectedHash, ToolVersion))
        {
            Log.Verbose("Another build compiled the rules assembly; reusing it.");
            return LoadAssembly(AssemblyPath);
        }

        Log.Info("Compiling {0} build rules files...", SourcePaths.Count);
        Compile(SourcePaths, AssemblyPath);

        JsonStore.Save(ManifestPath, new RulesCacheManifest
        {
            ToolVersion = ToolVersion,
            SourceHash = ExpectedHash,
            SourceFiles = SourcePaths,
        });

        return LoadAssembly(AssemblyPath);
    }

    private static bool IsCacheCurrent(string ManifestPath, string AssemblyPath, string ExpectedHash, string ToolVersion)
    {
        if (!File.Exists(AssemblyPath))
        {
            return false;
        }

        RulesCacheManifest? Cached = JsonStore.Load<RulesCacheManifest>(ManifestPath);

        return Cached is not null && Cached.SourceHash == ExpectedHash && Cached.ToolVersion == ToolVersion;
    }

    /// <summary>Hash covers the file set plus each timestamp and size, so any rules edit invalidates it.</summary>
    public static string ComputeSourceHash(IEnumerable<string> SourcePaths)
    {
        return ContentHash.OfFiles(SourcePaths.OrderBy(P => P, StringComparer.OrdinalIgnoreCase));
    }

    public static string GetToolVersion()
    {
        FileItem ToolAssembly = FileItem.Get(Assembly.GetExecutingAssembly().Location);
        return $"{ToolAssembly.Timestamp.Ticks}-{ToolAssembly.Length}";
    }

    private static void Compile(IReadOnlyList<string> SourcePaths, string AssemblyPath)
    {
        List<SyntaxTree> Trees = new(SourcePaths.Count);
        CSharpParseOptions ParseOptions = new(LanguageVersion.Latest);

        foreach (string SourcePath in SourcePaths)
        {
            string Text = File.ReadAllText(SourcePath);
            Trees.Add(CSharpSyntaxTree.ParseText(Text, ParseOptions, path: SourcePath));
        }

        CSharpCompilationOptions Options = new(
            OutputKind.DynamicallyLinkedLibrary,
            optimizationLevel: OptimizationLevel.Release,
            // Rules files are terse by design; do not fail them over unused usings.
            warningLevel: 4,
            nullableContextOptions: NullableContextOptions.Annotations);

        CSharpCompilation Compilation = CSharpCompilation.Create(
            "LuminaRules",
            Trees,
            BuildReferences(),
            Options);

        PathUtils.EnsureDirectoryForFile(AssemblyPath);

        using MemoryStream AssemblyStream = new();
        EmitResult Result = Compilation.Emit(AssemblyStream);

        if (!Result.Success)
        {
            List<string> Errors = Result.Diagnostics
                .Where(D => D.Severity == DiagnosticSeverity.Error)
                .Select(FormatDiagnostic)
                .ToList();

            throw new BuildException(
                "Build rules failed to compile:" + Environment.NewLine + string.Join(Environment.NewLine, Errors));
        }

        foreach (Diagnostic Warning in Result.Diagnostics.Where(D => D.Severity == DiagnosticSeverity.Warning))
        {
            Log.Verbose("{0}", FormatDiagnostic(Warning));
        }

        File.WriteAllBytes(AssemblyPath, AssemblyStream.ToArray());
        FileItem.Get(AssemblyPath).Invalidate();
    }

    private static string FormatDiagnostic(Diagnostic Diagnostic)
    {
        FileLinePositionSpan Span = Diagnostic.Location.GetLineSpan();
        string Location = Span.Path.Length > 0
            ? $"{Span.Path}({Span.StartLinePosition.Line + 1},{Span.StartLinePosition.Character + 1})"
            : "rules";

        return $"  {Location}: {Diagnostic.Id}: {Diagnostic.GetMessage()}";
    }

    /// <summary>References the runtime assemblies plus this tool, exposing TargetRules and ModuleRules.</summary>
    private static List<MetadataReference> BuildReferences()
    {
        HashSet<string> Locations = new(StringComparer.OrdinalIgnoreCase);
        List<MetadataReference> References = new();

        void AddReference(string? Location)
        {
            if (string.IsNullOrEmpty(Location) || !File.Exists(Location) || !Locations.Add(Location))
            {
                return;
            }

            References.Add(MetadataReference.CreateFromFile(Location));
        }

        AddReference(typeof(object).Assembly.Location);
        AddReference(typeof(ModuleRules).Assembly.Location);

        foreach (Assembly Loaded in AppDomain.CurrentDomain.GetAssemblies())
        {
            if (!Loaded.IsDynamic)
            {
                AddReference(Loaded.Location);
            }
        }

        // Reference facade assemblies the trimmed loaded set can miss, for example System.Runtime.
        string RuntimeDirectory = Path.GetDirectoryName(typeof(object).Assembly.Location)!;

        foreach (string Facade in new[] { "System.Runtime.dll", "System.Collections.dll", "System.Linq.dll", "netstandard.dll", "System.Console.dll" })
        {
            AddReference(Path.Combine(RuntimeDirectory, Facade));
        }

        return References;
    }

    private static Assembly LoadAssembly(string AssemblyPath)
    {
        // Load from bytes so the file stays writable for the next recompile.
        byte[] Bytes = File.ReadAllBytes(AssemblyPath);
        return AssemblyLoadContext.Default.LoadFromStream(new MemoryStream(Bytes));
    }
}
