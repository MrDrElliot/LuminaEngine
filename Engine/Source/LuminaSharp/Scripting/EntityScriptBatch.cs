using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace LuminaSharp;

// One crossing per run of scripts instead of one per script, which is most of what an empty OnUpdate cost.
internal static unsafe class EntityScriptBatch
{
    // Returns how many ran, stopping early once the native structure epoch moves, since the driver then has to re-check the rest.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int DispatchEntityScriptUpdates(IntPtr* Handles, int Count, float DeltaTime, uint* StructureEpoch)
    {
        uint Epoch = Volatile.Read(ref *StructureEpoch);
        using var Scope = Game.Snapshot();
        for (int Index = 0; Index < Count; ++Index)
        {
            object? Target = null;
            try
            {
                if (GCHandle.FromIntPtr(Handles[Index]).Target is Lumina.CEntityScript Script)
                {
                    Target = Script;
                    Game.RetargetScriptEvent(Script);
                    Script.OnUpdate(DeltaTime);
                }
            }
            catch (Exception Exception)
            {
                NativeBindings.ScriptEventException(Target, "OnUpdate", Exception);
            }

            if (Volatile.Read(ref *StructureEpoch) != Epoch)
            {
                return Index + 1;
            }
        }
        return Count;
    }
}
