using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace LuminaSharp;

/// Managed entry surface for FDotNetHost. Entry method names are a name-based ABI contract and must not be renamed. Per-instance entries take an IntPtr that IS a strong GCHandle to the managed EntityScript.
public static unsafe partial class Host
{
    // Must equal Lumina::DotNet::GAbiVersion. Bump on ABI breaks.
    private const int AbiVersion = 14;

    // Logical name for the engine module hosting this assembly (Runtime); resolved to a native handle via ModuleHandle.
    public const string NativeLibrary = "LuminaNative";

    private static ScriptManager? Scripts;
    private static IntPtr NativeModule;
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
            BusRegistry.Remove(World);
            UIDataModel.RemoveForWorld(World);
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"OnWorldTeardown threw: {Exception}");
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
            Diag.EntityScriptCount = Scripts?.EntityScripts?.TypeNames.Count ?? 0;
            Diag.EntitySystemCount = Scripts?.EntitySystemCount ?? 0;
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


    /// A native script delegate with live managed bindings was destroyed; free the matching GCHandles.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void OnNativeDelegateDestroyed(IntPtr Delegate)
    {
        try
        {
            DelegateBindings.ForgetByAddress(Delegate);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    /// Writes a script type's [Property] schema + defaults to a recursive blob and hands it to a native sink (called once).
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void GetScriptSchema(byte* ScriptClass, int ClassLength, IntPtr Sink, IntPtr Context)
    {
        try
        {
            byte[]? Blob = Scripts?.EntityScripts?.Schema(Interop.GetString(ScriptClass, ClassLength));
            if (Blob == null || Sink == IntPtr.Zero)
            {
                return;
            }

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

    /// Writes a script type's [Button] methods to a native sink (called once); drives the inspector's action buttons.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void GetScriptButtons(byte* ScriptClass, int ClassLength, IntPtr Sink, IntPtr Context)
    {
        try
        {
            byte[]? Blob = Scripts?.EntityScripts?.Buttons(Interop.GetString(ScriptClass, ClassLength));
            if (Blob == null || Sink == IntPtr.Zero)
            {
                return;
            }

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


    /// Resumes an Asset.LoadAsync continuation; Callback is the GCHandle to an Action&lt;IntPtr&gt; trampoline, freed here.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void InvokeAssetCallback(IntPtr Callback, IntPtr Object)
    {
        try
        {
            GCHandle Handle = GCHandle.FromIntPtr(Callback);
            Asset.Complete(Callback);
            Action<IntPtr>? Trampoline = Handle.Target as Action<IntPtr>;
            Handle.Free();
            Trampoline?.Invoke(Object);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    /// Reports every loaded EntityScript type's full name to a native sink (once per type), for the editor's script picker.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void EnumerateEntityScripts(IntPtr Sink, IntPtr Context)
    {
        try
        {
            EntityScriptRuntime? Runtime = Scripts?.EntityScripts;
            if (Runtime == null || Sink == IntPtr.Zero)
            {
                return;
            }

            var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, void>)Sink;
            Span<byte> Scratch = stackalloc byte[256];
            foreach (string Name in Runtime.TypeNames)
            {
                Interop.FInteropString Encoded = new(Name, Scratch);
                try
                {
                    Add(Context, Encoded.Pointer, Encoded.Length);
                }
                finally
                {
                    Encoded.Free();
                }
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Scriptable bridge: a C# subclass of a REFLECT(Scriptable) native CObject. Native creates the CObject
    // (a minted CClass), then binds the managed instance to it via a GCHandle stored in the object's
    // managed-instance slot, which owns it from there (no DestroyScriptable: the object's destructor and the
    // teardown drain both go through that slot).

    /// Instantiates the named Scriptable subclass, pairs it to the native object, and returns a strong
    /// GCHandle (IntPtr.Zero on failure). The override mask is no longer reported per instance -- it is
    /// type-uniform and rides on the minted CClass (see ScriptableRuntime.Enumerate).
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

    /// Reports each discovered Scriptable C# type as (full name, native base class name) to a native sink, so
    /// the host can mint a CClass deriving from that native base.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void EnumerateScriptables(IntPtr Sink, IntPtr Context)
    {
        try
        {
            Scripts?.Scriptables?.Enumerate(Sink, Context);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    /// Reports each (prior name, current name) pair from [Alias] on a script class, so the host can record
    /// where a renamed class went and keep saved references resolving.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void EnumerateScriptableAliases(IntPtr Sink, IntPtr Context)
    {
        try
        {
            Scripts?.Scriptables?.EnumerateAliases(Sink, Context);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    /// Runs a Scriptable type's declared [Property] initializers into its class default object. Called once
    /// per type at mint, after the CDO exists; instances are copied from it.
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

    /// Reports each marked data type as (StableId, native base struct name) to a native sink, so the host can
    /// mint a CScriptStruct deriving from that native base.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void EnumerateScriptStructs(IntPtr Sink, IntPtr Context)
    {
        try
        {
            Scripts?.DataStructs?.Enumerate(Sink, Context);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    /// Writes the member schema blob for a marked data type, addressed by its StableId.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static unsafe void GetScriptStructSchema(byte* StableId, int IdLength, IntPtr Sink, IntPtr Context)
    {
        try
        {
            byte[]? Blob = Scripts?.DataStructs?.Schema(Interop.GetString(StableId, IdLength));
            if (Blob == null || Sink == IntPtr.Zero)
            {
                return;
            }

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
            return Scripts.LoadOrReload(List) ? 0 : 4;
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
            Scripts?.Shutdown();
            Scripts = null;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }
}
