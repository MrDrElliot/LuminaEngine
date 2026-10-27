using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace LuminaSharp;

/// Managed entry surface for FDotNetHost. Entry method names are a name-based ABI contract and must not be renamed. Per-instance entries take an IntPtr that IS a strong GCHandle to the managed CEntityScript.
public static unsafe partial class Host
{
    // Must equal Lumina::DotNet::GAbiVersion. Bump on ABI breaks.
    private const int AbiVersion = 17;

    // Logical name for the engine module hosting this assembly (Runtime); resolved to a native handle via ModuleHandle.
    public const string NativeLibrary = "LuminaNative";

    private static ScriptManager? Scripts;

    // The loaded generation's types, for code that turns a runtime type name back into a C# type.
    internal static TypeLibrary? Library => Scripts?.Library;

    // A reload compiling on a worker thread, which the game thread swaps in once it finishes.
    private static System.Threading.Tasks.Task<FCompiledGeneration?>? PendingCompile;
    private static IntPtr NativeModule;

    // Generated P/Invokes name an engine module, which is already loaded, so the name maps to its handle rather than a file search.
    static Host()
    {
        System.Runtime.InteropServices.NativeLibrary.SetDllImportResolver(typeof(Host).Assembly,
            (Name, Assembly, SearchPath) => ModuleHandle(Name));
    }
    private static readonly Dictionary<string, IntPtr> ModuleHandles = new();

    // Bootstrap-critical exports, resolved directly from the host (Runtime) module.
    private static delegate* unmanaged[Cdecl]<int, int, int> NativeSelfTestPtr;
    private static delegate* unmanaged[Cdecl]<byte*, int, IntPtr> ResolveModuleHandlePtr;

    // The ONE entry the native host resolves by name (hostfxr); not in the export table.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Bootstrap(FBootstrapArgs* Args)
    {
        try
        {
            if (Args == null || Args->Exports == null)
            {
                return 1;
            }
            if (Args->AbiVersion != AbiVersion)
            {
                return 2;
            }

            // NativeModule must be set before any binding resolves (Native's static init may run here).
            NativeModule = Args->NativeModule;

            NativeSelfTestPtr = (delegate* unmanaged[Cdecl]<int, int, int>)NativeBindings.ResolveFrom(NativeModule, "LuminaSharp_NativeSelfTest");
            // Touching Native below runs its one-shot binding resolves, and any non-default module needs this first.
            ResolveModuleHandlePtr = (delegate* unmanaged[Cdecl]<byte*, int, IntPtr>)NativeBindings.ResolveFrom(NativeModule, "LuminaSharp_ResolveModuleHandle");

            Native.SetExports(*Args->Exports);

            ManagedExportTable.RegisterEngineExports();

            int Sum = NativeSelfTestPtr != null ? NativeSelfTestPtr(2, 3) : -1;
            Native.Log(Sum == 5 ? ELogLevel.Info : ELogLevel.Error,
                Sum == 5 ? "C#->native function-pointer path OK." : $"C# interop self-test FAILED (got {Sum}).");

            // Cross-check every blittable C#/C++ mirror's size before any crosses the boundary; a mismatch corrupts memory.
            if (!LayoutValidator.ValidateAll())
            {
                return 4;
            }

            // Installed from the game thread, so every await in script code resumes back on it.
            GameThreadContext.Install();

            Scripts = new ScriptManager();
            Native.Log(ELogLevel.Info, $"LuminaSharp online (runtime {RuntimeInformation.FrameworkDescription}).");
            return 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 3;
        }
    }

    /// Resolves a native->managed export to its function pointer by name, or IntPtr.Zero if unknown.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr ResolveManagedExport(byte* Name, int Length)
    {
        try
        {
            return ManagedExportRegistry.Resolve(Interop.GetString(Name, Length));
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return IntPtr.Zero;
        }
    }

    /// Resolves (and caches) a native module's loaded handle by name; "LuminaNative" is the host (Runtime) module.
    public static IntPtr ModuleHandle(string Name)
    {
        if (Name == NativeLibrary)
        {
            return NativeModule;
        }

        lock (ModuleHandles)
        {
            if (ModuleHandles.TryGetValue(Name, out IntPtr Handle))
            {
                return Handle;
            }

            Handle = ResolveModule(Name);
            // Only cache a SUCCESSFUL resolve; a miss can be transient during early bootstrap, so retry next call.
            if (Handle != IntPtr.Zero)
            {
                ModuleHandles[Name] = Handle;
            }
            return Handle;
        }
    }

    private static IntPtr ResolveModule(string Name)
    {
        if (ResolveModuleHandlePtr == null)
        {
            return IntPtr.Zero;
        }

        Span<byte> Scratch = stackalloc byte[256];
        Interop.FInteropString Encoded = new(Name, Scratch);
        try
        {
            return ResolveModuleHandlePtr(Encoded.Pointer, Encoded.Length);
        }
        finally
        {
            Encoded.Free();
        }
    }

    // Called as a native object releases its managed handle, from every destruction path there is.
    internal static void ForgetScriptHandle(IntPtr Handle)
    {
        Scripts?.EntityScripts?.Forget(Handle);
    }

    // The generation outlives a world, so state keyed on one accumulates every PIE session without this.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void OnWorldTeardown(ulong World)
    {
        try
        {
            UIDataModel.RemoveForWorld(World);
            GameTaskRegistry.CancelWorld(World);
            DropWorldReference((IntPtr)World);
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"OnWorldTeardown threw: {Exception}");
        }
    }

    // Immediate, since the editor force-destroys a stopped PIE world, while its context still owns it here.
    private static void DropWorldReference(IntPtr World)
    {
        IntPtr Existing = Native.ObjectGetManagedInstance(World);
        if (Existing != IntPtr.Zero && GCHandle.FromIntPtr(Existing).Target is NativeObject Wrapper)
        {
            Wrapper.ReleaseAllReferences();
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int GetGeneration()
    {
        return Scripts?.Generation ?? 0;
    }

    /// Takes effect on the next LoadScripts. The packager sets it so shipped assemblies are optimized, and clears it so editor reloads stay debuggable.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void SetScriptCompileOptimization(int bOptimize)
    {
        ScriptCompiler.bOptimize = bOptimize != 0;
    }

    /// Fills the editor's C# Diagnostics snapshot (heap, GC, ALC health). Returns 1 on success. ForceCollect != 0 runs a blocking GC first.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int GetRuntimeDiagnostics(IntPtr OutPtr, int ForceCollect)
    {
        try
        {
            if (OutPtr == IntPtr.Zero)
            {
                return 0;
            }

            if (ForceCollect != 0)
            {
                for (int Pass = 0; Pass < 3; Pass++)
                {
                    GC.Collect();
                    GC.WaitForPendingFinalizers();
                }
            }

            ref FScriptDiagnostics Diag = ref *(FScriptDiagnostics*)OutPtr;
            Diag = default;

            GCMemoryInfo Info = GC.GetGCMemoryInfo();
            Diag.ManagedHeapBytes    = GC.GetTotalMemory(false);
            Diag.HeapSizeBytes       = Info.HeapSizeBytes;
            Diag.FragmentedBytes     = Info.FragmentedBytes;
            Diag.CommittedBytes      = Info.TotalCommittedBytes;
            Diag.TotalAllocatedBytes = GC.GetTotalAllocatedBytes(false);
            Diag.WorkingSetBytes     = Environment.WorkingSet;
            Diag.PauseTimePercentage = Info.PauseTimePercentage;
            ReadOnlySpan<TimeSpan> Pauses = Info.PauseDurations;
            Diag.LastPauseMs         = Pauses.Length > 0 ? Pauses[Pauses.Length - 1].TotalMilliseconds : 0.0;
            Diag.Gen0Collections     = GC.CollectionCount(0);
            Diag.Gen1Collections     = GC.CollectionCount(1);
            Diag.Gen2Collections     = GC.CollectionCount(2);
            Diag.PinnedObjects       = (int)Info.PinnedObjectsCount;

            Diag.Generation        = Scripts?.Generation ?? 0;
            Diag.LoadedTypeCount   = Scripts?.LoadedTypeCount ?? 0;
            Diag.ScriptsOnline     = Scripts?.EntityScripts != null ? 1 : 0;

            // Collectible script generations CoreCLR still has loaded; 1 == healthy. A count climbing across reloads is a real ALC unload leak.
            int Alive = 0;
            int Oldest = int.MaxValue;
            const string Prefix = "GameScripts.Gen";
            foreach (AssemblyLoadContext Context in AssemblyLoadContext.All)
            {
                if (Context.Name is string Name && Name.StartsWith(Prefix, StringComparison.Ordinal)
                    && int.TryParse(Name.AsSpan(Prefix.Length), out int Gen))
                {
                    Alive++;
                    if (Gen < Oldest)
                    {
                        Oldest = Gen;
                    }
                }
            }
            Diag.AliveScriptAlcCount   = Alive;
            Diag.OldestAliveGeneration = Alive > 0 ? Oldest : 0;

            return 1;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 0;
        }
    }

    // Pairs a new managed instance of the named script class with its native object and returns the strong GCHandle that owns it.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr CreateScriptable(byte* TypeName, int TypeNameLength, ulong NativePtr)
    {
        try
        {
            ScriptableRuntime? Runtime = Scripts?.Scriptables;
            return Runtime == null
                ? IntPtr.Zero
                : Runtime.Create(Interop.GetString(TypeName, TypeNameLength), NativePtr);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return IntPtr.Zero;
        }
    }

    // Runs a script type's declared initializers into its defaults, once per type at mint, since instances are copied from them.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static unsafe void ApplyScriptableDefaults(byte* TypeName, int NameLength, ulong DefaultObject)
    {
        try
        {
            Scripts?.Scriptables?.ApplyDefaults(Interop.GetString(TypeName, NameLength), DefaultObject);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Every type the loaded generation publishes, in one blob, so native mints the whole generation from one crossing.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void GetTypeManifest(IntPtr Sink, IntPtr Context)
    {
        try
        {
            if (Scripts?.Library is not { } Library || Scripts.Scriptables is not { } Scriptables || Sink == IntPtr.Zero)
            {
                return;
            }

            byte[] Blob = TypeManifest.Write(Library, Scriptables);
            var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, void>)Sink;
            fixed (byte* Bytes = Blob)
            {
                Add(Context, Bytes, Blob.Length);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Copied out of native memory here, since the buffers are freed as soon as the export returns.
    private static List<ScriptAssemblyUnit> ReadUnits(FSourceAssembly* Units, int Count)
    {
        var List = new List<ScriptAssemblyUnit>(Count < 0 ? 0 : Count);
        for (int Index = 0; Index < Count; Index++)
        {
            ref FSourceAssembly Unit = ref Units[Index];

            string DepsJoined = Interop.GetString(Unit.Deps, Unit.DepsLength);
            string[] Deps = DepsJoined.Length == 0
                ? Array.Empty<string>()
                : DepsJoined.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

            var Sources = new List<(string, string)>(Unit.SourceCount < 0 ? 0 : Unit.SourceCount);
            for (int S = 0; S < Unit.SourceCount; S++)
            {
                ref FSourceFile Source = ref Unit.Sources[S];
                Sources.Add((Interop.GetString(Source.Path, Source.PathLength), Interop.GetString(Source.Text, Source.TextLength)));
            }

            string ReferencesJoined = Interop.GetString(Unit.References, Unit.ReferencesLength);
            string[] References = ReferencesJoined.Length == 0
                ? Array.Empty<string>()
                : ReferencesJoined.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

            string DllPath = Interop.GetString(Unit.DllPath, Unit.DllPathLength);
            List.Add(new ScriptAssemblyUnit
            {
                Name = Interop.GetString(Unit.Name, Unit.NameLength),
                Dependencies = Deps,
                Sources = Sources,
                DllPath = DllPath.Length == 0 ? null : DllPath,
                References = References,
            });
        }
        return List;
    }

    // Native passes script sources bucketed per compilation unit with each unit's deps; each bucket becomes one assembly in the shared collectible ALC.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int LoadScripts(FSourceAssembly* Units, int Count)
    {
        try
        {
            if (Scripts == null)
            {
                return 1;
            }

            // A blocking load supersedes a background one, whose units may already be stale.
            DiscardPendingCompile();
            List<ScriptAssemblyUnit> Read = ReadUnits(Units, Count);
            Scripts.Language.SetUnits(Read);
            return Scripts.LoadOrReload(Read) ? 0 : 4;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 3;
        }
    }

    // Starts compiling on a worker thread, so the editor keeps drawing frames while Roslyn runs.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int BeginScriptCompile(FSourceAssembly* Units, int Count)
    {
        try
        {
            if (Scripts == null)
            {
                return 1;
            }

            DiscardPendingCompile();
            List<ScriptAssemblyUnit> Read = ReadUnits(Units, Count);
            Scripts.Language.SetUnits(Read);
            bool bUseLastBuildOnError = Scripts.bUseLastBuildOnError;
            PendingCompile = System.Threading.Tasks.Task.Run<FCompiledGeneration?>(() =>
            {
                try
                {
                    return ScriptManager.BuildGeneration(Read, bUseLastBuildOnError);
                }
                catch (Exception Exception)
                {
                    Interop.LogException(Exception);
                    return null;
                }
            });
            return 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 3;
        }
    }

    // 0 nothing pending, 1 still compiling, 2 ready to commit, 3 the compile failed and was dropped.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int PollScriptCompile()
    {
        System.Threading.Tasks.Task<FCompiledGeneration?>? Pending = PendingCompile;
        if (Pending == null)
        {
            return 0;
        }
        if (!Pending.IsCompleted)
        {
            return 1;
        }
        if (Pending.Result == null)
        {
            PendingCompile = null;
            return 3;
        }
        return 2;
    }

    // Swaps the finished generation in, on the game thread at a frame boundary.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int CommitScriptCompile()
    {
        try
        {
            System.Threading.Tasks.Task<FCompiledGeneration?>? Pending = PendingCompile;
            PendingCompile = null;
            if (Scripts == null || Pending == null)
            {
                Pending?.Result?.Abandon();
                return 1;
            }

            FCompiledGeneration? Compiled = Pending.Result;
            return Compiled != null && Scripts.Commit(Compiled) ? 0 : 4;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 3;
        }
    }

    // Drops a background compile without loading it, waiting so no compile outlives the request that started it.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void DiscardScriptCompile()
    {
        try
        {
            DiscardPendingCompile();
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    private static void DiscardPendingCompile()
    {
        System.Threading.Tasks.Task<FCompiledGeneration?>? Pending = PendingCompile;
        PendingCompile = null;
        Pending?.Result?.Abandon();
    }

    // One code editor question about an open script, written to Sink as records. Returns 2 while the script's unit is still warming.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int QueryScriptLanguage(int Kind, byte* Path, int PathLength, byte* Text, int TextLength, int Position, IntPtr Sink, IntPtr Context)
    {
        try
        {
            if (Scripts == null || Sink == IntPtr.Zero)
            {
                return 1;
            }

            // The editor counts UTF-8 bytes and Roslyn counts UTF-16 characters.
            int CharPosition = Text == null ? 0 : System.Text.Encoding.UTF8.GetCharCount(Text, Math.Clamp(Position, 0, TextLength));
            string? Answer = Scripts.Language.Query((EScriptQuery)Kind, Interop.GetString(Path, PathLength), Interop.GetString(Text, TextLength), CharPosition);
            if (Answer == null)
            {
                return 2;
            }

            byte[] Bytes = System.Text.Encoding.UTF8.GetBytes(Answer);
            var Emit = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, void>)Sink;
            fixed (byte* Data = Bytes)
            {
                Emit(Context, Data, Bytes.Length);
            }
            return 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 3;
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Tick()
    {
        try
        {
            ObjectReference.DrainReleases();
            Scripts?.Tick();
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Shutdown()
    {
        try
        {
            DiscardPendingCompile();
            Scripts?.Shutdown();
            Scripts = null;
            ObjectReference.OnHostShutdown();
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }
}
