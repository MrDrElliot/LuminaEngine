using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace LuminaSharp;

/// One unit's compiled output. The PDB is what gives a script stack trace its file and line, and what lets a debugger bind a breakpoint.
internal readonly struct FScriptImage
{
    public FScriptImage(byte[] Pe, byte[]? Pdb)
    {
        this.Pe = Pe;
        this.Pdb = Pdb;
    }

    public readonly byte[] Pe;
    public readonly byte[]? Pdb;
}

/// Compiles loose C# script sources into one in-memory assembly with Roslyn; scripts reference the TPA set plus this engine assembly.
internal static class ScriptCompiler
{
    private static MetadataReference[]? CachedReferences;

    private static MetadataReference[] References
    {
        get
        {
            if (CachedReferences != null)
            {
                return CachedReferences;
            }

            var Built = new List<MetadataReference>();

            // Every framework assembly the host resolved (BCL).
            string Tpa = AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string ?? string.Empty;
            foreach (string EntryPath in Tpa.Split(Path.PathSeparator))
            {
                if (EntryPath.Length == 0)
                {
                    continue;
                }
                try
                {
                    Built.Add(MetadataReference.CreateFromFile(EntryPath));
                }
                catch
                {
                    // Skip unreadable entries.
                }
            }

            // The engine API assembly (LuminaSharp) so scripts can reference its base classes + generated bindings.
            string SelfPath = typeof(Host).Assembly.Location;
            if (!string.IsNullOrEmpty(SelfPath) && File.Exists(SelfPath))
            {
                Built.Add(MetadataReference.CreateFromFile(SelfPath));
            }

            CachedReferences = Built.ToArray();
            return CachedReferences;
        }
    }

    /// Set only for the packaging compile, whose output ships; every editor compile stays unoptimized so the debugger can read locals.
    public static bool bOptimize { get; set; }

    // Null for a reference built from an image (a sibling unit), which can never shadow a framework assembly.
    private static string? SimpleName(MetadataReference Reference)
    {
        if (Reference is PortableExecutableReference Pe && !string.IsNullOrEmpty(Pe.FilePath))
        {
            return Path.GetFileNameWithoutExtension(Pe.FilePath);
        }
        return null;
    }

    /// Compiles all sources into a PE plus its PDB, or null on error (diagnostics logged). ExtraReferences are the dependency units' emitted images.
    public static FScriptImage? Compile(string AssemblyName, IReadOnlyList<(string Path, string Text)> Sources,
        IReadOnlyList<MetadataReference>? ExtraReferences = null)
    {
        var ParseOptions = new CSharpParseOptions(LanguageVersion.Latest);
        // Roslyn refuses to emit debug info for source text with no encoding, because it cannot checksum it (CS8055).
        SyntaxTree[] Trees = Sources
            .Select(Source => CSharpSyntaxTree.ParseText(SourceText.From(Source.Text, Encoding.UTF8), ParseOptions, path: Source.Path))
            .ToArray();

        MetadataReference[] AllReferences = References;
        if (ExtraReferences != null && ExtraReferences.Count > 0)
        {
            // A package shipping its own copy of a framework assembly has to win, or Roslyn sees two of one identity.
            var Shadowed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (MetadataReference Reference in ExtraReferences)
            {
                if (SimpleName(Reference) is { } Name)
                {
                    Shadowed.Add(Name);
                }
            }

            AllReferences = References
                .Where(Reference => SimpleName(Reference) is not { } Name || !Shadowed.Contains(Name))
                .Concat(ExtraReferences)
                .ToArray();
        }

        // Turn each [Property] field into the native-backed property it has to be. This is the one thing a
        // source generator could not do -- it may only ADD members -- and it is why script authors no longer
        // write `partial`. Done here rather than anywhere else because this compilation is the only one whose
        // output is ever loaded: the generated .csproj is IntelliSense-only, and packaging stages what this
        // emits (ProjectPackager copies FScriptUnit::AssemblyPath).
        //
        // The probe compilation exists only to give the rewriter a semantic model, so it can tell a string
        // from a struct from an enum. Binding is lazy, so this costs the trees it actually inspects.
        var RewriteErrors = new List<string>();
        var Probe = CSharpCompilation.Create(AssemblyName, Trees, AllReferences,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true));
        Trees = Trees.Select(Tree => ScriptPropertyRewriter.Rewrite(Probe, Tree, RewriteErrors)).ToArray();

        if (RewriteErrors.Count > 0)
        {
            foreach (string Error in RewriteErrors)
            {
                Native.Log(ELogLevel.Error, "C# script property: " + Error);
            }
            return null;
        }

        var Compilation = CSharpCompilation.Create(
            AssemblyName,
            Trees,
            AllReferences,
            new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: bOptimize ? OptimizationLevel.Release : OptimizationLevel.Debug,
                allowUnsafe: true,
                nullableContextOptions: NullableContextOptions.Enable));

        Compilation Compiled = Compilation;
        ImmutableArray<ISourceGenerator> Generators = SourceGenerators;
        if (Generators.Length > 0)
        {
            GeneratorDriver Driver = CSharpGeneratorDriver.Create(Generators);
            Driver = Driver.RunGeneratorsAndUpdateCompilation(Compilation, out Compilation Updated, out ImmutableArray<Diagnostic> GeneratorDiagnostics);
            Compiled = Updated;
            foreach (Diagnostic Diagnostic in GeneratorDiagnostics)
            {
                if (Diagnostic.Severity == DiagnosticSeverity.Error)
                {
                    Native.Log(ELogLevel.Error, "C# generator: " + Diagnostic);
                }
            }
        }

        using var PeStream = new MemoryStream();
        using var PdbStream = new MemoryStream();
        EmitResult Result = Compiled.Emit(PeStream, PdbStream,
            options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));

        bool bHadError = false;
        foreach (Diagnostic Diagnostic in Result.Diagnostics)
        {
            if (Diagnostic.Severity == DiagnosticSeverity.Error)
            {
                bHadError = true;
                Native.Log(ELogLevel.Error, "C# compile: " + Diagnostic);
                ExplainShadowedGameNamespace(Diagnostic);
            }
            else if (Diagnostic.Severity == DiagnosticSeverity.Warning)
            {
                Native.Log(ELogLevel.Warn, "C# compile: " + Diagnostic);
            }
        }

        if (Result.Success && !bHadError)
        {
            return new FScriptImage(PeStream.ToArray(), PdbStream.ToArray());
        }
        return null;
    }

    // CS0234 names the missing member, never the shadowing namespace that actually caused it.
    private static void ExplainShadowedGameNamespace(Diagnostic Diagnostic)
    {
        if (Diagnostic.Id != "CS0234" || !Diagnostic.GetMessage().Contains("namespace 'Game'"))
        {
            return;
        }

        Native.Log(ELogLevel.Error,
            "C# compile: 'Game' here is your own namespace, which hides LuminaSharp.Game. Rename the "
            + "namespace (the template uses GameScripts), or write global::LuminaSharp.Game.");
    }

    private static ImmutableArray<ISourceGenerator>? CachedGenerators;

    // Source generators run on every script compile (loaded once).
    //
    // An explicit allow-list, NOT "every IIncrementalGenerator in the assembly": ManagedExportGenerator is
    // engine-only and emits its attribute definition into whatever compilation it runs in, so letting it into
    // a script compile duplicates a type LuminaSharp already exports (CS0101).
    private static readonly string[] ScriptGeneratorNames =
    {
        "NativeCallGenerator",      // [NativeCall] -> the native binding glue
        // Emits nothing; ScriptPropertyRewriter produces the accessors. It runs here anyway for the one thing
        // the rewriter cannot report: a [Property] declared as a partial property is invisible to it, and
        // would surface as a bare CS9248 "must have an implementation part" with nothing naming the cause.
        "ScriptPropertyGenerator",
    };

    private static ImmutableArray<ISourceGenerator> SourceGenerators => CachedGenerators ??= LoadGenerators();

    private static ImmutableArray<ISourceGenerator> LoadGenerators()
    {
        try
        {
            string Directory = Path.GetDirectoryName(typeof(Host).Assembly.Location) ?? string.Empty;
            string GeneratorPath = Path.Combine(Directory, "LuminaSharp.Generators.dll");
            if (!File.Exists(GeneratorPath))
            {
                Native.Log(ELogLevel.Warn,
                    $"Source generator not found at '{GeneratorPath}'; routed [NativeCall] bindings won't compile in scripts.");
                return ImmutableArray<ISourceGenerator>.Empty;
            }

            // Load into LuminaSharp's OWN ALC (not Default) so the generator's Microsoft.CodeAnalysis binds to the same instance, else IIncrementalGenerator has a different type identity.
            AssemblyLoadContext Context = AssemblyLoadContext.GetLoadContext(typeof(Host).Assembly) ?? AssemblyLoadContext.Default;
            Assembly GeneratorAssembly = Context.LoadFromAssemblyPath(GeneratorPath);
            Type[] AllTypes;
            try
            {
                AllTypes = GeneratorAssembly.GetTypes();
            }
            catch (ReflectionTypeLoadException Exception)
            {
                Native.Log(ELogLevel.Error, $"Generator GetTypes failed: {Exception.LoaderExceptions.FirstOrDefault()?.Message}");
                AllTypes = Exception.Types.Where(T => T != null).ToArray()!;
            }

            var Found = new List<ISourceGenerator>();
            var Missing = new List<string>(ScriptGeneratorNames);
            foreach (Type Type in AllTypes)
            {
                if (Array.IndexOf(ScriptGeneratorNames, Type.Name) >= 0
                    && typeof(IIncrementalGenerator).IsAssignableFrom(Type) && !Type.IsAbstract
                    && Activator.CreateInstance(Type) is IIncrementalGenerator Generator)
                {
                    Found.Add(Generator.AsSourceGenerator());
                    Missing.Remove(Type.Name);
                }
            }

            // A generator that silently fails to load looks like "the language feature does not work": every
            // [Property] partial in every script becomes CS9248 with nothing pointing at the cause. Say so.
            if (Missing.Count > 0)
            {
                Native.Log(ELogLevel.Error,
                    $"Script source generator(s) not loaded from '{GeneratorPath}': {string.Join(", ", Missing)}. "
                    + "Scripts using the features they implement will fail to compile.");
            }
            return Found.ToImmutableArray();
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"Failed to load script source generators: {Exception.Message}");
            return ImmutableArray<ISourceGenerator>.Empty;
        }
    }
}
