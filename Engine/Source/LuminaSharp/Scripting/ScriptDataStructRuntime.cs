using System;
using System.Collections.Generic;

namespace LuminaSharp;

/// <summary>
/// Publishes the C# types marked with a <see cref="ScriptStructBaseAttribute"/> so the host can mint a
/// CScriptStruct for each, deriving from the native struct the marker names.
/// </summary>
/// <remarks>
/// Holds no managed instances, unlike the other runtimes: these types are pure data shapes, and the
/// engine only ever needs their layout and their base. That is also why there is no FreeAll here - there
/// is nothing to pin the collectible ALC. Discovery already happened in <see cref="TypeLibrary"/>, on its
/// own pass over every loaded type, so this class only reports what was found.
/// </remarks>
internal sealed class ScriptDataStructRuntime
{
    private readonly TypeLibrary Library;

    public ScriptDataStructRuntime(TypeLibrary Library)
    {
        this.Library = Library;
    }

    /// <summary>Number of published data types, for the load log.</summary>
    public int Count
    {
        get
        {
            int Total = 0;
            foreach (KeyValuePair<string, DataStructEntry> _ in Library.DataStructTypes)
            {
                Total++;
            }
            return Total;
        }
    }

    /// <summary>
    /// Reports each marked type as (StableId, native base name) to a native sink, so the host can mint a
    /// CScriptStruct deriving from that base. Same shape as the Scriptable enumeration.
    /// </summary>
    public unsafe void Enumerate(IntPtr Sink, IntPtr Context)
    {
        if (Sink == IntPtr.Zero)
        {
            return;
        }

        var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, byte*, int, void>)Sink;
        Span<byte> NameScratch = stackalloc byte[256];
        Span<byte> BaseScratch = stackalloc byte[256];

        foreach (KeyValuePair<string, DataStructEntry> Pair in Library.DataStructTypes)
        {
            if (string.IsNullOrEmpty(Pair.Value.NativeBase))
            {
                continue;
            }

            Interop.FInteropString Name = new(Pair.Key, NameScratch);
            Interop.FInteropString Base = new(Pair.Value.NativeBase, BaseScratch);
            try
            {
                Add(Context, Name.Pointer, Name.Length, Base.Pointer, Base.Length);
            }
            finally
            {
                Name.Free();
                Base.Free();
            }
        }
    }

    /// <summary>The member schema blob for a marked type by StableId, or null if unknown.</summary>
    public byte[]? Schema(string StableId)
    {
        TypeDescription? Description = Library.GetDataStruct(StableId);
        return Description != null ? Serializer.WriteSchema(Description) : null;
    }
}
