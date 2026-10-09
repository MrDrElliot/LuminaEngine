using System;
using System.Collections.Generic;
using System.Linq.Expressions;
using System.Reflection;
using LuminaSharp.ScriptProperties;
using System.Runtime.CompilerServices;
using Lumina;

namespace LuminaSharp;

// Where one field of a script struct sits in the native value, and the managed field it maps to.
internal readonly record struct FScriptStructField(FieldInfo Field, int Offset);

// The native layout of a struct declared in script, with each field at its own alignment in declaration order as native mints it.
internal sealed class FScriptStructLayout
{
    public int Size;
    public int Alignment = 1;
    public bool bMarshalled;
    public FScriptStructField[] Fields = Array.Empty<FScriptStructField>();
}

internal static unsafe partial class ScriptStructLayout
{
    [NativeCall("LuminaSharp_ScriptValueLayout")]
    private static partial long NativeValueLayout(int Kind, string StructName);

    // Weak, so a hot reload's old struct types are not pinned.
    private static readonly ConditionalWeakTable<Type, FScriptStructLayout> Layouts = new();

    // The same fields TypeLibrary.BuildMembers exports for a struct, in the same order, or the two layouts would disagree.
    public static IEnumerable<FieldInfo> StoredFields(Type Type)
    {
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        foreach (FieldInfo Field in Type.GetFields(Flags))
        {
            if (TypeLibrary.RuleFor(Field, EScriptMemberKind.Field, bOwnerIsNativeObject: false).bStored)
            {
                yield return Field;
            }
        }
    }

    // A struct declared in script whose fields reach a string, which is what keeps it from being read in place.
    public static bool NeedsMarshalling(Type Type)
    {
        return IsScriptStruct(Type) && Of(Type).bMarshalled;
    }

    public static FScriptStructLayout Of(Type Type)
    {
        return Layouts.GetValue(Type, Build);
    }

    private static bool IsScriptStruct(Type Type)
    {
        return Type.IsValueType && !Type.IsPrimitive && !Type.IsEnum && Type != typeof(FString) && Type != typeof(Entity)
            && NativeNameOf(Type) == null && Nullable.GetUnderlyingType(Type) == null;
    }

    private static string? NativeNameOf(Type Type)
    {
        return Type.GetCustomAttribute<NativeTypeAttribute>()?.Name ?? Type.GetCustomAttribute<NativeLayoutAttribute>()?.NativeType;
    }

    private static FScriptStructLayout Build(Type Type)
    {
        var Layout = new FScriptStructLayout();
        var Fields = new List<FScriptStructField>();
        int Running = 0;
        foreach (FieldInfo Field in StoredFields(Type))
        {
            (int Size, int Alignment, bool bMarshalled) = SizeAlignOf(Field.FieldType);
            int Offset = AlignUp(Running, Alignment);
            Fields.Add(new FScriptStructField(Field, Offset));
            Running = Offset + Size;
            Layout.Alignment = Math.Max(Layout.Alignment, Alignment);
            Layout.bMarshalled |= bMarshalled;
        }
        Layout.Size = Running > 0 ? AlignUp(Running, Layout.Alignment) : 0;
        Layout.Fields = Fields.ToArray();
        return Layout;
    }

    private static (int Size, int Alignment, bool bMarshalled) SizeAlignOf(Type Type)
    {
        if (Type == typeof(bool))
        {
            return (1, 1, false);
        }
        if (Type.IsEnum)
        {
            Type = Enum.GetUnderlyingType(Type);
        }
        if (Type.IsPrimitive)
        {
            int Size = System.Runtime.InteropServices.Marshal.SizeOf(Type);
            return (Size, Size, false);
        }
        if (Type == typeof(string) || Type == typeof(FString))
        {
            (int Size, int Alignment) = NativeLayoutOf(EPropertyType.String, "");
            return (Size, Alignment, true);
        }
        if (Type == typeof(Entity))
        {
            (int Size, int Alignment) = NativeLayoutOf(EPropertyType.Entity, "");
            return (Size, Alignment, false);
        }
        if (NativeNameOf(Type) is string NativeName)
        {
            (int Size, int Alignment) = NativeLayoutOf(EPropertyType.Struct, NativeName);
            return (Size, Alignment, false);
        }

        FScriptStructLayout Nested = Of(Type);
        return (Nested.Size, Nested.Alignment, Nested.bMarshalled);
    }

    private static (int Size, int Alignment) NativeLayoutOf(EPropertyType Kind, string StructName)
    {
        long Packed = NativeValueLayout((int)Kind, StructName);
        if (Packed < 0)
        {
            throw new InvalidOperationException($"Native has no layout for {Kind} '{StructName}'.");
        }
        return ((int)(Packed & 0xFFFFFFFF), (int)(Packed >> 32));
    }

    private static int AlignUp(int Value, int Alignment)
    {
        return Alignment <= 1 ? Value : (Value + Alignment - 1) / Alignment * Alignment;
    }

    private static readonly MethodInfo ReadValueMethod = typeof(ScriptStructLayout).GetMethod(nameof(ReadValue), BindingFlags.NonPublic | BindingFlags.Static)!;
    private static readonly MethodInfo WriteValueMethod = typeof(ScriptStructLayout).GetMethod(nameof(WriteValue), BindingFlags.NonPublic | BindingFlags.Static)!;

    // Expression trees define no arithmetic on nint, so the offset is applied here.
    private static nint FieldAddress(nint Address, int Offset) => Address + Offset;
    private static T ReadValue<T>(nint Address) => Unsafe.ReadUnaligned<T>((void*)Address);
    private static void WriteValue<T>(nint Address, T Value) => Unsafe.WriteUnaligned((void*)Address, Value);
    private static bool ReadBool(nint Address) => *(byte*)Address != 0;
    private static void WriteBool(nint Address, bool Value) => *(byte*)Address = (byte)(Value ? 1 : 0);
    private static string ReadManagedString(nint Address) => NativeMarshal.ReadString(Address);
    private static void WriteManagedString(nint Address, string? Value) => Native.StringAssign(Address, Value ?? string.Empty);
    private static FString ReadString(nint Address) => new(NativeMarshal.ReadString(Address));
    private static void WriteString(nint Address, FString Value) => Native.StringAssign(Address, Value.ToString());

    private static MethodInfo Helper(string Name)
    {
        return typeof(ScriptStructLayout).GetMethod(Name, BindingFlags.NonPublic | BindingFlags.Static)!;
    }

    private static Expression ReadField(Type Type, Expression Address)
    {
        if (Type == typeof(bool))
        {
            return Expression.Call(Helper(nameof(ReadBool)), Address);
        }
        if (Type == typeof(string))
        {
            return Expression.Call(Helper(nameof(ReadManagedString)), Address);
        }
        if (Type == typeof(FString))
        {
            return Expression.Call(Helper(nameof(ReadString)), Address);
        }
        if (NeedsMarshalling(Type))
        {
            return Expression.Call(typeof(ScriptStructMarshal<>).MakeGenericType(Type).GetMethod(nameof(ScriptStructMarshal<int>.Read))!, Address);
        }
        return Expression.Call(ReadValueMethod.MakeGenericMethod(Type), Address);
    }

    private static Expression WriteField(Type Type, Expression Address, Expression Value)
    {
        if (Type == typeof(bool))
        {
            return Expression.Call(Helper(nameof(WriteBool)), Address, Value);
        }
        if (Type == typeof(string))
        {
            return Expression.Call(Helper(nameof(WriteManagedString)), Address, Value);
        }
        if (Type == typeof(FString))
        {
            return Expression.Call(Helper(nameof(WriteString)), Address, Value);
        }
        if (NeedsMarshalling(Type))
        {
            return Expression.Call(typeof(ScriptStructMarshal<>).MakeGenericType(Type).GetMethod(nameof(ScriptStructMarshal<int>.Write))!, Address, Value);
        }
        return Expression.Call(WriteValueMethod.MakeGenericMethod(Type), Address, Value);
    }

    private static readonly ConditionalWeakTable<Type, Func<nint, object>> BoxedReaders = new();
    private static readonly ConditionalWeakTable<Type, Action<nint, object>> BoxedWriters = new();

    // A call frame carries values as objects, so it reaches the typed marshal through one delegate per struct type.
    public static object ReadBoxed(Type Type, nint Address)
    {
        return BoxedReaders.GetValue(Type, Key => (Func<nint, object>)typeof(ScriptStructLayout)
            .GetMethod(nameof(BoxReader), BindingFlags.NonPublic | BindingFlags.Static)!.MakeGenericMethod(Key).Invoke(null, null)!)(Address);
    }

    public static void WriteBoxed(Type Type, nint Address, object? Value)
    {
        BoxedWriters.GetValue(Type, Key => (Action<nint, object>)typeof(ScriptStructLayout)
            .GetMethod(nameof(BoxWriter), BindingFlags.NonPublic | BindingFlags.Static)!.MakeGenericMethod(Key).Invoke(null, null)!)(Address, Value ?? Activator.CreateInstance(Type)!);
    }

    private static Func<nint, object> BoxReader<T>() where T : struct => Address => ScriptStructMarshal<T>.Read(Address);

    private static Action<nint, object> BoxWriter<T>() where T : struct => (Address, Value) => ScriptStructMarshal<T>.Write(Address, (T)Value);

    public static Func<nint, T> CompileReader<T>()
    {
        FScriptStructLayout Layout = Of(typeof(T));
        ParameterExpression Address = Expression.Parameter(typeof(nint), "Address");
        ParameterExpression Result = Expression.Variable(typeof(T), "Result");
        var Body = new List<Expression> { Expression.Assign(Result, Expression.Default(typeof(T))) };
        foreach (FScriptStructField Field in Layout.Fields)
        {
            Expression At = Expression.Call(Helper(nameof(FieldAddress)), Address, Expression.Constant(Field.Offset));
            Body.Add(Expression.Assign(Expression.Field(Result, Field.Field), ReadField(Field.Field.FieldType, At)));
        }
        Body.Add(Result);
        return Expression.Lambda<Func<nint, T>>(Expression.Block(new[] { Result }, Body), Address).Compile();
    }

    public static Action<nint, T> CompileWriter<T>()
    {
        FScriptStructLayout Layout = Of(typeof(T));
        ParameterExpression Address = Expression.Parameter(typeof(nint), "Address");
        ParameterExpression Value = Expression.Parameter(typeof(T), "Value");
        var Body = new List<Expression>();
        foreach (FScriptStructField Field in Layout.Fields)
        {
            Expression At = Expression.Call(Helper(nameof(FieldAddress)), Address, Expression.Constant(Field.Offset));
            Body.Add(WriteField(Field.Field.FieldType, At, Expression.Field(Value, Field.Field)));
        }
        Body.Add(Expression.Empty());
        return Expression.Lambda<Action<nint, T>>(Expression.Block(Body), Address, Value).Compile();
    }
}

// A struct declared in script that holds a string crosses field by field, decoding its strings on read and assigning them natively on write.
public static class ScriptStructMarshal<T> where T : struct
{
    private static readonly Func<nint, T> Reader = ScriptStructLayout.CompileReader<T>();
    private static readonly Action<nint, T> Writer = ScriptStructLayout.CompileWriter<T>();

    public static T Read(nint Address) => Reader(Address);

    public static void Write(nint Address, T Value) => Writer(Address, Value);
}
