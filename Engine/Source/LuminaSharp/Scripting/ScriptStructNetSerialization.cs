using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace LuminaSharp;

// The managed half of CScriptStruct::NetSerializeCustom. Native asks once per script struct type, then for every value of one that packs itself.
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ScriptStructNetSerialization
{
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int IsScriptStructNetSerializable(byte* Name, int NameSize)
    {
        return Host.Library?.FindNetSerializer(Encoding.UTF8.GetString(Name, NameSize)) != null ? 1 : 0;
    }

    // Zero when the value could not be packed, which native turns into a broken message rather than a desynced one.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int NetSerializeScriptStruct(byte* Name, int NameSize, IntPtr Archive, IntPtr Data)
    {
        try
        {
            TypeLibrary.FNetSerializeCall? Serialize = Host.Library?.FindNetSerializer(Encoding.UTF8.GetString(Name, NameSize));
            if (Serialize == null || Data == IntPtr.Zero)
            {
                return 0;
            }
            Serialize(Data, new FNetArchive(Archive));
            return 1;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 0;
        }
    }
}
