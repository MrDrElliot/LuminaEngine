using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace LuminaSharp;

/// <summary>
/// </summary>
public static unsafe class ManagedCalls
{
    private const BindingFlags MemberFlags =
        BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.FlattenHierarchy;

    /// <summary>Resolves a managed type by name (LuminaSharp.dll, then the loaded script assembly, then an
    /// assembly-qualified fallback). Returns a strong GCHandle to the System.Type, or IntPtr.Zero.</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr ClassFind(byte* Name, int Length)
    {
        try
        {
            Type? Resolved = ResolveType(Interop.GetString(Name, Length));
            return Resolved == null ? IntPtr.Zero : GCHandle.ToIntPtr(GCHandle.Alloc(Resolved));
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return IntPtr.Zero;
        }
    }

    /// <summary>Default-constructs an instance of the GCHandle'd type; returns a strong GCHandle to it.</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static IntPtr ObjectNew(IntPtr TypeHandle)
    {
        try
        {
            if (TypeHandle == IntPtr.Zero || GCHandle.FromIntPtr(TypeHandle).Target is not Type Type)
            {
                return IntPtr.Zero;
            }

            object? Instance = Activator.CreateInstance(Type);
            return Instance == null ? IntPtr.Zero : GCHandle.ToIntPtr(GCHandle.Alloc(Instance));
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return IntPtr.Zero;
        }
    }

    /// <summary>Releases a strong GCHandle previously returned by ClassFind / ObjectNew.</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void FreeHandle(IntPtr Handle)
    {
        try
        {
            if (Handle != IntPtr.Zero)
            {
                GCHandle.FromIntPtr(Handle).Free();
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    /// <summary>Invokes a method by name on a target (an object handle, or a Type handle when bStatic != 0).
    /// Matched on name + argument count; arguments are coerced to the parameter types. A non-void return is
    /// written back through Sink(ctx, bytes, len) as one self-describing value. Returns 0 on success.</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int Invoke(IntPtr Target, byte bStatic, byte* Name, int NameLength, byte* Args, int ArgsLength, IntPtr Sink, IntPtr Context)
    {
        try
        {
            if (Target == IntPtr.Zero)
            {
                return 1;
            }

            object? TargetObject = GCHandle.FromIntPtr(Target).Target;
            bool IsStatic = bStatic != 0;
            Type? Type = IsStatic ? TargetObject as Type : TargetObject?.GetType();
            object? Instance = IsStatic ? null : TargetObject;
            if (Type == null)
            {
                return 2;
            }

            string MethodName = Interop.GetString(Name, NameLength);

            // Decode the arguments (natural types) before resolving the method, we need the count to match.
            var Reader = new FBlobReader(new ReadOnlySpan<byte>(Args, ArgsLength));
            int ArgCount = Reader.ReadInt32();
            object?[] RawArgs = new object?[ArgCount < 0 ? 0 : ArgCount];
            for (int Index = 0; Index < RawArgs.Length; Index++)
            {
                RawArgs[Index] = ReadBoxed(ref Reader);
            }

            BindingFlags Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy
                | (IsStatic ? BindingFlags.Static : BindingFlags.Instance);
            MethodInfo? Method = FindMethod(Type, MethodName, RawArgs.Length, Flags);
            if (Method == null)
            {
                Native.Log(ELogLevel.Error, $"ManagedCalls.Invoke: no method '{Type.FullName}.{MethodName}' taking {RawArgs.Length} arg(s).");
                return 3;
            }

            ParameterInfo[] Parameters = Method.GetParameters();
            object?[] CallArgs = new object?[Parameters.Length];
            for (int Index = 0; Index < Parameters.Length; Index++)
            {
                CallArgs[Index] = Coerce(Index < RawArgs.Length ? RawArgs[Index] : null, Parameters[Index].ParameterType);
            }

            object? ReturnValue = Method.Invoke(Instance, CallArgs);
            if (Method.ReturnType != typeof(void) && Sink != IntPtr.Zero)
            {
                WriteResult(Sink, Context, ReturnValue);
            }
            return 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 4;
        }
    }

    /// <summary>Reads a field or property by name; writes its value back through Sink. Returns 0 on success.</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int FieldGet(IntPtr ObjectHandle, byte* Name, int NameLength, IntPtr Sink, IntPtr Context)
    {
        try
        {
            if (ObjectHandle == IntPtr.Zero)
            {
                return 1;
            }

            object? Instance = GCHandle.FromIntPtr(ObjectHandle).Target;
            if (Instance == null)
            {
                return 2;
            }

            if (!TryGetMember(Instance.GetType(), Interop.GetString(Name, NameLength), Instance, out object? Value))
            {
                return 3;
            }
            if (Sink != IntPtr.Zero)
            {
                WriteResult(Sink, Context, Value);
            }
            return 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 4;
        }
    }

    /// <summary>Writes a field or property by name from a self-describing value blob. Returns 0 on success.</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int FieldSet(IntPtr ObjectHandle, byte* Name, int NameLength, byte* Value, int ValueLength)
    {
        try
        {
            if (ObjectHandle == IntPtr.Zero)
            {
                return 1;
            }

            object? Instance = GCHandle.FromIntPtr(ObjectHandle).Target;
            if (Instance == null)
            {
                return 2;
            }

            var Reader = new FBlobReader(new ReadOnlySpan<byte>(Value, ValueLength));
            object? Raw = ReadBoxed(ref Reader);
            return TrySetMember(Instance.GetType(), Interop.GetString(Name, NameLength), Instance, Raw) ? 0 : 3;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 4;
        }
    }

    // ---- internals ----

    private static Type? ResolveType(string Name)
    {
        if (string.IsNullOrEmpty(Name))
        {
            return null;
        }

        Type? Resolved = typeof(ManagedCalls).Assembly.GetType(Name); // engine + self-test types (LuminaSharp.dll)
        Resolved ??= Host.ResolveScriptType(Name);                    // loaded script assembly (collectible ALC)
        Resolved ??= Type.GetType(Name);                              // assembly-qualified fallback
        return Resolved;
    }

    private static MethodInfo? FindMethod(Type Type, string Name, int ArgCount, BindingFlags Flags)
    {
        MethodInfo? Fallback = null;
        foreach (MethodInfo Method in Type.GetMethods(Flags))
        {
            if (Method.Name != Name)
            {
                continue;
            }
            if (Method.GetParameters().Length == ArgCount)
            {
                return Method;
            }
            Fallback ??= Method; // last resort: same name, different arity (coercion fills/drops)
        }
        return Fallback;
    }

    private static bool TryGetMember(Type Type, string Name, object Instance, out object? Value)
    {
        FieldInfo? Field = Type.GetField(Name, MemberFlags);
        if (Field != null)
        {
            Value = Field.GetValue(Instance);
            return true;
        }

        PropertyInfo? Property = Type.GetProperty(Name, MemberFlags);
        if (Property != null && Property.CanRead)
        {
            Value = Property.GetValue(Instance);
            return true;
        }

        Value = null;
        return false;
    }

    private static bool TrySetMember(Type Type, string Name, object Instance, object? Raw)
    {
        FieldInfo? Field = Type.GetField(Name, MemberFlags);
        if (Field != null)
        {
            Field.SetValue(Instance, Coerce(Raw, Field.FieldType));
            return true;
        }

        PropertyInfo? Property = Type.GetProperty(Name, MemberFlags);
        if (Property != null && Property.CanWrite)
        {
            Property.SetValue(Instance, Coerce(Raw, Property.PropertyType));
            return true;
        }

        return false;
    }

    private static void WriteResult(IntPtr Sink, IntPtr Context, object? Value)
    {
        using var Stream = new MemoryStream();
        using (var Writer = new BinaryWriter(Stream, Encoding.UTF8, leaveOpen: true))
        {
            WriteBoxed(Writer, Value);
            Writer.Flush();
        }

        byte[] Blob = Stream.ToArray();
        var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, void>)Sink;
        fixed (byte* Bytes = Blob)
        {
            Add(Context, Bytes, Blob.Length);
        }
    }

    // Self-describing value encode (infer the wire kind from the runtime type). Mirror of the native
    // ReadManagedArg; unsupported kinds (vectors, objects, null) encode as Nil and arrive as Void natively.
    private static void WriteBoxed(BinaryWriter Writer, object? Value)
    {
        switch (Value)
        {
            case bool Bool:
                Writer.Write((byte)EScriptValueKind.Bool);
                Writer.Write((byte)(Bool ? 1 : 0));
                break;
            case Enum:
                Writer.Write((byte)EScriptValueKind.Int);
                Writer.Write(Convert.ToInt64(Value));
                break;
            case sbyte or byte or short or ushort or int or uint or long or ulong:
                Writer.Write((byte)EScriptValueKind.Int);
                Writer.Write(Convert.ToInt64(Value));
                break;
            case float or double:
                Writer.Write((byte)EScriptValueKind.Double);
                Writer.Write(Convert.ToDouble(Value));
                break;
            case string Text:
                Writer.Write((byte)EScriptValueKind.String);
                byte[] Bytes = Encoding.UTF8.GetBytes(Text);
                Writer.Write(Bytes.Length);
                Writer.Write(Bytes);
                break;
            default:
                Writer.Write((byte)EScriptValueKind.Nil);
                break;
        }
    }

    // Self-describing value decode to the natural CLR type. Mirror of the native WriteManagedArg.
    private static object? ReadBoxed(ref FBlobReader Reader)
    {
        var Kind = (EScriptValueKind)Reader.ReadByte();
        switch (Kind)
        {
            case EScriptValueKind.Bool:
                return Reader.ReadByte() != 0;
            case EScriptValueKind.Int:
                return Reader.ReadInt64();
            case EScriptValueKind.Double:
                return Reader.ReadDouble();
            case EScriptValueKind.String:
                return Reader.ReadString();
            default:
                return null;
        }
    }

    private static object? Coerce(object? Raw, Type Target)
    {
        if (Raw == null)
        {
            return Target.IsValueType ? Activator.CreateInstance(Target) : null;
        }
        if (Target.IsInstanceOfType(Raw))
        {
            return Raw;
        }
        if (Target.IsEnum)
        {
            return Enum.ToObject(Target, Convert.ToInt64(Raw));
        }
        if (Target == typeof(float))
        {
            return Convert.ToSingle(Raw);
        }
        if (Target == typeof(double))
        {
            return Convert.ToDouble(Raw);
        }
        if (Target == typeof(string))
        {
            return Raw.ToString();
        }

        try
        {
            return Convert.ChangeType(Raw, Target);
        }
        catch
        {
            Native.Log(ELogLevel.Warn, $"Coerce: failed to convert {Raw?.GetType().Name ?? "null"} to {Target.Name}.");
            return Raw;
        }
    }
}
