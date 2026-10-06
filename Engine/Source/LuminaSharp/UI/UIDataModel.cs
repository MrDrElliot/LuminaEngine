using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// One argument of a data-event-* call; mirrors native RmlUi::FUIArg (UTF-8 ptr+len).
[StructLayout(LayoutKind.Sequential)]
internal struct UIArg
{
    public IntPtr Ptr;
    public int Len;
}

// Binds a ViewModel's or UIScript's [Bind] members to a named RmlUi data model, comparing them once a frame so plain assignments reach the view.
public sealed unsafe class UIDataModel : IDisposable
{
    // Game-thread only, so a plain dictionary is fine. Lets World.UI.GetModel re-fetch a model by name.
    private static readonly Dictionary<(ulong World, string Name), UIDataModel> Registry = new();

    private readonly ulong _world;
    private readonly string _name;
    private readonly object _target;
    private readonly UIModelShape _shape;
    private Lumina.FUIDataModel _native;
    private GCHandle _self;

    // Native field id per scalar and per list, parallel to the shape's lists. Negative when the bind failed.
    private readonly int[] _scalarField;
    private readonly int[] _listField;

    // What the view last received, so a frame where nothing changed pushes nothing.
    private readonly object?[] _lastScalar;
    private readonly long[] _lastList;

    private readonly Dictionary<string, int> _scalarByName = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> _listByName = new(StringComparer.Ordinal);

    // Set while applying a value the view sent, so the property setter's Set() does not echo it back.
    private bool _applyingFromNative;

    internal UIDataModel(ulong World, string Name, object Target)
    {
        _world = World;
        _name = Name;
        _target = Target;
        _shape = UIModelShape.Of(Target.GetType());
        _scalarField = new int[_shape.Scalars.Count];
        _listField = new int[_shape.Lists.Count];
        _lastScalar = new object?[_shape.Scalars.Count];
        _lastList = new long[_shape.Lists.Count];
        _self = GCHandle.Alloc(this);

        _native = Lumina.CUILibrary.CreateDataModel(UI.WorldOf(World), Name,
            (ulong)GCHandle.ToIntPtr(_self), (ulong)SetThunkPtr, (ulong)EventThunkPtr);
        if (!_native.IsValid)
        {
            _self.Free();
            return;
        }

        BindMembers();
        if (_target is ViewModel Model)
        {
            Model.Binding = this;
        }
        Registry[(World, Name)] = this;
        PushAll();
    }

    // False if the model failed to register or has been disposed.
    public bool IsValid => _native.IsValid;

    public string Name => _name;

    // The view-model this binding drives, or null when the target is a UIScript.
    public ViewModel? ViewModel => _target as ViewModel;

    public object Target => _target;

    internal ulong World => _world;

    internal static UIDataModel? Find(ulong World, string Name)
        => Registry.TryGetValue((World, Name), out UIDataModel? Model) ? Model : null;

    private void BindMembers()
    {
        for (int Index = 0; Index < _shape.Scalars.Count; ++Index)
        {
            UIModelShape.FScalar Scalar = _shape.Scalars[Index];
            _scalarField[Index] = Lumina.CUILibrary.BindScalar(_native, Scalar.Name, Scalar.VarType);
            _scalarByName[Scalar.Name] = Index;
            _scalarByName[Scalar.MemberName] = Index;
        }

        for (int Index = 0; Index < _shape.Lists.Count; ++Index)
        {
            UIModelShape.FList List = _shape.Lists[Index];
            int Field = Lumina.CUILibrary.BindList(_native, List.Name);
            _listField[Index] = Field;
            if (Field >= 0)
            {
                foreach (UIModelShape.FListItem Item in List.Items)
                {
                    Lumina.CUILibrary.BindListMember(_native, Field, Item.Name);
                }
            }
            _listByName[List.Name] = Index;
            _listByName[List.MemberName] = Index;
        }

        for (int Index = 0; Index < _shape.Commands.Count; ++Index)
        {
            Lumina.CUILibrary.BindCommand(_native, _shape.Commands[Index].Name, Index);
        }
    }

    // Pushes one member by its bound or member name, for ViewModel.Set and for a collection edited in place.
    internal void OnPropertyChanged(string Name)
    {
        if (_applyingFromNative || !_native.IsValid)
        {
            return;
        }
        if (_scalarByName.TryGetValue(Name, out int Scalar))
        {
            PushScalar(Scalar, ReadScalar(Scalar));
        }
        else if (_listByName.TryGetValue(Name, out int List))
        {
            PushList(List, force: true);
        }
    }

    // Re-push every bound member and mark the whole model dirty.
    public void PushAll()
    {
        if (!_native.IsValid)
        {
            return;
        }
        for (int Index = 0; Index < _shape.Scalars.Count; ++Index)
        {
            object? Value = ReadScalar(Index);
            _lastScalar[Index] = Value;
            WriteScalar(Index, Value);
        }
        for (int Index = 0; Index < _shape.Lists.Count; ++Index)
        {
            PushList(Index, force: true);
        }
        Lumina.CUILibrary.MarkAllDirty(_native);
    }

    // Sends whatever changed since the last push. Runs for every model of a world just before its UI updates.
    internal void Poll()
    {
        if (!_native.IsValid)
        {
            return;
        }
        for (int Index = 0; Index < _shape.Scalars.Count; ++Index)
        {
            object? Value = ReadScalar(Index);
            if (!Equals(Value, _lastScalar[Index]))
            {
                PushScalar(Index, Value);
            }
        }
        for (int Index = 0; Index < _shape.Lists.Count; ++Index)
        {
            PushList(Index, force: false);
        }
    }

    private object? ReadScalar(int Index)
    {
        try
        {
            return _shape.Scalars[Index].Get(_target);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return _lastScalar[Index];
        }
    }

    private void PushScalar(int Index, object? Value)
    {
        _lastScalar[Index] = Value;
        if (_scalarField[Index] < 0)
        {
            return;
        }
        WriteScalar(Index, Value);
        Lumina.CUILibrary.MarkDirty(_native, _scalarField[Index]);
    }

    private void WriteScalar(int Index, object? Value)
    {
        int Field = _scalarField[Index];
        if (Field < 0)
        {
            return;
        }
        Lumina.EUIVarType Type = _shape.Scalars[Index].VarType;
        if (Type == Lumina.EUIVarType.String)
        {
            Lumina.CUILibrary.SetString(_native, Field, Value as string ?? string.Empty);
        }
        else
        {
            Lumina.CUILibrary.SetNumber(_native, Field, ToNumber(Value, Type));
        }
    }

    private void PushList(int Index, bool force)
    {
        int Field = _listField[Index];
        if (Field < 0)
        {
            return;
        }
        UIModelShape.FList List = _shape.Lists[Index];
        object? Collection;
        try
        {
            Collection = List.Get(_target);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return;
        }

        long Signature = UIModelShape.Signature(Collection);
        if (!force && Signature == _lastList[Index])
        {
            return;
        }
        _lastList[Index] = Signature;

        var Rows = new List<object>();
        if (Collection is IEnumerable Items)
        {
            foreach (object? Item in Items)
            {
                if (Item != null)
                {
                    Rows.Add(Item);
                }
            }
        }

        Lumina.CUILibrary.ResizeList(_native, Field, Rows.Count);
        for (int Row = 0; Row < Rows.Count; ++Row)
        {
            for (int Col = 0; Col < List.Items.Length; ++Col)
            {
                Lumina.CUILibrary.SetListCell(_native, Field, Row, Col, ToCell(List.Items[Col].Get(Rows[Row])));
            }
        }
        Lumina.CUILibrary.MarkListDirty(_native, Field);
    }

    // ---- native -> managed ----

    private void ApplyFromNative(int Field, double Number, IntPtr Str, int StrLen)
    {
        int Index = Array.IndexOf(_scalarField, Field);
        if (Index < 0)
        {
            return;
        }
        UIModelShape.FScalar Scalar = _shape.Scalars[Index];
        if (Scalar.Set == null)
        {
            return;
        }

        object Value = Scalar.VarType == Lumina.EUIVarType.String
            ? (StrLen > 0 ? Marshal.PtrToStringUTF8(Str, StrLen) ?? string.Empty : string.Empty)
            : ConvertNumber(Scalar.ValueType, Number);

        _applyingFromNative = true;
        try
        {
            using var Scope = EnterTarget();
            Scalar.Set(_target, Value);
            _lastScalar[Index] = Value;
        }
        catch (Exception Exception)
        {
            Report(Scalar.MemberName, Exception);
        }
        finally
        {
            _applyingFromNative = false;
        }
    }

    private void InvokeCommand(int CommandId, int ArgCount, UIArg* Args)
    {
        if (CommandId < 0 || CommandId >= _shape.Commands.Count)
        {
            return;
        }
        UIModelShape.FCommand Command = _shape.Commands[CommandId];

        object?[] Call = Command.Parameters.Length == 0 ? Array.Empty<object?>() : new object?[Command.Parameters.Length];
        for (int Index = 0; Index < Call.Length; ++Index)
        {
            string Arg = Index < ArgCount ? (Marshal.PtrToStringUTF8(Args[Index].Ptr, Args[Index].Len) ?? string.Empty) : string.Empty;
            Call[Index] = ConvertArg(Arg, Command.Parameters[Index].ParameterType);
        }

        try
        {
            using var Scope = EnterTarget();
            Command.Method.Invoke(_target, Call);
        }
        catch (TargetInvocationException Thrown) when (Thrown.InnerException != null)
        {
            Report(Command.Name, Thrown.InnerException);
        }
    }

    // A UIScript's handlers run inside its own script context, the same as its other callbacks.
    private Engine.Scope EnterTarget()
    {
        return _target is EntityScript Script && !Script.Entity.IsNull ? Engine.Push(Script.World, Script.Entity, Script) : Engine.Snapshot();
    }

    private void Report(string Member, Exception Exception)
    {
        if (_target is EntityScript)
        {
            NativeBindings.ScriptEventException(_target, Member, Exception);
        }
        else
        {
            Interop.LogException(Exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void SetThunk(IntPtr Context, int Field, int Type, double Number, IntPtr Str, int StrLen)
    {
        try
        {
            if (GCHandle.FromIntPtr(Context).Target is UIDataModel Self)
            {
                Self.ApplyFromNative(Field, Number, Str, StrLen);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void EventThunk(IntPtr Context, int CommandId, int ArgCount, UIArg* Args)
    {
        try
        {
            if (GCHandle.FromIntPtr(Context).Target is UIDataModel Self)
            {
                Self.InvokeCommand(CommandId, ArgCount, Args);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    private static readonly IntPtr SetThunkPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, int, int, double, IntPtr, int, void>)&SetThunk;

    private static readonly IntPtr EventThunkPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, int, int, UIArg*, void>)&EventThunk;

    // Called by native once a frame per world, just before that world's UI updates.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void PollUIDataModels(ulong World)
    {
        try
        {
            if (Registry.Count == 0)
            {
                return;
            }
            foreach (UIDataModel Model in new List<UIDataModel>(Registry.Values))
            {
                if (Model._world == World)
                {
                    Model.Poll();
                }
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Drops the models belonging to one world as it tears down, since its Rml context goes with it.
    internal static void RemoveForWorld(ulong World)
    {
        foreach (UIDataModel Model in new List<UIDataModel>(Registry.Values))
        {
            if (Model._world == World)
            {
                Model.Dispose();
            }
        }
    }

    // Before a script load context unloads, since a live model roots the user types it binds.
    public static void DisposeAll()
    {
        foreach (UIDataModel Model in new List<UIDataModel>(Registry.Values))
        {
            Model.Dispose();
        }
        Registry.Clear();
    }

    public void Dispose()
    {
        if (Registry.TryGetValue((_world, _name), out UIDataModel? Registered) && ReferenceEquals(Registered, this))
        {
            Registry.Remove((_world, _name));
        }
        if (_native.IsValid)
        {
            Lumina.CUILibrary.DestroyDataModel(_native);
            _native = default;
        }
        if (_self.IsAllocated)
        {
            _self.Free();
        }
        if (_target is ViewModel Model && ReferenceEquals(Model.Binding, this))
        {
            Model.Binding = null;
        }
    }

    // ---- conversions ----

    private static double ToNumber(object? Value, Lumina.EUIVarType Type)
    {
        if (Value == null)
        {
            return 0.0;
        }
        return Type == Lumina.EUIVarType.Bool ? ((bool)Value ? 1.0 : 0.0) : Convert.ToDouble(Value, CultureInfo.InvariantCulture);
    }

    private static string ToCell(object? Value)
    {
        return Value == null ? string.Empty : (Convert.ToString(Value, CultureInfo.InvariantCulture) ?? string.Empty);
    }

    private static object ConvertNumber(Type Type, double Number)
    {
        if (Type.IsEnum) return Enum.ToObject(Type, (long)Number);
        if (Type == typeof(bool)) return Number != 0.0;
        if (Type == typeof(float)) return (float)Number;
        if (Type == typeof(double)) return Number;
        if (Type == typeof(int)) return (int)Number;
        if (Type == typeof(long)) return (long)Number;
        if (Type == typeof(short)) return (short)Number;
        if (Type == typeof(byte)) return (byte)Number;
        if (Type == typeof(sbyte)) return (sbyte)Number;
        if (Type == typeof(uint)) return (uint)Number;
        if (Type == typeof(ushort)) return (ushort)Number;
        if (Type == typeof(ulong)) return (ulong)Number;
        return Convert.ChangeType(Number, Type, CultureInfo.InvariantCulture);
    }

    private static object? ConvertArg(string Value, Type Type)
    {
        try
        {
            if (Type == typeof(string)) return Value;
            if (Type == typeof(bool)) return Value == "1" || Value.Equals("true", StringComparison.OrdinalIgnoreCase);
            if (Type.IsEnum) return Enum.Parse(Type, Value, true);
            return Convert.ChangeType(Value, Type, CultureInfo.InvariantCulture);
        }
        catch
        {
            return Type.IsValueType ? Activator.CreateInstance(Type) : null;
        }
    }
}
