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
using Microsoft.CodeAnalysis.CSharp.Syntax;
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

    // The generated IDE project adds the same imports as Using items, so IntelliSense agrees with this compile.
    public const string ScriptGlobalUsings = "global using static LuminaSharp.ObjectCore;\nglobal using static LuminaSharp.ESpecifier;";

    internal static CSharpParseOptions ParseOptions { get; } = new(LanguageVersion.Latest);

    // The trees every unit compiles beside its own sources.
    internal static IEnumerable<SyntaxTree> SupportTrees(string AssemblyName, IReadOnlyList<(string Path, string Text)> Sources)
    {
        // Routed value mirrors carry bool fields, and only with runtime marshalling off is their managed layout the sequential one C++ shares.
        if (!Sources.Any(Source => Source.Text.Contains("DisableRuntimeMarshalling", StringComparison.Ordinal)))
        {
            yield return CSharpSyntaxTree.ParseText(
                SourceText.From("[assembly: System.Runtime.CompilerServices.DisableRuntimeMarshalling]", Encoding.UTF8),
                ParseOptions, path: AssemblyName + ".Marshalling.g.cs");
        }

        // LoadObject<T>(Path) and [Property(Editable)] read as they do in C++, with no qualifier, in every script.
        yield return CSharpSyntaxTree.ParseText(
            SourceText.From(ScriptGlobalUsings, Encoding.UTF8), ParseOptions, path: AssemblyName + ".GlobalUsings.g.cs");
    }

    internal static CSharpCompilationOptions CompilationOptions(bool bOptimizeOutput)
    {
        return new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: bOptimizeOutput ? OptimizationLevel.Release : OptimizationLevel.Debug,
                allowUnsafe: true,
                nullableContextOptions: NullableContextOptions.Enable)
            // The NativeCall generator gives every script assembly its own copy of the attribute on purpose.
            .WithSpecificDiagnosticOptions(new Dictionary<string, ReportDiagnostic> { ["CS0436"] = ReportDiagnostic.Suppress });
    }

    // The framework and engine references plus ExtraReferences. With documentation, the engine reference carries LuminaSharp.xml for hovers.
    internal static MetadataReference[] ReferencesWith(IReadOnlyList<MetadataReference>? ExtraReferences, bool bWithDocumentation)
    {
        IEnumerable<MetadataReference> Base = References;
        string SelfPath = typeof(Host).Assembly.Location;
        string DocumentationPath = Path.ChangeExtension(SelfPath, ".xml");
        if (bWithDocumentation && File.Exists(SelfPath) && File.Exists(DocumentationPath))
        {
            Base = Base
                .Where(Reference => Reference is not PortableExecutableReference Pe || !string.Equals(Pe.FilePath, SelfPath, StringComparison.OrdinalIgnoreCase))
                .Append(MetadataReference.CreateFromFile(SelfPath, documentation: new FileDocumentationProvider(DocumentationPath)));
        }
        if (ExtraReferences == null || ExtraReferences.Count == 0)
        {
            return Base.ToArray();
        }

        // A package shipping its own copy of a framework assembly has to win, or Roslyn sees two of one identity.
        var Shadowed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (MetadataReference Reference in ExtraReferences)
        {
            if (SimpleName(Reference) is { } Name)
            {
                Shadowed.Add(Name);
            }
        }
        return Base
            .Where(Reference => SimpleName(Reference) is not { } Name || !Shadowed.Contains(Name))
            .Concat(ExtraReferences)
            .ToArray();
    }

    /// Compiles all sources into a PE plus its PDB, or null on error (diagnostics logged). ExtraReferences are the dependency units' emitted images.
    public static FScriptImage? Compile(string AssemblyName, IReadOnlyList<(string Path, string Text)> Sources,
        IReadOnlyList<MetadataReference>? ExtraReferences = null)
    {
        // Roslyn refuses to emit debug info for source text with no encoding, because it cannot checksum it (CS8055).
        SyntaxTree[] Trees = Sources
            .Select(Source => CSharpSyntaxTree.ParseText(SourceText.From(Source.Text, Encoding.UTF8), ParseOptions, path: Source.Path))
            .Concat(SupportTrees(AssemblyName, Sources))
            .ToArray();

        MetadataReference[] AllReferences = ReferencesWith(ExtraReferences, false);

        // Each stored field becomes its native-backed property here, the only compilation ever loaded, the probe compilation just giving the rewriter a semantic model.
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

        var Compilation = CSharpCompilation.Create(AssemblyName, Trees, AllReferences, CompilationOptions(bOptimize));

        Compilation Compiled = Compilation;
        bool bHadError = false;
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
                    bHadError = true;
                    Native.Log(ELogLevel.Error, "C# generator: " + Diagnostic);
                }
            }
        }

        using var PeStream = new MemoryStream();
        using var PdbStream = new MemoryStream();
        EmitResult Result = Compiled.Emit(PeStream, PdbStream,
            options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));

        foreach (Diagnostic Diagnostic in Result.Diagnostics)
        {
            if (Diagnostic.Severity == DiagnosticSeverity.Error)
            {
                bHadError = true;
                Native.Log(ELogLevel.Error, "C# compile: " + Diagnostic);
                ExplainShadowedGameNamespace(Diagnostic);
            }
            else if (Diagnostic.Severity == DiagnosticSeverity.Warning && !IsAssignedByEngine(Diagnostic))
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

    // An [Element] field is filled when its UI document loads, so a never-assigned warning is wrong about it.
    internal static bool IsAssignedByEngine(Diagnostic Diagnostic)
    {
        if (Diagnostic.Id != "CS0649" || Diagnostic.Location.SourceTree is not { } Tree)
        {
            return false;
        }
        SyntaxNode Node = Tree.GetRoot().FindNode(Diagnostic.Location.SourceSpan);
        FieldDeclarationSyntax? Field = Node.FirstAncestorOrSelf<FieldDeclarationSyntax>();
        return Field != null && Field.AttributeLists.SelectMany(List => List.Attributes)
            .Any(Attribute => Attribute.Name.ToString() is "Element" or "ElementAttribute" or "LuminaSharp.Element");
    }

    // CS0234 names the missing member, never the shadowing namespace that actually caused it.
    private static void ExplainShadowedGameNamespace(Diagnostic Diagnostic)
    {
        string Message = Diagnostic.GetMessage();
        string? Shadowed = Message.Contains("namespace 'Engine'") ? "Engine" : Message.Contains("namespace 'Game'") ? "Game" : null;
        if (Diagnostic.Id != "CS0234" || Shadowed == null)
        {
            return;
        }

        Native.Log(ELogLevel.Error,
            $"C# compile: '{Shadowed}' here is your own namespace, which hides LuminaSharp.{Shadowed}. Rename the "
            + $"namespace (the template uses GameScripts), or write global::LuminaSharp.{Shadowed}.");
    }

    private static ImmutableArray<ISourceGenerator>? CachedGenerators;

    // An explicit list, since ManagedExportGenerator is engine-only and would duplicate a type LuminaSharp exports.
    private static readonly string[] ScriptGeneratorNames =
    {
        "NativeCallGenerator",      // [NativeCall] -> the native binding glue
        // Emits nothing but reports a [Property] declared as a partial property, which would otherwise surface as a bare CS9248.
        "ScriptPropertyGenerator",
        "ScriptFunctionInvokerGenerator",   // [Function] -> the typed frame invoker
        "ContainerIndexerWriteGenerator",   // rejects a write through a TVector indexer that reads a held copy
        "ScriptStructGenerator",            // gives a [Reflect] script struct the handle it views native memory through
        "ComponentStorageGenerator",        // rejects a component held or created as a C# value, since it only exists in storage
    };

    internal static ImmutableArray<ISourceGenerator> SourceGenerators => CachedGenerators ??= LoadGenerators();

    internal static string GeneratorPath =>
        Path.Combine(Path.GetDirectoryName(typeof(Host).Assembly.Location) ?? string.Empty, "LuminaSharp.Generators.dll");

    private static ImmutableArray<ISourceGenerator> LoadGenerators()
    {
        try
        {
            string GeneratorPath = ScriptCompiler.GeneratorPath;
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

            // A generator that fails to load looks like a missing language feature, so it is named here.
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
