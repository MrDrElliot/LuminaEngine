using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using Microsoft.CodeAnalysis;

namespace LuminaSharp;

/// <summary>
/// One compilation unit of a generation: a plugin, the game, or the engine library. Compiles (or loads a
/// prebuilt DLL) into one assembly inside the shared collectible ALC. <see cref="Dependencies"/> are the
/// names of the sibling units this one references, used to order compilation and wire metadata refs.
/// </summary>
internal sealed class ScriptAssemblyUnit
{
    public required string Name;
    public required IReadOnlyList<string> Dependencies;
    public required IReadOnlyList<(string Path, string Text)> Sources;

    // Absolute paths to third-party assemblies this unit references (restored packages and declared DLLs).
    public IReadOnlyList<string> References = Array.Empty<string>();

    /// <summary>A prebuilt managed assembly to load as-is (when there are no <see cref="Sources"/>); null/empty
    /// for a compile-from-source unit.</summary>
    public string? DllPath;
}

/// <summary>
/// Owns one loaded generation of user C# scripts: compiles the sources into a collectible
/// AssemblyLoadContext, builds the <see cref="TypeLibrary"/> from the loaded types, and exposes the
/// <see cref="EntityScriptRuntime"/> the native entry points dispatch into. Reload compiles a fresh
/// generation and, only if that succeeds, frees the previous generation's live handles and unloads its
/// ALC (no engine restart). There is no archetype/dictionary indirection, the world's native ECS
/// system drives every per-instance lifecycle call.
/// </summary>
internal sealed class ScriptManager
{
    private ScriptLoadContext? Context;

    /// <summary>The runtime for the current generation, or null when no scripts are loaded.</summary>
    public EntityScriptRuntime? EntityScripts { get; private set; }

    /// <summary>The EntitySystem runtime for the current generation, or null when no scripts are loaded.</summary>
    public EntitySystemRuntime? EntitySystems { get; private set; }

    public RenderSceneRuntime? RenderScenes { get; private set; }

    /// <summary>Hosts C# subclasses of REFLECT(Scriptable) native CObjects for the current generation.</summary>
    public ScriptableRuntime? Scriptables { get; private set; }

    /// <summary>Types marked as engine data shapes ([DataTableRow], ...).</summary>
    public ScriptDataStructRuntime? DataStructs { get; private set; }

    /// <summary>Bumps on every successful (re)load; the native side rebinds entity scripts when it changes.</summary>
    public int Generation { get; private set; }

    /// <summary>Total managed types in the current generation's assembly (for editor diagnostics).</summary>
    public int LoadedTypeCount { get; private set; }

    public bool LoadOrReload(IReadOnlyList<ScriptAssemblyUnit> Units)
    {
        // Order units so every dependency is compiled before its dependents (its emitted image becomes a
        // metadata reference for them). Cycles are broken + logged rather than fatal.
        List<ScriptAssemblyUnit> Ordered = TopologicalOrder(Units);

        // Build every unit's PE image FIRST; never tear down working scripts for a broken edit. Compilation
        // and DLL reads are independent of any ALC, so a failure here leaves the live generation untouched.
        var Images = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        var Pending = new List<(ScriptAssemblyUnit Unit, FScriptImage Image)>();
        foreach (ScriptAssemblyUnit Unit in Ordered)
        {
            FScriptImage Image;
            if (Unit.Sources.Count > 0)
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

                // Roslyn is the single largest cost in engine startup, and nothing has usually changed.
                string? CompileKey = ComputeCompileKey(Unit, Images);
                if (TryLoadCachedAssembly(Unit.DllPath, CompileKey, out FScriptImage CachedImage))
                {
                    Image = CachedImage;
                }
                else
                {
                    FScriptImage? Compiled = ScriptCompiler.Compile(Unit.Name, Unit.Sources, Refs);
                    if (Compiled == null)
                    {
                        Native.Log(ELogLevel.Error,
                            $"Script reload aborted: compilation of '{Unit.Name}' failed; keeping current scripts.");
                        return false;
                    }
                    Image = Compiled.Value;

                    // A failed write leaves the previous DLL in place, so the key only follows a real one.
                    bool bEmitted = EmitAssembly(Unit.Name, Unit.DllPath, Image);
                    WriteCompileKey(Unit.DllPath, bEmitted ? CompileKey : null);
                }
            }
            else if (!string.IsNullOrEmpty(Unit.DllPath) && File.Exists(Unit.DllPath))
            {
                // No sources: load the unit's prebuilt managed DLL as-is (a code-only plugin).
                try
                {
                    Image = new FScriptImage(File.ReadAllBytes(Unit.DllPath!), ReadSymbols(Unit.DllPath!));
                }
                catch (Exception Exception)
                {
                    Native.Log(ELogLevel.Error,
                        $"Script reload aborted: failed to read prebuilt assembly '{Unit.DllPath}': {Exception.Message}");
                    return false;
                }
            }
            else
            {
                continue; // empty unit (no sources, no prebuilt DLL)
            }

            Images[Unit.Name] = Image.Pe;
            Pending.Add((Unit, Image));
        }

        UnloadCurrent();

        // Even an EMPTY generation must advance the counter: live native bridges gate their rebind on it,
        // and UnloadCurrent just freed their GCHandles. Without a bump they would keep dispatching into
        // the unloaded generation instead of rebinding to nothing and falling back to native behavior.
        Generation++;

        if (Pending.Count == 0)
        {
            Native.Log(ELogLevel.Info, "No C# scripts found.");
            return true;
        }

        var NewContext = new ScriptLoadContext($"GameScripts.Gen{Generation}");

        // Registered before any unit loads, so a script's first bind to a package assembly resolves in this context.
        foreach ((ScriptAssemblyUnit Unit, FScriptImage _) in Pending)
        {
            foreach (string Reference in Unit.References)
            {
                NewContext.RegisterReference(Reference);
            }
        }

        // Load in dependency order: each unit is registered before any dependent loads, so a dependent's
        // sibling reference resolves to the in-ALC assembly (see ScriptLoadContext.Load). LoadFromStream
        // (not a path) keeps the file unlocked so the collectible context unloads cleanly on reload.
        var AllTypes = new List<Type>();
        foreach ((ScriptAssemblyUnit Unit, FScriptImage Image) in Pending)
        {
            Assembly Loaded = NewContext.LoadScriptAssembly(Unit.Name, Image);
            // Run module initializers now (deterministically) so a plugin's [ModuleInitializer] export
            // registration into ManagedExportRegistry happens at load, not lazily on first type use.
            RuntimeHelpers.RunModuleConstructor(Loaded.ManifestModule.ModuleHandle);
            AllTypes.AddRange(SafeGetTypes(Loaded, Unit.Name));
        }
        Context = NewContext;

        LoadedTypeCount = AllTypes.Count;
        var Library = new TypeLibrary(AllTypes);
        EntityScripts = new EntityScriptRuntime(Library);
        EntitySystems = new EntitySystemRuntime(Library);
        RenderScenes = new RenderSceneRuntime(Library);
        Scriptables = new ScriptableRuntime(Library, EntityScripts);
        DataStructs = new ScriptDataStructRuntime(Library);

        Native.Log(ELogLevel.Info,
            $"Loaded C# scripts [generation {Generation}]: {Pending.Count} assembl(ies), {AllTypes.Count} type(s), " +
            $"{Library.EntityScriptTypeNames.Count} EntityScript(s), {Library.EntitySystemTypes.Count} EntitySystem(s).");
        return true;
    }

    // Post-order DFS over the unit dependency graph: a unit appears after every dependency it names that is
    // also present in this set (unknown names, a dependency that ships no scripts, are ignored). A cycle is
    // broken at the back-edge and logged; the generation still loads (degraded refs) rather than failing.
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

    // Writes a unit's compiled image to its on-disk DLL (creating <root>/Binaries/DotNet as needed). Purely an
    // artifact: the generation always loads from the in-memory bytes, so a failure here (locked file, read-only
    // path) is logged and ignored rather than aborting the reload.
    /// <summary>False when the artifact did not reach disk, which must not be stamped as cacheable.</summary>
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

    // A prebuilt unit keeps its symbols only if whoever built it shipped them, so this stays optional.
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

    // Global per-frame pump, empty because SEntityScriptSystem drives per-world ticking natively.
    public void Tick()
    {
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

        // Drop all message-bus subscriptions first: their listener delegates capture script instances, and the
        // process-static BusRegistry would otherwise pin the collectible ALC. The next generation re-subscribes.
        BusRegistry.ClearAll();

        // Same rationale for the other process-static holders of script-side state: each roots user types, or
        // GCHandles over user delegates, that would otherwise pin the collectible generation across the unload.
        // The next generation rebuilds them lazily / re-subscribes.
        Native.ClearAllManagedTimers();       // world timers whose Action captures a script instance
        Native.ClearAllManagedTweens();       // tween callbacks whose Action captures a script instance
        UIDataModel.DisposeAll();             // MVVM bindings (user ViewModel + native data model)
        Asset.PurgePending();                 // in-flight async asset-load callbacks
        PropertyAccessor.ClearScriptCaches(); // cached get/set delegates over user property types

        // Every strong handle has to go before the unload, or it roots the generation the ALC is dropping.
        EntityScripts?.FreeAll();  // detaches only; the table drain below owns the script handles
        EntityScripts = null;
        EntitySystems?.FreeAll();
        EntitySystems = null;
        RenderScenes?.FreeAll();  // native destroys these pre-reload; this is the process-shutdown backstop
        RenderScenes = null;
        // C# Scriptable subclass instances live in their native object's managed-instance slot, and those
        // handles are STRONG -- they would pin this ALC. Draining the whole table here keeps that release at
        // exactly the point in the teardown contract it has always been at, just on the side that owns it now.
        // Plain wrapper entries in the same table are weak and would not have pinned anything; they are
        // dropped too and re-created lazily against the next generation.
        Native.ReleaseAllManagedInstances();
        Scriptables = null;

        // Holds no handles of its own, but it holds the TypeLibrary, which holds user Types. Cleared here
        // so the teardown table stays complete rather than depending on this runtime being harmless.
        DataStructs = null;

        // Drop this generation's script-tier managed exports: their function pointers reference code in the
        // ALC about to unload and would dangle. The next generation's module initializers repopulate them.
        ManagedExportRegistry.ClearScriptExports();

        // Confine the only strong reference to the ALC inside a method that fully returns before we
        // collect: a collectible ALC won't unload while any caller frame (even a JIT-spilled temp under
        // tier-0) still holds it. The GC loop then runs with no root.
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
            // A debugger pins every loaded assembly for its lifetime, so a collectible ALC will not unload
            // while one is attached. This is expected when running under VS/F5 and resolves in a normal run.
            Native.Log(ELogLevel.Info,
                "Previous script ALC did NOT unload -- expected with a debugger attached (it pins loaded " +
                "assemblies). It should unload cleanly in a normal (non-debugger) run.");
        }
        else
        {
            // A collectible ALC unloads asynchronously: after the synchronous GC loop the WeakReference can
            // still be briefly alive while the unload settles on the finalizer thread, so a single residual is
            // normal and NOT proof of a leak. The real signal is the trend: open the editor's C# Diagnostics
            // tool and watch "Resident generations" -- it should fall back to 1; a value that climbs across
            // reloads is a genuine leak (then capture a gcdump and inspect GC roots of GameScripts.Gen*).
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
