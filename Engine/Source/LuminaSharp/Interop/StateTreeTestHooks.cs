using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

public static unsafe partial class Host
{
    // Loads Source as the whole script generation, or unloads every script when it is empty, leaving the native stages to the caller.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Test_LoadScriptSource(byte* Source, int Length)
    {
        try
        {
            if (Scripts == null)
            {
                return 1;
            }
            var Units = new List<ScriptAssemblyUnit>();
            if (Length > 0)
            {
                Units.Add(new ScriptAssemblyUnit
                {
                    Name = "ScriptSourceProbe",
                    Dependencies = Array.Empty<string>(),
                    Sources = new[] { ("ScriptSourceProbe.cs", Interop.GetString(Source, Length)) },
                });
            }
            return Scripts.LoadOrReload(Units) ? 0 : 2;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 3;
        }
    }
}

internal static unsafe class StateTreeTestHooks
{
    // Out holds IsRunning, whether State is active, the live Speed parameter before it is set to NewSpeed, and whether Event queued.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_StateTreeComponentQuery(IntPtr Component, byte* State, int StateLength, byte* EventName, int EventNameLength, int NewSpeed, int* Out)
    {
        ref SStateTreeComponent Tree = ref Unsafe.AsRef<SStateTreeComponent>((void*)Component);
        Out[0] = Tree.IsRunning() ? 1 : 0;
        Out[1] = Tree.GetActiveState() == Interop.GetString(State, StateLength) ? 1 : 0;
        Out[2] = -1;
        if (Tree.TryGetParameters<SStateTreeTestParameters>() is { } Parameters)
        {
            Out[2] = Parameters.Speed;
            Parameters.Speed = NewSpeed;
        }
        Out[3] = EventNameLength > 0 && Tree.SendEvent(HierarchicalTag.Request(Interop.GetString(EventName, EventNameLength))) ? 1 : 0;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void Test_StateTreeComponentRun(IntPtr Component, int bRun)
    {
        ref SStateTreeComponent Tree = ref Unsafe.AsRef<SStateTreeComponent>((void*)Component);
        if (bRun != 0)
        {
            Tree.Start();
        }
        else
        {
            Tree.Stop();
        }
    }
}
