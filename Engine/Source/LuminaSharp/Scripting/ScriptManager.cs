using System;
using Lumina;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace LuminaSharp;

// One assembly of a generation, a plugin's, the game's or the engine's, compiled from sources or loaded prebuilt.
internal sealed class ScriptAssemblyUnit
{
    public required string Name;
    public required IReadOnlyList<string> Dependencies;
    public required IReadOnlyList<(string Path, string Text)> Sources;

    // Absolute paths to third-party assemblies this unit references (restored packages and declared DLLs).
    public IReadOnlyList<string> References = Array.Empty<string>();

    // A prebuilt assembly to load as is when the unit has no sources.
    public string? DllPath;
}

// A generation built and loaded off the game thread, waiting for the frame start that swaps it in.
internal sealed class FCompiledGeneration
{
    public required List<(ScriptAssemblyUnit Unit, FScriptImage Image)> Pending;
    public Dictionary<string, string> ScriptSourcePaths = new(StringComparer.Ordinal);

    // Null for a generation with no units. Nothing runs in it until Commit, so it can be dropped unused.
    public ScriptLoadContext? Context;

    public List<(string UnitName, Assembly Loaded)> Assemblies = new();

    public void Abandon()
    {
        Context?.Unload();
        Context = null;
        Assemblies.Clear();
    }
}

// Owns the loaded script generation, compiling a new one into a fresh collectible context and swapping it in only once it succeeds.
internal sealed class ScriptManager
{
    private ScriptLoadContext? Context;

    /// <summary>The runtime for the current generation, or null when no scripts are loaded.</summary>
    public EntityScriptRuntime? EntityScripts { get; private set; }

    // Hosts the C# subclasses of reflected native classes for the current generation.
    public ScriptableRuntime? Scriptables { get; private set; }

    // The reflected view of the loaded generation, which the type manifest is written from.
    public TypeLibrary? Library { get; private set; }

    /// <summary>Bumps on every successful (re)load; the native side rebinds entity scripts when it changes.</summary>
    public int Generation { get; private set; }

    /// <summary>Total managed types in the current generation's assembly (for editor diagnostics).</summary>
    public int LoadedTypeCount { get; private set; }

    // Answers the editor's code completion, hover and diagnostics for scripts, from the units the last load was given.
    public ScriptLanguageService Language { get; } = new();

    // Roslyn's cached references and the on-disk compile keys are shared, so two builds must never overlap.
    private static readonly object BuildLock = new();

    // Names each context as it is built, which can be before the generation it becomes is known.
    private static int ContextSerial;

    // With nothing running there are no current scripts to keep, so a broken unit loads its last good build instead.
    public bool bUseLastBuildOnError => Library == null;

    public bool LoadOrReload(IReadOnlyList<ScriptAssemblyUnit> Units)
    {
        FCompiledGeneration? Compiled = BuildGeneration(Units, bUseLastBuildOnError);
        return Compiled != null && Commit(Compiled);
    }

    // Touches nothing the live generation uses, so it runs on any thread while the current scripts keep running.
    public static FCompiledGeneration? BuildGeneration(IReadOnlyList<ScriptAssemblyUnit> Units, bool bUseLastBuildOnError)
    {
        lock (BuildLock)
        {
            return BuildGenerationLocked(Units, bUseLastBuildOnError);
        }
    }

    private static FCompiledGeneration? BuildGenerationLocked(IReadOnlyList<ScriptAssemblyUnit> Units, bool bUseLastBuildOnError)
    {
        // Every dependency compiles before its dependents, since its image is their metadata reference.
        List<ScriptAssemblyUnit> Ordered = TopologicalOrder(Units);

        // Every image is built before the live generation is touched, so a broken edit leaves working scripts running.
        var Images = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        var Pending = new List<(ScriptAssemblyUnit Unit, FScriptImage Image)>();
        foreach (ScriptAssemblyUnit Unit in Ordered)
        {
            FScriptImage Image;
            if (Unit.Sources.Count > 0)
            {
                // Roslyn is the single largest cost in engine startup, and nothing has usually changed.
                string? CompileKey = ComputeCompileKey(Unit, Images);
                if (TryLoadCachedAssembly(Unit.DllPath, CompileKey, out FScriptImage CachedImage))
                {
                    Image = CachedImage;
                }
                else
                {
                    FScriptImage? Compiled = CompileUnit(Unit, Images);
                    if (Compiled != null)
                    {
                        Image = Compiled.Value;

                        // A failed write leaves the previous DLL in place, so the key only follows a real one.
                        bool bEmitted = EmitAssembly(Unit.Name, Unit.DllPath, Image);
                        WriteCompileKey(Unit.DllPath, bEmitted ? CompileKey : null);
                    }
                    else if (bUseLastBuildOnError && TryReadImage(Unit.DllPath, out Image))
                    {
                        // Only a successful compile writes the DLL, so what is on disk is the last build that worked.
                        Native.Log(ELogLevel.Error,
                            $"Compilation of '{Unit.Name}' failed; its last successful build is loaded until the errors are fixed.");
                    }
                    else
                    {
                        Native.Log(ELogLevel.Error,
                            $"Script reload aborted: compilation of '{Unit.Name}' failed; keeping current scripts.");
                        return null;
                    }
                }
            }
            else if (!string.IsNullOrEmpty(Unit.DllPath) && File.Exists(Unit.DllPath))
            {
                // No sources: load the unit's prebuilt managed DLL as-is (a code-only plugin).
                if (!TryReadImage(Unit.DllPath, out Image))
                {
                    Native.Log(ELogLevel.Error, $"Script reload aborted: failed to read prebuilt assembly '{Unit.DllPath}'.");
                    return null;
                }
            }
            else
            {
                continue; // empty unit (no sources, no prebuilt DLL)
            }

            Images[Unit.Name] = Image.Pe;
            Pending.Add((Unit, Image));
        }

        var Built = new FCompiledGeneration { Pending = Pending };
        Built.ScriptSourcePaths = GatherScriptSourcePaths(Pending);
        if (Pending.Count > 0)
        {
            LoadIntoNewContext(Built);
        }
        return Built;
    }

    // Loading a freshly compiled image costs tens of milliseconds, and a context nobody uses yet can take it off the game thread.
    private static void LoadIntoNewContext(FCompiledGeneration Built)
    {
        var NewContext = new ScriptLoadContext($"GameScripts.Gen{Interlocked.Increment(ref ContextSerial)}");

        // Registered before any unit loads, so a script's first bind to a package assembly resolves in this context.
        foreach ((ScriptAssemblyUnit Unit, FScriptImage _) in Built.Pending)
        {
            foreach (string Reference in Unit.References)
            {
                NewContext.RegisterReference(Reference);
            }
        }

        // Loaded in dependency order and from memory, so references resolve inside the context and no file stays locked.
        foreach ((ScriptAssemblyUnit Unit, FScriptImage Image) in Built.Pending)
        {
            Built.Assemblies.Add((Unit.Name, NewContext.LoadScriptAssembly(Unit.Name, Image)));
        }
        Built.Context = NewContext;
    }

    // Swaps the built images in, so it runs on the game thread between frames.
    public bool Commit(FCompiledGeneration Compiled)
    {
        List<(ScriptAssemblyUnit Unit, FScriptImage Image)> Pending = Compiled.Pending;

        // Refused before the swap, so bindings built against another engine build never replace a working generation.
        var LayoutFailures = new System.Text.StringBuilder();
        foreach ((string _, Assembly Loaded) in Compiled.Assemblies)
        {
            LayoutValidator.ValidateModules(Loaded, LayoutFailures);
        }
        if (LayoutFailures.Length > 0)
        {
            Native.Log(ELogLevel.Error, $"C# scripts were not loaded, their bindings do not match the engine:{LayoutFailures}");
            Compiled.Context?.Unload();
            return false;
        }

        UnloadCurrent();

        // Bumped even for an empty generation, since native rebinds on it after the unload freed every handle.
        Generation++;

        if (Pending.Count == 0 || Compiled.Context == null)
        {
            Native.Log(ELogLevel.Info, "No C# scripts found.");
            return true;
        }

        // Module initializers register exports that UnloadCurrent clears, so they wait for the swap rather than the load.
        var AllTypes = new List<Type>();
        foreach ((string UnitName, Assembly Loaded) in Compiled.Assemblies)
        {
            RuntimeHelpers.RunModuleConstructor(Loaded.ManifestModule.ModuleHandle);
            AllTypes.AddRange(SafeGetTypes(Loaded, UnitName));
        }
        Context = Compiled.Context;

        LoadedTypeCount = AllTypes.Count;
        var Library = new TypeLibrary(AllTypes);
        EntityScripts = new EntityScriptRuntime(Library);
        Scriptables = new ScriptableRuntime(Library, EntityScripts, Compiled.ScriptSourcePaths);
        this.Library = Library;

        Native.Log(ELogLevel.Info,
            $"Loaded C# scripts [generation {Generation}]: {Pending.Count} assembl(ies), {AllTypes.Count} type(s), " +
            $"{Library.ScriptableTypeNames.Count} script class(es).");
        return true;
    }

    // Kept out of LoadOrReload, whose JIT would otherwise load Roslyn in a packaged game that ships only prebuilt scripts.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static FScriptImage? CompileUnit(ScriptAssemblyUnit Unit, Dictionary<string, byte[]> Images)
    {
        var Refs = new List<MetadataReference>();
        foreach (string Dep in Unit.Dependencies)
        {
            if (Images.TryGetValue(Dep, out byte[]? DepImage))
            {
                Refs.Add(MetadataReference.CreateFromImage(DepImage));
            }
        }

        foreach (string Reference in Unit.References)
        {
            try
            {
                Refs.Add(MetadataReference.CreateFromFile(Reference));
            }
            catch (Exception Exception)
            {
                Native.Log(ELogLevel.Error,
                    $"Script reference '{Reference}' could not be read for '{Unit.Name}': {Exception.Message}");
            }
        }

        return ScriptCompiler.Compile(Unit.Name, Unit.Sources, Refs);
    }

    // Dependencies first, with a cycle broken at its back edge and logged so the generation still loads.
    private static List<ScriptAssemblyUnit> TopologicalOrder(IReadOnlyList<ScriptAssemblyUnit> Units)
    {
        var ByName = new Dictionary<string, ScriptAssemblyUnit>(StringComparer.OrdinalIgnoreCase);
        foreach (ScriptAssemblyUnit Unit in Units)
        {
            ByName[Unit.Name] = Unit;
        }

        var Ordered = new List<ScriptAssemblyUnit>(Units.Count);
        var State = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase); // 0 = visiting, 1 = done

        void Visit(ScriptAssemblyUnit Unit)
        {
            if (State.TryGetValue(Unit.Name, out int Status))
            {
                if (Status == 0)
                {
                    Native.Log(ELogLevel.Warn, $"Script unit dependency cycle involving '{Unit.Name}'; breaking it.");
                }
                return;
            }

            State[Unit.Name] = 0;
            foreach (string Dep in Unit.Dependencies)
            {
                if (ByName.TryGetValue(Dep, out ScriptAssemblyUnit? DepUnit))
                {
                    Visit(DepUnit);
                }
            }
            State[Unit.Name] = 1;
            Ordered.Add(Unit);
        }

        foreach (ScriptAssemblyUnit Unit in Units)
        {
            Visit(Unit);
        }
        return Ordered;
    }

    // Writes the image to its DLL for the next boot, false when it did not reach disk and so must not be cached.
    private static bool EmitAssembly(string UnitName, string? Path, FScriptImage Image)
    {
        if (string.IsNullOrEmpty(Path))
        {
            return false;
        }

        try
        {
            string? Directory = System.IO.Path.GetDirectoryName(Path);
            if (!string.IsNullOrEmpty(Directory))
            {
                System.IO.Directory.CreateDirectory(Directory);
            }
            File.WriteAllBytes(Path!, Image.Pe);
            if (Image.Pdb != null)
            {
                File.WriteAllBytes(SymbolPath(Path!), Image.Pdb);
            }
            return true;
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Warn, $"Could not write '{UnitName}' assembly to '{Path}': {Exception.Message}");
            return false;
        }
    }

    private static string SymbolPath(string DllPath)
    {
        return System.IO.Path.ChangeExtension(DllPath, ".pdb");
    }

    // Bumped whenever the emitted image could differ for identical sources.
    private const int CompileCacheVersion = 1;

    private static string CompileKeyPath(string DllPath) => DllPath + ".compilekey";

    /// <summary>Identity of everything the emitted image depends on, or null when it cannot be pinned down.</summary>
    private static string? ComputeCompileKey(ScriptAssemblyUnit Unit, Dictionary<string, byte[]> DependencyImages)
    {
        if (string.IsNullOrEmpty(Unit.DllPath))
        {
            return null;
        }

        try
        {
            using var Digest = System.Security.Cryptography.IncrementalHash.CreateHash(
                System.Security.Cryptography.HashAlgorithmName.SHA256);

            void Feed(string Value) => Digest.AppendData(System.Text.Encoding.UTF8.GetBytes(Value + "\u0000"));

            Feed($"v{CompileCacheVersion}");
            Feed(ScriptCompiler.bOptimize ? "release" : "debug");
            Feed(typeof(ScriptManager).Assembly.FullName ?? "LuminaSharp");
            // The assembly version never moves, so a script built against an older LuminaSharp would load and then miss members.
            Feed(typeof(ScriptManager).Assembly.ManifestModule.ModuleVersionId.ToString());
            var Generators = new FileInfo(ScriptCompiler.GeneratorPath);
            Feed(Generators.Exists ? $"{Generators.Length}:{Generators.LastWriteTimeUtc.Ticks}" : "no generators");
            Feed(Unit.Name);

            foreach ((string Path, string Text) in Unit.Sources)
            {
                Feed(Path);
                Feed(Text);
            }

            // The dependency's own bytes, so a changed dependency invalidates every dependent too.
            foreach (string Dep in Unit.Dependencies)
            {
                Feed(Dep);
                if (DependencyImages.TryGetValue(Dep, out byte[]? DepImage))
                {
                    Digest.AppendData(DepImage);
                }
            }

            // Stamps rather than contents, since a reference can be a large third-party assembly.
            foreach (string Reference in Unit.References)
            {
                Feed(Reference);
                var Info = new FileInfo(Reference);
                Feed(Info.Exists ? $"{Info.Length}:{Info.LastWriteTimeUtc.Ticks}" : "missing");
            }

            return Convert.ToHexString(Digest.GetHashAndReset());
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static bool TryLoadCachedAssembly(string? DllPath, string? CompileKey, out FScriptImage Image)
    {
        Image = default;
        if (string.IsNullOrEmpty(DllPath) || CompileKey == null || !File.Exists(DllPath))
        {
            return false;
        }

        try
        {
            string KeyPath = CompileKeyPath(DllPath!);
            if (!File.Exists(KeyPath) || File.ReadAllText(KeyPath).Trim() != CompileKey)
            {
                return false;
            }
        }
        catch (Exception)
        {
            return false;
        }

        return TryReadImage(DllPath, out Image);
    }

    private static bool TryReadImage(string? DllPath, out FScriptImage Image)
    {
        Image = default;
        if (string.IsNullOrEmpty(DllPath) || !File.Exists(DllPath))
        {
            return false;
        }

        try
        {
            Image = new FScriptImage(File.ReadAllBytes(DllPath!), ReadSymbols(DllPath!));
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>A null key clears any stale stamp, so the next load recompiles rather than trusting an old DLL.</summary>
    private static void WriteCompileKey(string? DllPath, string? CompileKey)
    {
        if (string.IsNullOrEmpty(DllPath))
        {
            return;
        }

        try
        {
            string KeyPath = CompileKeyPath(DllPath!);
            if (CompileKey == null)
            {
                File.Delete(KeyPath);
                return;
            }

            File.WriteAllText(KeyPath, CompileKey);
        }
        catch (Exception)
        {
            // A cache that cannot be written just means the next load compiles again.
        }
    }

    private static byte[]? ReadSymbols(string DllPath)
    {
        try
        {
            string Path = SymbolPath(DllPath);
            return File.Exists(Path) ? File.ReadAllBytes(Path) : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static IEnumerable<Type> SafeGetTypes(Assembly Assembly, string UnitName)
    {
        try
        {
            return Assembly.GetTypes();
        }
        catch (ReflectionTypeLoadException Exception)
        {
            Native.Log(ELogLevel.Warn, $"Some types in script assembly '{UnitName}' failed to load: {Exception.Message}");
            return Exception.Types.Where(Type => Type != null)!;
        }
    }

    private static Dictionary<string, string> GatherScriptSourcePaths(
        List<(ScriptAssemblyUnit Unit, FScriptImage Image)> Pending)
    {
        var Paths = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach ((ScriptAssemblyUnit Unit, FScriptImage _) in Pending)
        {
            foreach ((string Path, string Text) in Unit.Sources)
            {
                SyntaxNode Root = CSharpSyntaxTree.ParseText(Text).GetRoot();
                foreach (ClassDeclarationSyntax Class in Root.DescendantNodes().OfType<ClassDeclarationSyntax>())
                {
                    var Namespaces = new Stack<string>();
                    var Types = new Stack<string>();
                    Types.Push(Class.Identifier.ValueText);
                    for (SyntaxNode? Parent = Class.Parent; Parent != null; Parent = Parent.Parent)
                    {
                        if (Parent is BaseNamespaceDeclarationSyntax Namespace)
                        {
                            Namespaces.Push(Namespace.Name.ToString());
                        }
                        else if (Parent is TypeDeclarationSyntax Enclosing)
                        {
                            Types.Push(Enclosing.Identifier.ValueText);
                        }
                    }

                    string NamespaceName = string.Join(".", Namespaces);
                    string FullName = (NamespaceName.Length == 0 ? "" : NamespaceName + ".") + string.Join("+", Types);
                    Paths.TryAdd(FullName, Path);
                }
            }
        }
        return Paths;
    }

    // Global per-frame pump, empty because SEntityScriptSystem drives per-world ticking natively.
    public void Tick()
    {
        GameThreadContext.Drain();

        // A fire-and-forget async function is never asked about again, so this retires the finished ones and reports what they threw.
        ScriptAsync.ReapCompleted();
    }

    public void Shutdown()
    {
        UnloadCurrent();
    }

    private void UnloadCurrent()
    {
        if (Context == null)
        {
            return;
        }

        // Each process-static holder of script state would otherwise pin the collectible generation across the unload.
        CTimerSubsystem.ClearAllManaged();      // world timers whose Action captures a script instance
        UIDataModel.DisposeAll();             // MVVM bindings (user ViewModel + native data model)
        ScriptAsync.Clear();                  // tokens for async functions still running in the old generation
        GameTaskRegistry.CancelAll();         // pending awaits, whose continuations close over unloading code
        Interop.ResetExceptionThrottle();     // the reloaded code earns a full stack on its first throw
        ScriptCallback.PurgeAll();            // every closure handed to native, callbacks, listeners and loads alike
        PropertyAccessor.ClearScriptCaches(); // cached get/set delegates over user property types
        ScriptFunctionDispatch.Reset();        // [Function] bindings, which root user MethodInfos
        ScriptInvokerRegistry.Clear();         // generated invokers, which are delegates over user methods
        NativeObjectMarshal.ClearTypeCache();  // wrapper instantiations keyed by user parameter types

        // Every strong handle has to go before the unload, or it roots the generation the ALC is dropping.
        EntityScripts?.FreeAll();  // detaches only; the table drain below owns the script handles
        EntityScripts = null;
        // A subclass instance holds a strong handle that would pin this context, so the whole instance table drains here.
        Native.ReleaseAllManagedInstances();
        Scriptables = null;

        // Cached argument slots are keyed by the handler's declared types, which are types from this ALC.
        ScriptDelegateArgs.PurgeAll();

        // The library holds user Types, which would pin the load context about to unload.
        Library = null;

        // This generation's managed exports point into the context about to unload, and the next one's initializers register them again.
        ManagedExportRegistry.ClearScriptExports();

        // The only strong reference stays inside a method that returns before collecting, or a spilled frame would keep the context alive.
        WeakReference Weak = UnloadContextLocked();

        for (int Index = 0; Weak.IsAlive && Index < 10; Index++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }

        if (!Weak.IsAlive)
        {
            Native.Log(ELogLevel.Info, "Previous script ALC unloaded cleanly.");
        }
        else if (System.Diagnostics.Debugger.IsAttached)
        {
            // A debugger pins every loaded assembly, so the context cannot unload while one is attached.
            Native.Log(ELogLevel.Info,
                "Previous script ALC did NOT unload -- expected with a debugger attached (it pins loaded " +
                "assemblies). It should unload cleanly in a normal (non-debugger) run.");
        }
        else
        {
            // An unload settles on the finalizer thread, so one survivor is normal and a count climbing across reloads in C# Diagnostics is the leak.
            Native.Log(ELogLevel.Info,
                "Previous script ALC not yet collected (asynchronous unload still settling). Watch the C# " +
                "Diagnostics tool's 'Resident generations' -- a count that keeps climbing across reloads is a leak.");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private WeakReference UnloadContextLocked()
    {
        var Weak = new WeakReference(Context);
        Context!.Unload();
        Context = null;
        return Weak;
    }
}
